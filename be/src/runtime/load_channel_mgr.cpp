// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/runtime/load_channel_mgr.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "runtime/load_channel_mgr.h"

#include <memory>

#include "gutil/strings/substitute.h"
#include "runtime/exec_env.h"
#include "runtime/load_channel.h"
#include "runtime/mem_tracker.h"
#include "service/backend_options.h"
#include "storage/lru_cache.h"
#include "util/starrocks_metrics.h"
#include "util/stopwatch.hpp"

namespace starrocks {

// Calculate the memory limit for a single load job.
static int64_t calc_job_max_load_memory(int64_t mem_limit_in_req, int64_t total_mem_limit) {
    // mem_limit_in_req == -1 means no limit for single load.
    // total_mem_limit according to load_process_max_memory_limit_percent calculation
    if (mem_limit_in_req == -1) {
        return std::max<int64_t>(total_mem_limit, config::write_buffer_size);
    }

    int64_t load_mem_limit = std::max<int64_t>(mem_limit_in_req, config::write_buffer_size);
    return std::min<int64_t>(load_mem_limit, total_mem_limit);
}

static int64_t calc_job_timeout_s(int64_t timeout_in_req_s) {
    int64_t load_channel_timeout_s = config::streaming_load_rpc_max_alive_time_sec;
    if (timeout_in_req_s > 0) {
        load_channel_timeout_s = std::max<int64_t>(load_channel_timeout_s, timeout_in_req_s);
    }
    return load_channel_timeout_s;
}

LoadChannelMgr::LoadChannelMgr() : _is_stopped(false) {
    REGISTER_GAUGE_STARROCKS_METRIC(load_channel_count, [this]() {
        std::lock_guard<std::mutex> l(_lock);
        return _load_channels.size();
    });
    _lastest_success_channel = new_lru_cache(1024);
}

LoadChannelMgr::~LoadChannelMgr() {
    _is_stopped.store(true);
    if (_load_channels_clean_thread.joinable()) {
        _load_channels_clean_thread.join();
    }
    delete _lastest_success_channel;
}

Status LoadChannelMgr::init(MemTracker* mem_tracker) {
    _mem_tracker = mem_tracker;
    RETURN_IF_ERROR(_start_bg_worker());
    return Status::OK();
}

Status LoadChannelMgr::open(const PTabletWriterOpenRequest& params) {
    UniqueId load_id(params.id());
    std::shared_ptr<LoadChannel> channel;
    {
        std::lock_guard<std::mutex> l(_lock);
        auto it = _load_channels.find(load_id);
        if (it != _load_channels.end()) {
            channel = it->second;
        } else {
            // create a new load channel
            int64_t mem_limit_in_req = params.has_load_mem_limit() ? params.load_mem_limit() : -1;
            int64_t job_max_memory = calc_job_max_load_memory(mem_limit_in_req, _mem_tracker->limit());

            int64_t timeout_in_req_s = params.has_load_channel_timeout_s() ? params.load_channel_timeout_s() : -1;
            int64_t job_timeout_s = calc_job_timeout_s(timeout_in_req_s);

            channel.reset(new LoadChannel(load_id, job_max_memory, job_timeout_s, _mem_tracker));
            _load_channels.insert({load_id, channel});
        }
    }

    RETURN_IF_ERROR(channel->open(params));
    return Status::OK();
}

static void dummy_deleter(const CacheKey& key, void* value) {}

Status LoadChannelMgr::add_chunk(const PTabletWriterAddChunkRequest& request,
                                 google::protobuf::RepeatedPtrField<PTabletInfo>* tablet_vec,
                                 int64_t* wait_lock_time_ns) {
    UniqueId load_id(request.id());
    // 1. get load channel
    std::shared_ptr<LoadChannel> channel;
    {
        std::lock_guard<std::mutex> l(_lock);
        auto it = _load_channels.find(load_id);
        if (it == _load_channels.end()) {
            auto* handle = _lastest_success_channel->lookup(load_id.to_string());
            // success only when eos be true
            if (handle != nullptr) {
                _lastest_success_channel->release(handle);
                if (request.has_eos() && request.eos()) {
                    return Status::OK();
                }
            }
            return Status::InternalError(
                    strings::Substitute("fail to add batch in load channel. unknown load_id=$0", load_id.to_string()));
        }
        channel = it->second;
    }

    // 2. check if mem consumption exceed limit
    _handle_mem_exceed_limit(channel);

    // 3. add batch to load channel
    // batch may not exist in request(eg: eos request without batch),
    // this case will be handled in load channel's add batch method.
    RETURN_IF_ERROR(channel->add_chunk(request, tablet_vec));

    // 4. handle finish
    if (channel->is_finished()) {
        LOG(INFO) << "Removing finished load channel load id=" << load_id;
        {
            std::lock_guard<std::mutex> l(_lock);
            _load_channels.erase(load_id);
            auto* handle = _lastest_success_channel->insert(load_id.to_string(), nullptr, 1, dummy_deleter);
            _lastest_success_channel->release(handle);
        }
        VLOG(1) << "Removed load channel load id=" << load_id;
    }

    return Status::OK();
}

void LoadChannelMgr::_handle_mem_exceed_limit(const std::shared_ptr<LoadChannel>& data_channel) {
    // lock so that only one thread can check mem limit
    std::lock_guard<std::mutex> l(_lock);
    if (!_mem_tracker->any_limit_exceeded()) {
        return;
    }

    LOG(INFO) << "Reducing memory because total load mem consumption=" << _mem_tracker->consumption()
              << " has exceeded limit=" << _mem_tracker->limit();

    // TODO: ancestors exceeded?
    int64_t exceeded_mem = _mem_tracker->consumption() - _mem_tracker->limit();
    std::vector<FlushTablet> flush_tablets;
    std::set<int64_t> flush_tablet_ids;
    int64_t tablet_mem_consumption;
    do {
        std::shared_ptr<LoadChannel> load_channel;
        std::shared_ptr<TabletsChannel> tablets_channel;
        int64_t tablet_id = -1;
        if (!_find_candidate_load_channel(data_channel, &load_channel)) {
            // should not happen, add log to observe
            LOG(WARNING) << "Fail to find suitable load channel when total load mem limit exceed";
            break;
        }
        DCHECK(load_channel.get() != nullptr);

        // reduce mem usage of the selected load channel
        load_channel->reduce_mem_usage_async(flush_tablet_ids, &tablets_channel, &tablet_id, &tablet_mem_consumption);
        if (tablet_id != -1) {
            flush_tablets.emplace_back(load_channel.get(), tablets_channel.get(), tablet_id);
            flush_tablet_ids.insert(tablet_id);
            exceeded_mem -= tablet_mem_consumption;
            VLOG(3) << "Flush " << *load_channel << ", tablet id=" << tablet_id
                    << ", mem consumption=" << tablet_mem_consumption;
        } else {
            break;
        }
    } while (exceeded_mem > 0);

    // wait flush finish
    for (const FlushTablet& flush_tablet : flush_tablets) {
        Status st = flush_tablet.tablets_channel->wait_mem_usage_reduced(flush_tablet.tablet_id);
        if (!st.ok()) {
            // wait may return failed, but no need to handle it here, just log.
            // tablet_vec will only contains success tablet, and then let FE judge it.
            LOG(WARNING) << "Fail to wait memory reduced. err=" << st.to_string();
        }
    }
    LOG(INFO) << "Reduce memory finish. flush tablets num=" << flush_tablet_ids.size()
              << ", current mem consumption=" << _mem_tracker->consumption() << ", limit=" << _mem_tracker->limit();
}

bool LoadChannelMgr::_find_candidate_load_channel(const std::shared_ptr<LoadChannel>& data_channel,
                                                  std::shared_ptr<LoadChannel>* candidate_channel) {
    // 1. select the load channel that consume this batch data if limit exceeded.
    if (data_channel->mem_limit_exceeded()) {
        *candidate_channel = data_channel;
        return true;
    }

    int64_t max_consume = 0;
    int64_t max_exceeded_consume = 0;
    std::shared_ptr<LoadChannel> max_channel;
    std::shared_ptr<LoadChannel> max_exceeded_channel;
    for (auto& kv : _load_channels) {
        if (kv.second->mem_consumption() > max_consume) {
            max_consume = kv.second->mem_consumption();
            max_channel = kv.second;
        }

        if (kv.second->mem_limit_exceeded() && kv.second->mem_consumption() > max_exceeded_consume) {
            max_exceeded_consume = kv.second->mem_consumption();
            max_exceeded_channel = kv.second;
        }
    }

    // 2. select the largest limit exceeded load channel.
    if (max_exceeded_consume > 0) {
        *candidate_channel = max_exceeded_channel;
        return true;
    }
    // 3. select the largest consumption load channel.
    if (max_consume > 0) {
        *candidate_channel = max_channel;
        return true;
    }
    return false;
}

Status LoadChannelMgr::cancel(const PTabletWriterCancelRequest& params) {
    UniqueId load_id(params.id());
    std::shared_ptr<LoadChannel> cancelled_channel;
    {
        std::lock_guard<std::mutex> l(_lock);
        if (_load_channels.find(load_id) != _load_channels.end()) {
            cancelled_channel = _load_channels[load_id];
            _load_channels.erase(load_id);
        }
    }

    if (cancelled_channel != nullptr) {
        cancelled_channel->cancel();
        LOG(INFO) << "Cancelled load channel load id=" << load_id;
    }

    return Status::OK();
}

Status LoadChannelMgr::_start_bg_worker() {
    _load_channels_clean_thread = std::thread([this] {
#ifdef GOOGLE_PROFILER
        ProfilerRegisterThread();
#endif

#ifndef BE_TEST
        uint32_t interval = 60;
#else
        uint32_t interval = 1;
#endif
        while (!_is_stopped.load()) {
            _start_load_channels_clean();
            sleep(interval);
        }
    });
    return Status::OK();
}

Status LoadChannelMgr::_start_load_channels_clean() {
    std::vector<std::shared_ptr<LoadChannel>> need_delete_channels;
    LOG(INFO) << "Cleaning timed out load channels";
    time_t now = time(nullptr);
    {
        std::vector<UniqueId> need_delete_channel_ids;
        std::lock_guard<std::mutex> l(_lock);
        VLOG(1) << "there are " << _load_channels.size() << " running load channels";
        int i = 0;
        for (auto& kv : _load_channels) {
            VLOG(1) << "load channel[" << i++ << "]: " << *(kv.second);
            time_t last_updated_time = kv.second->last_updated_time();
            if (difftime(now, last_updated_time) >= kv.second->timeout()) {
                need_delete_channel_ids.emplace_back(kv.first);
                need_delete_channels.emplace_back(kv.second);
            }
        }

        for (auto& key : need_delete_channel_ids) {
            _load_channels.erase(key);
            LOG(INFO) << "Erased timeout load channel=" << key;
        }
    }

    // we must cancel these load channels before destroying them.
    // otherwise some object may be invalid before trying to visit it.
    // eg: MemTracker in load channel
    for (auto& channel : need_delete_channels) {
        channel->cancel();
        LOG(INFO) << "Deleted canceled channel load id=" << channel->load_id() << " timeout=" << channel->timeout()
                  << "s";
    }

    // this log print every 1 min, so that we could observe the mem consumption of load process
    // on this Backend
    LOG(INFO) << "Memory consumption(bytes) limit=" << _mem_tracker->limit()
              << " current=" << _mem_tracker->consumption() << " peak=" << _mem_tracker->peak_consumption();

    return Status::OK();
}

} // namespace starrocks

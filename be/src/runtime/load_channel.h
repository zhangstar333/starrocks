// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/runtime/load_channel.h

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

#pragma once

#include <mutex>
#include <ostream>
#include <unordered_map>
#include <unordered_set>

#include "common/status.h"
#include "gen_cpp/InternalService_types.h"
#include "gen_cpp/Types_types.h"
#include "gen_cpp/internal_service.pb.h"
#include "runtime/mem_tracker.h"
#include "util/uid_util.h"

namespace starrocks {

class Cache;
class TabletsChannel;
class LoadChannel;

// Tablet info which memtables submitted to flush queue when load channel memory exceeds limit.
struct FlushTablet {
    LoadChannel* load_channel;
    TabletsChannel* tablets_channel;
    int64_t tablet_id;
    FlushTablet(LoadChannel* load_channel, TabletsChannel* tablets_channel, int64_t tablet_id)
            : load_channel(load_channel), tablets_channel(tablets_channel), tablet_id(tablet_id) {}
};

// A LoadChannel manages tablets channels for all indexes
// corresponding to a certain load job
class LoadChannel {
public:
    LoadChannel(const UniqueId& load_id, int64_t mem_limit, int64_t timeout_s, MemTracker* mem_tracker);
    ~LoadChannel();

    // open a new load channel if not exist
    Status open(const PTabletWriterOpenRequest& request);

    Status add_chunk(const PTabletWriterAddChunkRequest& request,
                     google::protobuf::RepeatedPtrField<PTabletInfo>* tablet_vec);

    // return true if this load channel has been opened and all tablets channels are closed then.
    bool is_finished();

    Status cancel();

    time_t last_updated_time() const { return _last_updated_time.load(); }

    const UniqueId& load_id() const { return _load_id; }

    // reduce memory with lock, for handle mem exceed limit in load channel mgr.
    void reduce_mem_usage_async(const std::set<int64_t>& flush_tablet_ids,
                                std::shared_ptr<TabletsChannel>* tablets_channel, int64_t* tablet_id,
                                int64_t* tablet_mem_consumption);

    int64_t mem_consumption() const { return _mem_tracker->consumption(); }
    bool mem_limit_exceeded() const { return _mem_tracker->limit_exceeded(); }

    int64_t timeout() const { return _timeout_s; }

private:
    // check if this load channel mem consumption exceeds limit.
    // If yes, it will pick several tablets channels to try to reduce memory consumption to limit.
    void _handle_mem_exceed_limit();

    // reduce memory internal without lock, for handle mem exceed limit in load channel
    void _reduce_mem_usage_async_internal(const std::set<int64_t>& flush_tablet_ids,
                                          std::shared_ptr<TabletsChannel>* tablets_channel, int64_t* tablet_id,
                                          int64_t* tablet_mem_consumption);

    // when mem consumption exceeds limit, should call this method to find the channel
    // that consumes the largest memory(, and then we can reduce its memory usage).
    bool _find_largest_consumption_channel(std::shared_ptr<TabletsChannel>* channel);

    UniqueId _load_id;
    // Tracks the total memory comsupted by current load job on this BE
    std::unique_ptr<MemTracker> _mem_tracker;

    // lock protect the tablets channel map
    std::mutex _lock;
    // index id -> tablets channel
    std::unordered_map<int64_t, std::shared_ptr<TabletsChannel>> _tablets_channels;
    // This is to save finished channels id, to handle the retry request.
    std::unordered_set<int64_t> _finished_channel_ids;
    // set to true if at least one tablets channel has been opened
    bool _opened = false;

    std::atomic<time_t> _last_updated_time;

    // the timeout of this load job.
    // Timed out channels will be periodically deleted by LoadChannelMgr.
    int64_t _timeout_s;
};

inline std::ostream& operator<<(std::ostream& os, const LoadChannel& load_channel) {
    os << "LoadChannel(id=" << load_channel.load_id() << ", mem=" << load_channel.mem_consumption()
       << ", last_update_time=" << static_cast<uint64_t>(load_channel.last_updated_time()) << ")";
    return os;
}

} // namespace starrocks

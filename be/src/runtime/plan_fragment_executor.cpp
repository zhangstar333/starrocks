// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/runtime/plan_fragment_executor.cpp

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

#include "runtime/plan_fragment_executor.h"

#include <memory>
#include <utility>

#include "common/logging.h"
#include "common/object_pool.h"
#include "exec/data_sink.h"
#include "exec/exchange_node.h"
#include "exec/exec_node.h"
#include "exec/scan_node.h"
#include "exprs/expr.h"
#include "gutil/map_util.h"
#include "runtime/current_thread.h"
#include "runtime/data_stream_mgr.h"
#include "runtime/descriptors.h"
#include "runtime/exec_env.h"
#include "runtime/mem_tracker.h"
#include "runtime/result_buffer_mgr.h"
#include "runtime/result_queue_mgr.h"
#include "runtime/row_batch.h"
#include "runtime/runtime_filter_worker.h"
#include "util/parse_util.h"
#include "util/pretty_printer.h"
#include "util/uid_util.h"

namespace starrocks {

PlanFragmentExecutor::PlanFragmentExecutor(ExecEnv* exec_env, report_status_callback report_status_cb)
        : _exec_env(exec_env),
          _report_status_cb(std::move(report_status_cb)),
          _done(false),
          _prepared(false),
          _closed(false),
          _has_thread_token(false),
          _is_report_success(true),
          _is_report_on_cancel(true),
          _collect_query_statistics_with_every_batch(false),
          _is_runtime_filter_merge_node(false) {}

PlanFragmentExecutor::~PlanFragmentExecutor() {
    close();
}

Status PlanFragmentExecutor::prepare(const TExecPlanFragmentParams& request) {
    const TPlanFragmentExecParams& params = request.params;
    _query_id = params.query_id;

    LOG(INFO) << "Prepare(): query_id=" << print_id(_query_id)
              << " fragment_instance_id=" << print_id(params.fragment_instance_id)
              << " backend_num=" << request.backend_num;

    if (_is_vectorized) {
        _runtime_state->set_batch_size(config::vector_chunk_size);
    }

    _runtime_state->set_be_number(request.backend_num);
    if (request.__isset.import_label) {
        _runtime_state->set_import_label(request.import_label);
    }
    if (request.__isset.db_name) {
        _runtime_state->set_db_name(request.db_name);
    }
    if (request.__isset.load_job_id) {
        _runtime_state->set_load_job_id(request.load_job_id);
    }
    if (request.__isset.load_error_hub_info) {
        _runtime_state->set_load_error_hub_info(request.load_error_hub_info);
    }

    if (request.query_options.__isset.is_report_success) {
        _is_report_success = request.query_options.is_report_success;
    }

    // Reserve one main thread from the pool
    _runtime_state->resource_pool()->acquire_thread_token();
    _has_thread_token = true;

    _average_thread_tokens = profile()->add_sampling_counter(
            "AverageThreadTokens", std::bind<int64_t>(std::mem_fn(&ThreadResourceMgr::ResourcePool::num_threads),
                                                      _runtime_state->resource_pool()));

    int64_t bytes_limit = request.query_options.mem_limit;
    if (bytes_limit <= 0) {
        // sometimes the request does not set the query mem limit, we use default one.
        // TODO(cmy): we should not allow request without query mem limit.
        bytes_limit = 2 * 1024 * 1024 * 1024L;
    }

    if (bytes_limit > _exec_env->query_pool_mem_tracker()->limit()) {
        LOG(WARNING) << "Query memory limit " << PrettyPrinter::print(bytes_limit, TUnit::BYTES)
                     << " exceeds process memory limit of "
                     << PrettyPrinter::print(_exec_env->query_pool_mem_tracker()->limit(), TUnit::BYTES)
                     << ". Using process memory limit instead";
        bytes_limit = _exec_env->query_pool_mem_tracker()->limit();
    }

    LOG(INFO) << "Using query memory limit: " << PrettyPrinter::print(bytes_limit, TUnit::BYTES);

    // set up desc tbl
    DescriptorTbl* desc_tbl = nullptr;
    DCHECK(request.__isset.desc_tbl);
    RETURN_IF_ERROR(DescriptorTbl::create(obj_pool(), request.desc_tbl, &desc_tbl));
    _runtime_state->set_desc_tbl(desc_tbl);

    // set up plan
    DCHECK(request.__isset.fragment);
    RETURN_IF_ERROR(ExecNode::create_tree(_runtime_state, obj_pool(), request.fragment.plan, *desc_tbl, &_plan));
    _runtime_state->set_fragment_root_id(_plan->id());

    if (request.params.__isset.debug_node_id) {
        DCHECK(request.params.__isset.debug_action);
        DCHECK(request.params.__isset.debug_phase);
        ExecNode::set_debug_options(request.params.debug_node_id, request.params.debug_phase,
                                    request.params.debug_action, _plan);
    }

    if (request.fragment.__isset.query_global_dicts) {
        RETURN_IF_ERROR(_runtime_state->init_query_global_dict(request.fragment.query_global_dicts));
    }

    // set #senders of exchange nodes before calling Prepare()
    std::vector<ExecNode*> exch_nodes;
    _plan->collect_nodes(TPlanNodeType::EXCHANGE_NODE, &exch_nodes);
    for (auto* exch_node : exch_nodes) {
        DCHECK_EQ(exch_node->type(), TPlanNodeType::EXCHANGE_NODE);
        int num_senders = FindWithDefault(params.per_exch_num_senders, exch_node->id(), 0);
        DCHECK_GT(num_senders, 0);
        static_cast<ExchangeNode*>(exch_node)->set_num_senders(num_senders);
    }

    // when has adapter node, set the batch_size with the config::vector_chunk_size
    // otherwise the adapter node will crash when convert
    std::vector<ExecNode*> adaptor_nodes;
    _plan->collect_nodes(TPlanNodeType::ADAPTER_NODE, &adaptor_nodes);
    if (!adaptor_nodes.empty()) {
        _runtime_state->set_batch_size(config::vector_chunk_size);
    }

    RETURN_IF_ERROR(_plan->prepare(_runtime_state));
    // set scan ranges
    std::vector<ExecNode*> scan_nodes;
    std::vector<TScanRangeParams> no_scan_ranges;
    _plan->collect_scan_nodes(&scan_nodes);
    VLOG(1) << "scan_nodes.size()=" << scan_nodes.size();
    VLOG(1) << "params.per_node_scan_ranges.size()=" << params.per_node_scan_ranges.size();

    for (auto& i : scan_nodes) {
        ScanNode* scan_node = down_cast<ScanNode*>(i);
        const std::vector<TScanRangeParams>& scan_ranges =
                FindWithDefault(params.per_node_scan_ranges, scan_node->id(), no_scan_ranges);
        scan_node->set_scan_ranges(scan_ranges);
        VLOG(1) << "scan_node_Id=" << scan_node->id() << " size=" << scan_ranges.size();
    }

    _runtime_state->set_per_fragment_instance_idx(params.sender_id);
    _runtime_state->set_num_per_fragment_instances(params.num_senders);

    // set up sink, if required
    if (request.fragment.__isset.output_sink) {
        RETURN_IF_ERROR(DataSink::create_data_sink(obj_pool(), request.fragment.output_sink,
                                                   request.fragment.output_exprs, params, row_desc(), &_sink));
        RETURN_IF_ERROR(_sink->prepare(runtime_state()));

        RuntimeProfile* sink_profile = _sink->profile();

        if (sink_profile != nullptr) {
            profile()->add_child(sink_profile, true, nullptr);
        }

        _collect_query_statistics_with_every_batch = params.__isset.send_query_statistics_with_every_batch
                                                             ? params.send_query_statistics_with_every_batch
                                                             : false;
    } else {
        // _sink is set to NULL
        _sink.reset(nullptr);
    }

    // set up profile counters
    profile()->add_child(_plan->runtime_profile(), true, nullptr);
    _rows_produced_counter = ADD_COUNTER(profile(), "RowsProduced", TUnit::UNIT);

    VLOG(3) << "plan_root=\n" << _plan->debug_string();
    _chunk = std::make_shared<vectorized::Chunk>();
    _prepared = true;

    _query_statistics.reset(new QueryStatistics());
    if (_sink != nullptr) {
        _sink->set_query_statistics(_query_statistics);
    }

    if (params.__isset.runtime_filter_params && params.runtime_filter_params.id_to_prober_params.size() != 0) {
        _is_runtime_filter_merge_node = true;
        _exec_env->runtime_filter_worker()->open_query(_query_id, request.query_options, params.runtime_filter_params);
    }

    return Status::OK();
}

Status PlanFragmentExecutor::open() {
    LOG(INFO) << "Open(): fragment_instance_id=" << print_id(_runtime_state->fragment_instance_id());
    tls_thread_status.set_query_id(_runtime_state->query_id());

    Status status = _open_internal_vectorized();
    if (!status.ok() && !status.is_cancelled() && _runtime_state->log_has_space()) {
        LOG(WARNING) << "fail to open fragment, instance_id=" << print_id(_runtime_state->fragment_instance_id())
                     << ", status=" << status.to_string();
        // Log error message in addition to returning in Status. Queries that do not
        // fetch results (e.g. insert) may not receive the message directly and can
        // only retrieve the log.
        _runtime_state->log_error(status.get_error_msg());
    }

    update_status(status);
    return status;
}

Status PlanFragmentExecutor::_open_internal_vectorized() {
    {
        SCOPED_TIMER(profile()->total_time_counter());
        RETURN_IF_ERROR(_plan->open(_runtime_state));
    }

    if (_sink == nullptr) {
        return Status::OK();
    }
    RETURN_IF_ERROR(_sink->open(runtime_state()));

    // If there is a sink, do all the work of driving it here, so that
    // when this returns the query has actually finished
    vectorized::ChunkPtr chunk;
    while (true) {
        RETURN_IF_ERROR(runtime_state()->check_mem_limit("QUERY"));

        RETURN_IF_ERROR(_get_next_internal_vectorized(&chunk));

        if (chunk == nullptr) {
            break;
        }

        if (VLOG_ROW_IS_ON) {
            VLOG_ROW << "_open_internal_vectorized: #rows=" << chunk->num_rows()
                     << " desc=" << row_desc().debug_string();
            // TODO(kks): support chunk debug log
        }

        SCOPED_TIMER(profile()->total_time_counter());
        // Collect this plan and sub plan statisticss, and send to parent plan.
        if (_collect_query_statistics_with_every_batch) {
            collect_query_statistics();
        }
        RETURN_IF_ERROR(_sink->send_chunk(runtime_state(), chunk.get()));
    }

    // Close the sink *before* stopping the report thread. Close may
    // need to add some important information to the last report that
    // gets sent. (e.g. table sinks record the files they have written
    // to in this method)
    // The coordinator report channel waits until all backends are
    // either in error or have returned a status report with done =
    // true, so tearing down any data stream state (a separate
    // channel) in Close is safe.

    // TODO: If this returns an error, the d'tor will call Close again. We should
    // audit the sinks to check that this is ok, or change that behaviour.
    Status close_status;
    {
        SCOPED_TIMER(profile()->total_time_counter());
        collect_query_statistics();
        Status status;
        {
            std::lock_guard<std::mutex> l(_status_lock);
            status = _status;
        }
        close_status = _sink->close(runtime_state(), status);
    }

    update_status(close_status);
    // Setting to NULL ensures that the d'tor won't double-close the sink.
    _sink.reset(nullptr);
    _done = true;

    release_thread_token();

    send_report(true);

    return close_status;
}

void PlanFragmentExecutor::collect_query_statistics() {
    _query_statistics->clear();
    _plan->collect_query_statistics(_query_statistics.get());
}

void PlanFragmentExecutor::send_report(bool done) {
    if (!_report_status_cb) {
        return;
    }

    Status status;
    {
        std::lock_guard<std::mutex> l(_status_lock);
        status = _status;
    }

    // If plan is done successfully, but _is_report_success is false,
    // no need to send report.
    if (!_is_report_success && done && status.ok()) {
        return;
    }

    // If both _is_report_success and _is_report_on_cancel are false,
    // which means no matter query is success or failed, no report is needed.
    // This may happen when the query limit reached and
    // a internal cancellation being processed
    if (!_is_report_success && !_is_report_on_cancel) {
        return;
    }

    // This will send a report even if we are cancelled.  If the query completed correctly
    // but fragments still need to be cancelled (e.g. limit reached), the coordinator will
    // be waiting for a final report and profile.
    _report_status_cb(status, profile(), done || !status.ok());
}

Status PlanFragmentExecutor::get_next(vectorized::ChunkPtr* chunk) {
    VLOG_FILE << "GetNext(): instance_id=" << _runtime_state->fragment_instance_id();
    Status status = _get_next_internal_vectorized(chunk);
    update_status(status);

    if (_done) {
        LOG(INFO) << "Finished executing fragment query_id=" << print_id(_query_id)
                  << " instance_id=" << print_id(_runtime_state->fragment_instance_id());
        // Query is done, return the thread token
        release_thread_token();
        send_report(true);
    }

    return status;
}

Status PlanFragmentExecutor::_get_next_internal_vectorized(vectorized::ChunkPtr* chunk) {
    // If there is a empty chunk, we continue to read next chunk
    // If we set chunk to nullptr, means this fragment read done
    while (!_done) {
        SCOPED_TIMER(profile()->total_time_counter());
        RETURN_IF_ERROR(_plan->get_next(_runtime_state, &_chunk, &_done));
        if (_done) {
            *chunk = nullptr;
            return Status::OK();
        } else if (_chunk->num_rows() > 0) {
            COUNTER_UPDATE(_rows_produced_counter, _chunk->num_rows());
            *chunk = _chunk;
            return Status::OK();
        }
    }

    return Status::OK();
}

void PlanFragmentExecutor::update_status(const Status& new_status) {
    if (new_status.ok()) {
        return;
    }

    {
        std::lock_guard<std::mutex> l(_status_lock);
        // if current `_status` is ok, set it to `new_status` to record the error.
        if (_status.ok()) {
            if (new_status.is_mem_limit_exceeded()) {
                _runtime_state->set_mem_limit_exceeded(new_status.get_error_msg());
            }
            _status = new_status;
            if (_runtime_state->query_options().query_type == TQueryType::EXTERNAL) {
                TUniqueId fragment_instance_id = _runtime_state->fragment_instance_id();
                _exec_env->result_queue_mgr()->update_queue_status(fragment_instance_id, new_status);
            }
        }
    }

    send_report(true);
}

void PlanFragmentExecutor::cancel() {
    LOG(INFO) << "cancel(): fragment_instance_id=" << print_id(_runtime_state->fragment_instance_id());
    DCHECK(_prepared);
    _runtime_state->set_is_cancelled(true);
    _runtime_state->exec_env()->stream_mgr()->cancel(_runtime_state->fragment_instance_id());
    _runtime_state->exec_env()->result_mgr()->cancel(_runtime_state->fragment_instance_id());

    if (_is_runtime_filter_merge_node) {
        _runtime_state->exec_env()->runtime_filter_worker()->close_query(_query_id);
    }
}

const RowDescriptor& PlanFragmentExecutor::row_desc() {
    return _plan->row_desc();
}

RuntimeProfile* PlanFragmentExecutor::profile() {
    return _runtime_state->runtime_profile();
}

void PlanFragmentExecutor::report_profile_once() {
    if (_done) return;

    if (VLOG_FILE_IS_ON) {
        VLOG_FILE << "Reporting profile for instance " << _runtime_state->fragment_instance_id();
        std::stringstream ss;
        profile()->compute_time_in_profile();
        profile()->pretty_print(&ss);
        VLOG_FILE << ss.str();
    }

    send_report(false);
}

void PlanFragmentExecutor::release_thread_token() {
    if (_has_thread_token) {
        _has_thread_token = false;
        _runtime_state->resource_pool()->release_thread_token(true);
        profile()->stop_sampling_counters_updates(_average_thread_tokens);
    }
}

void PlanFragmentExecutor::close() {
    if (_closed) {
        return;
    }

    _chunk.reset();

    if (_is_runtime_filter_merge_node) {
        _exec_env->runtime_filter_worker()->close_query(_query_id);
    }

    // Prepare may not have been called, which sets _runtime_state
    if (_runtime_state != nullptr) {
        // _runtime_state init failed
        if (_plan != nullptr) {
            _plan->close(_runtime_state);
        }

        if (_sink != nullptr) {
            if (_prepared) {
                Status status;
                {
                    std::lock_guard<std::mutex> l(_status_lock);
                    status = _status;
                }
                _sink->close(runtime_state(), status);
            } else {
                _sink->close(runtime_state(), Status::InternalError("prepare failed"));
            }
        }

        if (FLAGS_minloglevel == 0 /*INFO*/) {
            std::stringstream ss;
            // Compute the _local_time_percent before pretty_print the runtime_profile
            // Before add this operation, the print out like that:
            // UNION_NODE (id=0):(Active: 56.720us, non-child: 00.00%)
            // After add thie operation, the print out like that:
            // UNION_NODE (id=0):(Active: 56.720us, non-child: 82.53%)
            // We can easily know the exec node excute time without child time consumed.
            _runtime_state->runtime_profile()->compute_time_in_profile();
            _runtime_state->runtime_profile()->pretty_print(&ss);
            LOG(INFO) << ss.str();
        }
    }

    _closed = true;
}

} // namespace starrocks

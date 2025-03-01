// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks
// Limited.

#include "exec/pipeline/pipeline_driver.h"

#include <sstream>

#include "column/chunk.h"
#include "exec/pipeline/source_operator.h"
#include "runtime/exec_env.h"
#include "runtime/runtime_state.h"

namespace starrocks::pipeline {
Status PipelineDriver::prepare(RuntimeState* runtime_state) {
    DCHECK(_state == DriverState::NOT_READY);
    // fill OperatorWithDependency instances into _dependencies from _operators.
    DCHECK(_dependencies.empty());
    _dependencies.reserve(_operators.size());
    for (auto& op : _operators) {
        if (auto* op_with_dep = dynamic_cast<DriverDependencyPtr>(op.get())) {
            _dependencies.push_back(op_with_dep);
        }
    }
    source_operator()->add_morsel_queue(_morsel_queue);
    for (auto& op : _operators) {
        RETURN_IF_ERROR(op->prepare(runtime_state));
        _operator_stages[op->get_id()] = OperatorStage::PREPARED;
    }
    // Driver has no dependencies always sets _all_dependencies_ready to true;
    _all_dependencies_ready = _dependencies.empty();
    _state = DriverState::READY;

    _total_timer = ADD_TIMER(_runtime_profile, "DriverTotalTime");
    _active_timer = ADD_TIMER(_runtime_profile, "DriverActiveTime");
    _pending_timer = ADD_TIMER(_runtime_profile, "DriverPendingTime");

    _total_timer_sw = runtime_state->obj_pool()->add(new MonotonicStopWatch());
    _pending_timer_sw = runtime_state->obj_pool()->add(new MonotonicStopWatch());
    _total_timer_sw->start();
    _pending_timer_sw->start();

    return Status::OK();
}

StatusOr<DriverState> PipelineDriver::process(RuntimeState* runtime_state) {
    SCOPED_TIMER(_active_timer);
    _state = DriverState::RUNNING;
    size_t total_chunks_moved = 0;
    int64_t time_spent = 0;
    while (true) {
        RETURN_IF_LIMIT_EXCEEDED(runtime_state, "Pipeline");

        size_t num_chunk_moved = 0;
        bool should_yield = false;
        size_t num_operators = _operators.size();
        size_t _new_first_unfinished = _first_unfinished;
        for (size_t i = _first_unfinished; i < num_operators - 1; ++i) {
            {
                SCOPED_RAW_TIMER(&time_spent);
                auto& curr_op = _operators[i];
                auto& next_op = _operators[i + 1];

                // Check curr_op finished firstly
                if (curr_op->is_finished()) {
                    if (i == 0) {
                        // For source operators
                        _mark_operator_finishing(curr_op, runtime_state);
                    }
                    _mark_operator_finishing(next_op, runtime_state);
                    _new_first_unfinished = i + 1;
                    continue;
                }

                // try successive operator pairs
                if (!curr_op->has_output() || !next_op->need_input()) {
                    continue;
                }

                if (_check_fragment_is_canceled(runtime_state)) {
                    return _state;
                }

                // pull chunk from current operator and push the chunk onto next
                // operator
                StatusOr<vectorized::ChunkPtr> maybe_chunk;
                {
                    SCOPED_TIMER(curr_op->_pull_timer);
                    maybe_chunk = curr_op->pull_chunk(runtime_state);
                }
                auto status = maybe_chunk.status();
                if (!status.ok() && !status.is_end_of_file()) {
                    LOG(WARNING) << " status " << status.to_string();
                    return status;
                }

                if (_check_fragment_is_canceled(runtime_state)) {
                    return _state;
                }

                if (status.ok()) {
                    COUNTER_UPDATE(curr_op->_pull_chunk_num_counter, 1);
                    if (maybe_chunk.value() && maybe_chunk.value()->num_rows() > 0) {
                        size_t row_num = maybe_chunk.value()->num_rows();
                        {
                            SCOPED_TIMER(next_op->_push_timer);
                            next_op->push_chunk(runtime_state, maybe_chunk.value());
                        }
                        COUNTER_UPDATE(curr_op->_pull_row_num_counter, row_num);
                        COUNTER_UPDATE(next_op->_push_chunk_num_counter, 1);
                        COUNTER_UPDATE(next_op->_push_row_num_counter, row_num);
                    }
                    num_chunk_moved += 1;
                    total_chunks_moved += 1;
                }

                // Check curr_op finished again
                if (curr_op->is_finished()) {
                    if (i == 0) {
                        // For source operators
                        _mark_operator_finishing(curr_op, runtime_state);
                    }
                    _mark_operator_finishing(next_op, runtime_state);
                    _new_first_unfinished = i + 1;
                    continue;
                }
            }
            // yield when total chunks moved or time spent on-core for evaluation
            // exceed the designated thresholds.
            if (total_chunks_moved >= _yield_max_chunks_moved || time_spent >= _yield_max_time_spent) {
                should_yield = true;
                break;
            }
        }
        // close finished operators and update _first_unfinished index
        for (auto i = _first_unfinished; i < _new_first_unfinished; ++i) {
            _mark_operator_finished(_operators[i], runtime_state);
        }
        _first_unfinished = _new_first_unfinished;

        if (sink_operator()->is_finished()) {
            finish_operators(runtime_state);
            _state = source_operator()->pending_finish() ? DriverState::PENDING_FINISH : DriverState::FINISH;
            return _state;
        }

        // no chunk moved in current round means that the driver is blocked.
        // should yield means that the CPU core is occupied the driver for a
        // very long time so that the driver should switch off the core and
        // give chance for another ready driver to run.
        if (num_chunk_moved == 0 || should_yield) {
            driver_acct().increment_schedule_times();
            driver_acct().update_last_chunks_moved(total_chunks_moved);
            driver_acct().update_last_time_spent(time_spent);
            if (dependencies_block()) {
                _state = DriverState::DEPENDENCIES_BLOCK;
                return DriverState::DEPENDENCIES_BLOCK;
            } else if (!sink_operator()->is_finished() && !sink_operator()->need_input()) {
                _state = DriverState::OUTPUT_FULL;
                return DriverState::OUTPUT_FULL;
            }
            if (!source_operator()->is_finished() && !source_operator()->has_output()) {
                _state = DriverState::INPUT_EMPTY;
                return DriverState::INPUT_EMPTY;
            }
            _state = DriverState::READY;
            return DriverState::READY;
        }
    }
}

void PipelineDriver::dispatch_operators() {
    for (auto& op : _operators) {
        _operator_stages[op->get_id()] = OperatorStage::PROCESSING;
    }
}

void PipelineDriver::finish_operators(RuntimeState* runtime_state) {
    for (auto& op : _operators) {
        _mark_operator_finished(op, runtime_state);
    }
}
void PipelineDriver::_close_operators(RuntimeState* runtime_state) {
    for (auto& op : _operators) {
        _mark_operator_closed(op, runtime_state);
    }
}

void PipelineDriver::finalize(RuntimeState* runtime_state, DriverState state) {
    VLOG_ROW << "[Driver] finalize, driver=" << this;
    DCHECK(state == DriverState::FINISH || state == DriverState::CANCELED || state == DriverState::INTERNAL_ERROR);

    _close_operators(runtime_state);

    _state = state;

    // Calculate total time before report profile
    _total_timer->update(_total_timer_sw->elapsed_time());

    // last root driver cancel the all drivers' execution and notify FE the
    // fragment's completion but do not unregister the FragmentContext because
    // some non-root drivers maybe has pending io io tasks hold the reference to
    // object owned by FragmentContext.
    if (is_root()) {
        if (_fragment_ctx->count_down_root_drivers()) {
            _fragment_ctx->finish();
            auto status = _fragment_ctx->final_status();
            _fragment_ctx->runtime_state()->exec_env()->driver_dispatcher()->report_exec_state(_fragment_ctx, status,
                                                                                               true);
        }
    }
    // last finished driver notify FE the fragment's completion again and
    // unregister the FragmentContext.
    if (_fragment_ctx->count_down_drivers()) {
        auto status = _fragment_ctx->final_status();
        auto fragment_id = _fragment_ctx->fragment_instance_id();
        VLOG_ROW << "[Driver] Last driver finished: final_status=" << status.to_string();
        _query_ctx->count_down_fragments();
    }
}

std::string PipelineDriver::to_readable_string() const {
    std::stringstream ss;
    ss << "operator-chain: [";
    for (size_t i = 0; i < _operators.size(); ++i) {
        if (i == 0) {
            ss << _operators[i]->get_name();
        } else {
            ss << " -> " << _operators[i]->get_name();
        }
    }
    ss << "]";
    return ss.str();
}

bool PipelineDriver::_check_fragment_is_canceled(RuntimeState* runtime_state) {
    if (_fragment_ctx->is_canceled()) {
        finish_operators(runtime_state);
        // If the fragment is cancelled after the source operator commits an i/o task to i/o threads,
        // the driver cannot be finished immediately and should wait for the completion of the pending i/o task.
        if (source_operator()->pending_finish()) {
            _state = DriverState::PENDING_FINISH;
        } else {
            _state = _fragment_ctx->final_status().ok() ? DriverState::FINISH : DriverState::CANCELED;
        }
        return true;
    }
    return false;
}

void PipelineDriver::_mark_operator_finishing(OperatorPtr& op, RuntimeState* state) {
    auto& op_state = _operator_stages[op->get_id()];
    if (op_state >= OperatorStage::FINISHING) {
        return;
    }
    op->set_finishing(state);
    op_state = OperatorStage::FINISHING;
}

void PipelineDriver::_mark_operator_finished(OperatorPtr& op, RuntimeState* state) {
    _mark_operator_finishing(op, state);
    auto& op_state = _operator_stages[op->get_id()];
    if (op_state >= OperatorStage::FINISHED) {
        return;
    }
    op->set_finished(state);
    op_state = OperatorStage::FINISHED;
}

void PipelineDriver::_mark_operator_closed(OperatorPtr& op, RuntimeState* state) {
    _mark_operator_finished(op, state);
    auto& op_state = _operator_stages[op->get_id()];
    if (op_state >= OperatorStage::CLOSED) {
        return;
    }
    op->close(state);
    op_state = OperatorStage::CLOSED;
}

} // namespace starrocks::pipeline

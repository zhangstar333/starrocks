// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include "common/status.h"
#include "runtime/runtime_state.h"

namespace starrocks::pipeline {

// The complex ExecNode is usually decomposed to multiple operators,
// which share some variables in a context, and have dependency relationships between them.
// (For example, HashJoinNode is composed to HashJoinBuildOperator and HashJoinProbeOperator,
// which share a HashJoiner context. And HashJoinProbeOperator depends on HashJoinBuildOperator.)
// Assume OpA depends on OpB.
// However, OpA can be finished earlier than OpB, if it's successor operator is finished early.
// Therefore, there are two problems which needs to be considered:
// 1. OpB should perceive that OpA has been finished early.
// 2. Context object can be released only if both OpA and OpB are closed,
//    because the close order of OpA and OpB are uncertain.
class ContextWithDependency {
public:
    ContextWithDependency() = default;
    ~ContextWithDependency() = default;

    ContextWithDependency(const ContextWithDependency&) = delete;
    ContextWithDependency(ContextWithDependency&&) = delete;
    ContextWithDependency& operator=(ContextWithDependency&&) = delete;
    ContextWithDependency& operator=(const ContextWithDependency&) = delete;

    // For pipeline, it is called by unref() when the last operator is unreffed.
    // For non-pipeline, it is called by close() of the exec node directly
    // without calling ref and unref. For example, AggregateBaseNode uses Aggregator.
    virtual Status close(RuntimeState* state) = 0;

    // ref and unref are used to close context
    // when the last running related operator is closed.
    // - ref is called by operator::constructor() at the preparation stage.
    // - unref is called by operator::close() at the close stage.
    // It is the guaranteed by the dispatcher queue that the increment operations
    // by ref() are visible to unref(), so we needn't barrier here.
    void ref() { _num_running_operators.fetch_add(1, std::memory_order_relaxed); }

    // Called by operator::close. Close the context when the last running operator is closed.
    Status unref(RuntimeState* state) {
        if (_num_running_operators.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            return close(state);
        }
        return Status::OK();
    }

    // When the output operator is finished, the context can be finished regardless of other running operators.
    void set_finished() { _is_finished.store(true, std::memory_order_release); }

    // Predicate whether the context is finished, which is used to notify
    // non-output operators to be finished early.
    bool is_finished() const { return _is_finished.load(std::memory_order_acquire); }

private:
    std::atomic<int32_t> _num_running_operators = 0;
    std::atomic<bool> _is_finished = false;
};

} // namespace starrocks::pipeline
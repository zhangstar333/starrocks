// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include "exec/pipeline/operator.h"
#include "exec/pipeline/set/intersect_context.h"

namespace starrocks::pipeline {

// IntersectNode is decomposed to IntersectBuildSinkOperator, IntersectProbeSinkOperator, and IntersectOutputSourceOperator.
// - IntersectBuildSinkOperator builds a hast set from the IntersectNode's first child.
// - Each IntersectProbeSinkOperator probes the hash set built by IntersectBuildSinkOperator.
//   The hash set records the ordinal of IntersectProbeSinkOperator per key if the key is hit.
// - IntersectOutputSourceOperator traverses the hast set and picks up entries hit by all
//   IntersectProbeSinkOperators after probe phase is finished.
//
// IntersectBuildSinkOperator, IntersectProbeSinkOperator, and IntersectOutputSourceOperator
// belong to different pipelines. There is dependency between them:
// - The first IntersectProbeSinkOperator depends on IntersectBuildSinkOperator.
// - The rest IntersectProbeSinkOperator depends on the prev IntersectProbeSinkOperator.
// - IntersectOutputSourceOperator depends on the last IntersectProbeSinkOperator.
// The execution sequence is as follows: IntersectBuildSinkOperator -> IntersectProbeSinkOperator 0
// -> IntersectProbeSinkOperator 1 -> ... -> IntersectProbeSinkOperator N -> IntersectOutputSourceOperator.
//
// The rows are shuffled to degree of parallelism (DOP) partitions by local shuffle exchange.
// For each partition, there are a IntersectBuildSinkOperator driver, a IntersectProbeSinkOperator driver
// for each child, and a IntersectOutputSourceOperator.
class IntersectBuildSinkOperator final : public Operator {
public:
    IntersectBuildSinkOperator(int32_t id, int32_t plan_node_id, std::shared_ptr<IntersectContext> intersect_ctx,
                               const std::vector<ExprContext*>& dst_exprs)
            : Operator(id, "intersect_build_sink", plan_node_id),
              _intersect_ctx(std::move(intersect_ctx)),
              _dst_exprs(dst_exprs) {
        _intersect_ctx->ref();
    }

    bool need_input() const override { return !is_finished(); }

    bool has_output() const override { return false; }

    bool is_finished() const override { return _is_finished || _intersect_ctx->is_finished(); }

    void set_finishing(RuntimeState* state) override {
        _is_finished = true;
        _intersect_ctx->finish_build_ht();
    }

    Status prepare(RuntimeState* state) override;
    Status close(RuntimeState* state) override;

    StatusOr<vectorized::ChunkPtr> pull_chunk(RuntimeState* state) override;

    Status push_chunk(RuntimeState* state, const vectorized::ChunkPtr& chunk) override;

private:
    std::shared_ptr<IntersectContext> _intersect_ctx;

    const std::vector<ExprContext*>& _dst_exprs;

    bool _is_finished = false;
};

class IntersectBuildSinkOperatorFactory final : public OperatorFactory {
public:
    IntersectBuildSinkOperatorFactory(int32_t id, int32_t plan_node_id,
                                      IntersectPartitionContextFactoryPtr intersect_partition_ctx_factory,
                                      const std::vector<ExprContext*>& dst_exprs)
            : OperatorFactory(id, "intersect_build_sink", plan_node_id),
              _intersect_partition_ctx_factory(std::move(intersect_partition_ctx_factory)),
              _dst_exprs(dst_exprs) {}

    OperatorPtr create(int32_t degree_of_parallelism, int32_t driver_sequence) override {
        return std::make_shared<IntersectBuildSinkOperator>(
                _id, _plan_node_id, _intersect_partition_ctx_factory->get_or_create(driver_sequence), _dst_exprs);
    }

    Status prepare(RuntimeState* state) override;

    void close(RuntimeState* state) override;

private:
    IntersectPartitionContextFactoryPtr _intersect_partition_ctx_factory;
    const std::vector<ExprContext*>& _dst_exprs;
};

} // namespace starrocks::pipeline

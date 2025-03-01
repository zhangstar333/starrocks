// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include "exec/pipeline/operator.h"
#include "exec/pipeline/set/intersect_context.h"

namespace starrocks::pipeline {

// Each IntersectProbeSinkOperator probes the hash set built by IntersectBuildSinkOperator.
// The hash set records the ordinal of IntersectProbeSinkOperator per key if the key is hit.
// For more detail information, see the comments of class IntersectBuildSinkOperator.
class IntersectProbeSinkOperator final : public Operator {
public:
    IntersectProbeSinkOperator(int32_t id, int32_t plan_node_id, std::shared_ptr<IntersectContext> intersect_ctx,
                               const std::vector<ExprContext*>& dst_exprs, const int32_t dependency_index)
            : Operator(id, "intersect_probe_sink", plan_node_id),
              _intersect_ctx(std::move(intersect_ctx)),
              _dst_exprs(dst_exprs),
              _dependency_index(dependency_index) {
        _intersect_ctx->ref();
    }

    bool need_input() const override {
        return _intersect_ctx->is_dependency_finished(_dependency_index) &&
               !(_is_finished || _intersect_ctx->is_ht_empty());
    }

    bool has_output() const override { return false; }

    bool is_finished() const override {
        if (_intersect_ctx->is_finished()) {
            return true;
        }

        return _intersect_ctx->is_dependency_finished(_dependency_index) &&
               (_is_finished || _intersect_ctx->is_ht_empty());
    }

    void set_finishing(RuntimeState* state) override {
        _is_finished = true;
        _intersect_ctx->finish_probe_ht();
    }

    Status close(RuntimeState* state) override;

    StatusOr<vectorized::ChunkPtr> pull_chunk(RuntimeState* state) override;

    Status push_chunk(RuntimeState* state, const ChunkPtr& chunk) override;

private:
    IntersectContextPtr _intersect_ctx;

    const std::vector<ExprContext*>& _dst_exprs;

    bool _is_finished = false;
    const int32_t _dependency_index;
};

class IntersectProbeSinkOperatorFactory final : public OperatorFactory {
public:
    IntersectProbeSinkOperatorFactory(int32_t id, int32_t plan_node_id,
                                      IntersectPartitionContextFactoryPtr intersect_partition_ctx_factory,
                                      const std::vector<ExprContext*>& dst_exprs, const int32_t dependency_index)
            : OperatorFactory(id, "intersect_probe_sink", plan_node_id),
              _intersect_partition_ctx_factory(std::move(intersect_partition_ctx_factory)),
              _dst_exprs(dst_exprs),
              _dependency_index(dependency_index) {}

    OperatorPtr create(int32_t degree_of_parallelism, int32_t driver_sequence) override {
        IntersectContextPtr intersect_ctx = _intersect_partition_ctx_factory->get_or_create(driver_sequence);
        return std::make_shared<IntersectProbeSinkOperator>(_id, _plan_node_id, std::move(intersect_ctx), _dst_exprs,
                                                            _dependency_index);
    }

    Status prepare(RuntimeState* state) override;

    void close(RuntimeState* state) override;

private:
    IntersectPartitionContextFactoryPtr _intersect_partition_ctx_factory;

    const std::vector<ExprContext*>& _dst_exprs;
    const int32_t _dependency_index;
};

} // namespace starrocks::pipeline

// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#pragma once

#include "exec/exec_node.h"
#include "exec/pipeline/operator.h"

namespace starrocks {
namespace pipeline {
// UNION ALL operator has three kinds of sub-node as follows:
// 1. Passthrough.
//    The src column from sub-node is projected to the dest column without expressions.
//    A src column may be projected to the multiple dest columns.
//    *UnionPassthroughOperator* is used for this case.
// 2. Materialize.
//    The src column is projected to the dest column with expressions.
//    *ProjectOperator* is used for this case.
// 3. Const.
//    Use the evaluation result of const expressions WITHOUT sub-node as the dest column.
//    Each expression is projected to the one dest row.
//    *UnionConstSourceOperator* is used for this case.

// UnionPassthroughOperator is for the Passthrough kind of sub-node.
class UnionPassthroughOperator final : public Operator {
public:
    struct SlotItem {
        SlotId slot_id;
        size_t ref_count;
    };

    using SlotMap = std::unordered_map<SlotId, SlotItem>;

    UnionPassthroughOperator(int32_t id, int32_t plan_node_id, SlotMap* dst2src_slot_map,
                             const std::vector<SlotDescriptor*>& slots, const std::vector<SlotDescriptor*>& src_slots)
            : Operator(id, "union_passthrough", plan_node_id),
              _dst2src_slot_map(dst2src_slot_map),
              _dst_slots(slots),
              _src_slots(src_slots) {}

    bool need_input() const override { return !_is_finished && _dst_chunk == nullptr; }

    bool has_output() const override { return _dst_chunk != nullptr; }

    bool is_finished() const override { return _is_finished && _dst_chunk == nullptr; }

    void set_finishing(RuntimeState* state) override { _is_finished = true; }

    Status push_chunk(RuntimeState* state, const ChunkPtr& src_chunk) override;

    StatusOr<vectorized::ChunkPtr> pull_chunk(RuntimeState* state) override;

private:
    // Maps the dst slot id of the dest chunk to that of the src chunk.
    // There may be multiple dest slot ids mapping to the same src slot id,
    // so we should decide whether you can move the src column according to this situation.
    SlotMap* _dst2src_slot_map;

    const std::vector<SlotDescriptor*>& _dst_slots;
    const std::vector<SlotDescriptor*>& _src_slots;

    bool _is_finished = false;
    vectorized::ChunkPtr _dst_chunk = nullptr;
};

class UnionPassthroughOperatorFactory final : public OperatorFactory {
public:
    UnionPassthroughOperatorFactory(int32_t id, int32_t plan_node_id,
                                    UnionPassthroughOperator::SlotMap* dst2src_slot_map,
                                    const std::vector<SlotDescriptor*>& dst_slots,
                                    const std::vector<SlotDescriptor*>& src_slots)
            : OperatorFactory(id, "union_passthrough", plan_node_id),
              _dst2src_slot_map(dst2src_slot_map),
              _dst_slots(dst_slots),
              _src_slots(src_slots) {}

    OperatorPtr create(int32_t degree_of_parallelism, int32_t driver_sequence) override {
        return std::make_shared<UnionPassthroughOperator>(_id, _plan_node_id, _dst2src_slot_map, _dst_slots,
                                                          _src_slots);
    }

private:
    // It will be nullptr, if _pass_through_slot_maps of UnionNode is empty.
    UnionPassthroughOperator::SlotMap* _dst2src_slot_map;

    const std::vector<SlotDescriptor*>& _dst_slots;
    const std::vector<SlotDescriptor*>& _src_slots;
};

} // namespace pipeline
} // namespace starrocks

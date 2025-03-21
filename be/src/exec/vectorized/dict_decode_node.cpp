// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "exec/vectorized/dict_decode_node.h"

#include <fmt/ranges.h>

#include <utility>

#include "column/chunk.h"
#include "column/column_helper.h"
#include "common/logging.h"
#include "exec/pipeline/dict_decode_operator.h"
#include "exec/pipeline/pipeline_builder.h"
#include "fmt/format.h"
#include "glog/logging.h"
#include "runtime/runtime_state.h"
#include "util/runtime_profile.h"

namespace starrocks::vectorized {

DictDecodeNode::DictDecodeNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs)
        : ExecNode(pool, tnode, descs) {}

Status DictDecodeNode::init(const TPlanNode& tnode, RuntimeState* state) {
    RETURN_IF_ERROR(ExecNode::init(tnode, state));
    _init_counter();

    for (const auto& [slot_id, texpr] : tnode.decode_node.string_functions) {
        ExprContext* context;
        RETURN_IF_ERROR(Expr::create_expr_tree(_pool, texpr, &context));
        _string_functions[slot_id] = std::make_pair(context, DictOptimizeContext{});
        _expr_ctxs.push_back(context);
    }

    for (const auto& [encode_id, decode_id] : tnode.decode_node.dict_id_to_string_ids) {
        _encode_column_cids.emplace_back(encode_id);
        _decode_column_cids.emplace_back(decode_id);
    }

    return Status::OK();
}

void DictDecodeNode::_init_counter() {
    _decode_timer = ADD_TIMER(_runtime_profile, "DictDecodeTime");
}

Status DictDecodeNode::prepare(RuntimeState* state) {
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    RETURN_IF_ERROR(ExecNode::prepare(state));
    RETURN_IF_ERROR(Expr::prepare(_expr_ctxs, state, row_desc()));
    return Status::OK();
}

Status DictDecodeNode::open(RuntimeState* state) {
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    RETURN_IF_ERROR(ExecNode::open(state));
    RETURN_IF_ERROR(Expr::open(_expr_ctxs, state));
    RETURN_IF_CANCELLED(state);
    RETURN_IF_ERROR(_children[0]->open(state));

    const auto& global_dict = state->get_query_global_dict_map();
    _dict_optimize_parser.set_mutable_dict_maps(state->mutable_query_global_dict_map());

    DCHECK_EQ(_encode_column_cids.size(), _decode_column_cids.size());
    int need_decode_size = _decode_column_cids.size();
    for (int i = 0; i < need_decode_size; ++i) {
        int need_encode_cid = _encode_column_cids[i];
        auto dict_iter = global_dict.find(need_encode_cid);
        auto dict_not_contains_cid = dict_iter == global_dict.end();
        auto input_has_string_function = _string_functions.find(need_encode_cid) != _string_functions.end();

        if (dict_not_contains_cid && !input_has_string_function) {
            return Status::InternalError(fmt::format("Not found dict for cid:{}", need_encode_cid));
        } else if (dict_not_contains_cid && input_has_string_function) {
            auto& [expr_ctx, dict_ctx] = _string_functions[need_encode_cid];
            DCHECK(expr_ctx->root()->fn().could_apply_dict_optimize);
            _dict_optimize_parser.check_could_apply_dict_optimize(expr_ctx, &dict_ctx);

            if (!dict_ctx.could_apply_dict_optimize) {
                return Status::InternalError(
                        fmt::format("Not found dict for function-called cid:{} it may cause by unsupport function",
                                    need_encode_cid));
            }

            _dict_optimize_parser.eval_expr(state, expr_ctx, &dict_ctx, need_encode_cid);
            dict_iter = global_dict.find(need_encode_cid);
            DCHECK(dict_iter != global_dict.end());
            if (dict_iter == global_dict.end()) {
                return Status::InternalError(fmt::format("Eval Expr Error for cid:{}", need_encode_cid));
            }
        }

        DefaultDecoderPtr decoder = std::make_unique<DefaultDecoder>();
        // TODO : avoid copy dict
        decoder->dict = dict_iter->second.second;
        _decoders.emplace_back(std::move(decoder));
    }
    if (VLOG_ROW_IS_ON) {
        for (int i = 0; i < _decoders.size(); ++i) {
            VLOG_ROW << "map " << _encode_column_cids[i] << ":" << _decoders[i]->dict;
        }
    }

    return Status::OK();
}

Status DictDecodeNode::get_next(RuntimeState* state, ChunkPtr* chunk, bool* eos) {
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    RETURN_IF_CANCELLED(state);
    *eos = false;
    do {
        RETURN_IF_ERROR(_children[0]->get_next(state, chunk, eos));
    } while (!(*eos) && (*chunk)->num_rows() == 0);

    if (*eos) {
        *chunk = nullptr;
        return Status::OK();
    }

    Columns decode_columns(_encode_column_cids.size());
    for (size_t i = 0; i < _encode_column_cids.size(); i++) {
        const ColumnPtr& encode_column = (*chunk)->get_column_by_slot_id(_encode_column_cids[i]);
        TypeDescriptor desc;
        desc.type = TYPE_VARCHAR;

        decode_columns[i] = ColumnHelper::create_column(desc, encode_column->is_nullable());
        RETURN_IF_ERROR(_decoders[i]->decode(encode_column.get(), decode_columns[i].get()));
    }

    ChunkPtr nchunk = std::make_shared<Chunk>();
    for (const auto& [k, v] : (*chunk)->get_slot_id_to_index_map()) {
        if (std::find(_encode_column_cids.begin(), _encode_column_cids.end(), k) == _encode_column_cids.end()) {
            auto& col = (*chunk)->get_column_by_slot_id(k);
            nchunk->append_column(col, k);
        }
    }
    for (size_t i = 0; i < decode_columns.size(); i++) {
        nchunk->append_column(decode_columns[i], _decode_column_cids[i]);
    }
    *chunk = nchunk;

    DCHECK_CHUNK(*chunk);
    return Status::OK();
}

Status DictDecodeNode::close(RuntimeState* state) {
    if (is_closed()) {
        return Status::OK();
    }
    RETURN_IF_ERROR(ExecNode::close(state));
    Expr::close(_expr_ctxs, state);
    _dict_optimize_parser.close(state);

    return Status::OK();
}

pipeline::OpFactories DictDecodeNode::decompose_to_pipeline(pipeline::PipelineBuilderContext* context) {
    using namespace pipeline;
    OpFactories operators = _children[0]->decompose_to_pipeline(context);
    operators.emplace_back(std::make_shared<DictDecodeOperatorFactory>(
            context->next_operator_id(), id(), std::move(_encode_column_cids), std::move(_decode_column_cids),
            std::move(_expr_ctxs), std::move(_string_functions)));

    return operators;
}

} // namespace starrocks::vectorized

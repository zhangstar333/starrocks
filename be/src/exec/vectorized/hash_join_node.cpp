// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "exec/vectorized/hash_join_node.h"

#include <runtime/runtime_state.h>

#include <memory>

#include "column/column_helper.h"
#include "column/fixed_length_column.h"
#include "column/vectorized_fwd.h"
#include "exec/pipeline/hashjoin/hash_join_build_operator.h"
#include "exec/pipeline/hashjoin/hash_join_probe_operator.h"
#include "exec/pipeline/hashjoin/hash_joiner_factory.h"
#include "exec/pipeline/pipeline_builder.h"
#include "exec/vectorized/hash_joiner.h"
#include "exprs/expr.h"
#include "exprs/vectorized/column_ref.h"
#include "exprs/vectorized/in_const_predicate.hpp"
#include "exprs/vectorized/runtime_filter_bank.h"
#include "gutil/strings/substitute.h"
#include "runtime/runtime_filter_worker.h"
#include "simd/simd.h"
#include "util/runtime_profile.h"
namespace starrocks::vectorized {

static constexpr size_t kHashJoinKeyColumnOffset = 1;

HashJoinNode::HashJoinNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs)
        : ExecNode(pool, tnode, descs),
          _hash_join_node(tnode.hash_join_node),
          _join_type(tnode.hash_join_node.join_op) {
    _is_push_down = tnode.hash_join_node.is_push_down;
    if (_join_type == TJoinOp::LEFT_ANTI_JOIN && tnode.hash_join_node.is_rewritten_from_not_in) {
        _join_type = TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN;
    }
    _build_runtime_filters_from_planner = false;
    if (tnode.hash_join_node.__isset.build_runtime_filters_from_planner) {
        _build_runtime_filters_from_planner = tnode.hash_join_node.build_runtime_filters_from_planner;
    }
}

Status HashJoinNode::init(const TPlanNode& tnode, RuntimeState* state) {
    RETURN_IF_ERROR(ExecNode::init(tnode, state));

    if (tnode.hash_join_node.__isset.sql_join_predicates) {
        _runtime_profile->add_info_string("JoinPredicates", tnode.hash_join_node.sql_join_predicates);
    }
    if (tnode.hash_join_node.__isset.sql_predicates) {
        _runtime_profile->add_info_string("Predicates", tnode.hash_join_node.sql_predicates);
    }

    const std::vector<TEqJoinCondition>& eq_join_conjuncts = tnode.hash_join_node.eq_join_conjuncts;
    for (const auto& eq_join_conjunct : eq_join_conjuncts) {
        ExprContext* ctx = nullptr;
        RETURN_IF_ERROR(Expr::create_expr_tree(_pool, eq_join_conjunct.left, &ctx));
        _probe_expr_ctxs.push_back(ctx);
        RETURN_IF_ERROR(Expr::create_expr_tree(_pool, eq_join_conjunct.right, &ctx));
        _build_expr_ctxs.push_back(ctx);

        if (eq_join_conjunct.__isset.opcode && eq_join_conjunct.opcode == TExprOpcode::EQ_FOR_NULL) {
            _is_null_safes.emplace_back(true);
        } else {
            _is_null_safes.emplace_back(false);
        }
    }

    RETURN_IF_ERROR(
            Expr::create_expr_trees(_pool, tnode.hash_join_node.other_join_conjuncts, &_other_join_conjunct_ctxs));

    for (const auto& desc : tnode.hash_join_node.build_runtime_filters) {
        auto* rf_desc = _pool->add(new RuntimeFilterBuildDescriptor());
        RETURN_IF_ERROR(rf_desc->init(_pool, desc));
        _build_runtime_filters.emplace_back(rf_desc);
    }

    return Status::OK();
}

Status HashJoinNode::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(ExecNode::prepare(state));

    _build_timer = ADD_TIMER(_runtime_profile, "BuildTime");

    _copy_right_table_chunk_timer = ADD_CHILD_TIMER(_runtime_profile, "1-CopyRightTableChunkTime", "BuildTime");
    _build_ht_timer = ADD_CHILD_TIMER(_runtime_profile, "2-BuildHashTableTime", "BuildTime");
    _build_push_down_expr_timer = ADD_CHILD_TIMER(_runtime_profile, "3-BuildPushDownExprTime", "BuildTime");
    _build_conjunct_evaluate_timer = ADD_CHILD_TIMER(_runtime_profile, "4-BuildConjunctEvaluateTime", "BuildTime");

    _probe_timer = ADD_TIMER(_runtime_profile, "ProbeTime");
    _merge_input_chunk_timer = ADD_CHILD_TIMER(_runtime_profile, "1-MergeInputChunkTimer", "ProbeTime");
    _search_ht_timer = ADD_CHILD_TIMER(_runtime_profile, "2-SearchHashTableTimer", "ProbeTime");
    _output_build_column_timer = ADD_CHILD_TIMER(_runtime_profile, "3-OutputBuildColumnTimer", "ProbeTime");
    _output_probe_column_timer = ADD_CHILD_TIMER(_runtime_profile, "4-OutputProbeColumnTimer", "ProbeTime");
    _output_tuple_column_timer = ADD_CHILD_TIMER(_runtime_profile, "5-OutputTupleColumnTimer", "ProbeTime");
    _probe_conjunct_evaluate_timer = ADD_CHILD_TIMER(_runtime_profile, "6-ProbeConjunctEvaluateTime", "ProbeTime");
    _other_join_conjunct_evaluate_timer =
            ADD_CHILD_TIMER(_runtime_profile, "7-OtherJoinConjunctEvaluateTime", "ProbeTime");
    _where_conjunct_evaluate_timer = ADD_CHILD_TIMER(_runtime_profile, "8-WhereConjunctEvaluateTime", "ProbeTime");

    _probe_rows_counter = ADD_COUNTER(_runtime_profile, "ProbeRows", TUnit::UNIT);
    _build_rows_counter = ADD_COUNTER(_runtime_profile, "BuildRows", TUnit::UNIT);
    _build_buckets_counter = ADD_COUNTER(_runtime_profile, "BuildBuckets", TUnit::UNIT);
    _push_down_expr_num = ADD_COUNTER(_runtime_profile, "PushDownExprNum", TUnit::UNIT);
    _avg_input_probe_chunk_size = ADD_COUNTER(_runtime_profile, "AvgInputProbeChunkSize", TUnit::UNIT);
    _avg_output_chunk_size = ADD_COUNTER(_runtime_profile, "AvgOutputChunkSize", TUnit::UNIT);
    _runtime_profile->add_info_string("JoinType", _get_join_type_str(_join_type));

    RETURN_IF_ERROR(Expr::prepare(_build_expr_ctxs, state, child(1)->row_desc()));
    RETURN_IF_ERROR(Expr::prepare(_probe_expr_ctxs, state, child(0)->row_desc()));
    RETURN_IF_ERROR(Expr::prepare(_other_join_conjunct_ctxs, state, _row_descriptor));

    HashTableParam param;
    _init_hash_table_param(&param);
    _ht.create(param);

    _probe_column_count = _ht.get_probe_column_count();
    _build_column_count = _ht.get_build_column_count();

    return Status::OK();
}

void HashJoinNode::_init_hash_table_param(HashTableParam* param) {
    param->with_other_conjunct = !_other_join_conjunct_ctxs.empty();
    param->join_type = _join_type;
    param->row_desc = &_row_descriptor;
    param->build_row_desc = &child(1)->row_desc();
    param->probe_row_desc = &child(0)->row_desc();
    param->search_ht_timer = _search_ht_timer;
    param->output_build_column_timer = _output_build_column_timer;
    param->output_probe_column_timer = _output_probe_column_timer;
    param->output_tuple_column_timer = _output_tuple_column_timer;

    for (auto i = 0; i < _probe_expr_ctxs.size(); i++) {
        param->join_keys.emplace_back(JoinKeyDesc{_probe_expr_ctxs[i]->root()->type().type, _is_null_safes[i]});
    }
}

Status HashJoinNode::open(RuntimeState* state) {
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    ScopedTimer<MonotonicStopWatch> build_timer(_build_timer);
    RETURN_IF_CANCELLED(state);

    RETURN_IF_ERROR(ExecNode::open(state));
    RETURN_IF_ERROR(Expr::open(_build_expr_ctxs, state));
    RETURN_IF_ERROR(Expr::open(_probe_expr_ctxs, state));
    RETURN_IF_ERROR(Expr::open(_other_join_conjunct_ctxs, state));

    {
        build_timer.stop();
        RETURN_IF_ERROR(child(1)->open(state));
        build_timer.start();
    }

    while (true) {
        ChunkPtr chunk = nullptr;
        bool eos = false;
        {
            RETURN_IF_CANCELLED(state);
            // fetch chunk of right table
            build_timer.stop();
            RETURN_IF_ERROR(child(1)->get_next(state, &chunk, &eos));
            build_timer.start();
            if (eos) {
                break;
            }

            if (chunk->num_rows() <= 0) {
                continue;
            }
        }

        if (_ht.get_row_count() + chunk->num_rows() >= UINT32_MAX) {
            return Status::NotSupported(strings::Substitute("row count of right table in hash join > $0", UINT32_MAX));
        }

        {
            RETURN_IF_ERROR(state->check_mem_limit("HashJoinNode"));
            // copy chunk of right table
            SCOPED_TIMER(_copy_right_table_chunk_timer);
            RETURN_IF_ERROR(_ht.append_chunk(state, chunk));
        }
    }

    {
        // build hash table: compute key columns, and then build the hash table.
        RETURN_IF_ERROR(_build(state));
        RETURN_IF_ERROR(state->check_mem_limit("HashJoinNode"));
        COUNTER_SET(_build_rows_counter, static_cast<int64_t>(_ht.get_row_count()));
        COUNTER_SET(_build_buckets_counter, static_cast<int64_t>(_ht.get_bucket_size()));
    }

    uint64_t runtime_join_filter_pushdown_limit = 1024000;
    if (state->query_options().__isset.runtime_join_filter_pushdown_limit) {
        runtime_join_filter_pushdown_limit = state->query_options().runtime_join_filter_pushdown_limit;
    }

    if (_is_push_down) {
        if (_children[0]->type() == TPlanNodeType::EXCHANGE_NODE &&
            _children[1]->type() == TPlanNodeType::EXCHANGE_NODE) {
            _is_push_down = false;
        } else if (_ht.get_row_count() > runtime_join_filter_pushdown_limit) {
            _is_push_down = false;
        }

        if (_is_push_down || !child(1)->conjunct_ctxs().empty()) {
            // In filter could be used to fast compute segment row range in storage engine
            RETURN_IF_ERROR(_push_down_in_filter(state));
            RETURN_IF_ERROR(_create_implicit_local_join_runtime_filters(state));
        }
    }

    // it's quite critical to put publish runtime filters before short-circuit of
    // "inner-join with empty right table". because for global runtime filter
    // merge node is waiting for all partitioned runtime filter, so even hash row count is zero
    // we still have to build it.
    RETURN_IF_ERROR(_do_publish_runtime_filters(state, runtime_join_filter_pushdown_limit));

    build_timer.stop();
    RETURN_IF_ERROR(child(0)->open(state));
    build_timer.start();

    // special cases of short-circuit break.
    if (_ht.get_row_count() == 0 && (_join_type == TJoinOp::INNER_JOIN || _join_type == TJoinOp::LEFT_SEMI_JOIN)) {
        _eos = true;
        return Status::OK();
    }

    if (_ht.get_row_count() > 0) {
        if (_join_type == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN && _ht.get_key_columns().size() == 1 &&
            _has_null(_ht.get_key_columns()[0])) {
            // The current implementation of HashTable will reserve a row for judging the end of the linked list.
            // When performing expression calculations (such as cast string to int),
            // it is possible that this reserved row will generate Null,
            // so Column::has_null() cannot be used to judge whether there is Null in the right table.
            // TODO: This reserved field will be removed in the implementation mechanism in the future.
            // at that time, you can directly use Column::has_null() to judge
            _eos = true;
            return Status::OK();
        }
    }

    _mem_tracker->set(_ht.mem_usage());

    return Status::OK();
}

Status HashJoinNode::get_next(RuntimeState* state, RowBatch* row_batch, bool* eos) {
    return Status::NotSupported("get_next for row_batch is not supported");
}

Status HashJoinNode::get_next(RuntimeState* state, ChunkPtr* chunk, bool* eos) {
    RETURN_IF_CANCELLED(state);
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    ScopedTimer<MonotonicStopWatch> probe_timer(_probe_timer);

    if (reached_limit()) {
        _eos = true;
        *eos = true;
        _final_update_profile();
        return Status::OK();
    }

    if (_eos) {
        *eos = true;
        _final_update_profile();
        return Status::OK();
    }

    *chunk = std::make_shared<Chunk>();

    bool tmp_eos = false;
    if (!_probe_eos || _ht_has_remain) {
        RETURN_IF_ERROR(_probe(state, probe_timer, chunk, tmp_eos));
        if (tmp_eos) {
            if (_join_type == TJoinOp::RIGHT_OUTER_JOIN || _join_type == TJoinOp::RIGHT_ANTI_JOIN ||
                _join_type == TJoinOp::FULL_OUTER_JOIN) {
                // fetch the remain data of hash table
                RETURN_IF_ERROR(_probe_remain(chunk, tmp_eos));
                if (tmp_eos) {
                    _eos = true;
                    *eos = true;
                    _final_update_profile();
                    return Status::OK();
                }
            } else {
                _eos = true;
                *eos = true;
                _final_update_profile();
                return Status::OK();
            }
        }
    } else {
        if (!_build_eos) {
            if (_join_type == TJoinOp::RIGHT_OUTER_JOIN || _join_type == TJoinOp::RIGHT_ANTI_JOIN ||
                _join_type == TJoinOp::FULL_OUTER_JOIN) {
                // fetch the remain data of hash table
                RETURN_IF_ERROR(_probe_remain(chunk, tmp_eos));
                if (tmp_eos) {
                    _eos = true;
                    *eos = true;
                    _final_update_profile();
                    return Status::OK();
                }
            } else {
                _eos = true;
                *eos = true;
                _final_update_profile();
                return Status::OK();
            }
        } else {
            _eos = true;
            *eos = true;
            _final_update_profile();
            return Status::OK();
        }
    }

    DCHECK_LE((*chunk)->num_rows(), config::vector_chunk_size);
    _num_rows_returned += (*chunk)->num_rows();
    _output_chunk_count++;
    if (reached_limit()) {
        (*chunk)->set_num_rows((*chunk)->num_rows() - (_num_rows_returned - _limit));
        _num_rows_returned = _limit;
        COUNTER_SET(_rows_returned_counter, _limit);
    } else {
        COUNTER_SET(_rows_returned_counter, _num_rows_returned);
    }

    DCHECK_EQ((*chunk)->num_columns(),
              (*chunk)->get_tuple_id_to_index_map().size() + (*chunk)->get_slot_id_to_index_map().size());

    *eos = false;
    DCHECK_CHUNK(*chunk);
    return Status::OK();
}

Status HashJoinNode::close(RuntimeState* state) {
    if (is_closed()) {
        return Status::OK();
    }

    Expr::close(_build_expr_ctxs, state);
    Expr::close(_probe_expr_ctxs, state);
    Expr::close(_other_join_conjunct_ctxs, state);

    _ht.close();

    return ExecNode::close(state);
}

pipeline::OpFactories HashJoinNode::decompose_to_pipeline(pipeline::PipelineBuilderContext* context) {
    auto hash_joiner_factory = std::make_shared<starrocks::pipeline::HashJoinerFactory>(
            _hash_join_node, _id, _type, limit(), std::move(_is_null_safes), _build_expr_ctxs, _probe_expr_ctxs,
            std::move(_other_join_conjunct_ctxs), std::move(_conjunct_ctxs), child(1)->row_desc(), child(0)->row_desc(),
            _row_descriptor, context->degree_of_parallelism());

    auto build_op = std::make_shared<pipeline::HashJoinBuildOperatorFactory>(context->next_operator_id(), id(),
                                                                             hash_joiner_factory);
    // HashJoinProbeOperatorFactory holds the ownership of HashJoiner object.
    auto probe_op = std::make_shared<pipeline::HashJoinProbeOperatorFactory>(context->next_operator_id(), id(),
                                                                             hash_joiner_factory);
    auto rhs_operators = child(1)->decompose_to_pipeline(context);
    auto lhs_operators = child(0)->decompose_to_pipeline(context);
    // both HashJoin{Build, Probe}Operator are parallelized, so add LocalExchangeOperator
    // to shuffle multi-stream into #degree_of_parallelism# streams each of that pipes into HashJoin{Build, Probe}Operator.
    auto operators_with_build_op = context->maybe_interpolate_local_shuffle_exchange(rhs_operators, _build_expr_ctxs);
    auto operators_with_probe_op = context->maybe_interpolate_local_shuffle_exchange(lhs_operators, _probe_expr_ctxs);
    // add build-side pipeline to context and return probe-side pipeline.
    operators_with_build_op.emplace_back(std::move(build_op));
    context->add_pipeline(operators_with_build_op);
    operators_with_probe_op.emplace_back(std::move(probe_op));
    return operators_with_probe_op;
}

bool HashJoinNode::_has_null(const ColumnPtr& column) {
    if (column->is_nullable()) {
        const auto& null_column = ColumnHelper::as_raw_column<NullableColumn>(column)->null_column();
        DCHECK_GT(null_column->size(), 0);
        return null_column->contain_value(1, null_column->size(), 1);
    }
    return false;
}

Status HashJoinNode::_build(RuntimeState* state) {
    {
        SCOPED_TIMER(_build_conjunct_evaluate_timer);
        // Currently, in order to implement simplicity, HashJoinNode uses BigChunk,
        // Splice the Chunks from Scan on the right table into a big Chunk
        // In some scenarios, such as when the left and right tables are selected incorrectly
        // or when the large table is joined, the (BinaryColumn) in the Chunk exceeds the range of uint32_t,
        // which will cause the output of wrong data.
        // Currently, a defense needs to be added.
        // After a better solution is available, the BigChunk mechanism can be removed.
        if (_ht.get_build_chunk()->reach_capacity_limit()) {
            return Status::InternalError("Total size of single column exceed the limit of hash join");
        }

        for (auto& _build_expr_ctx : _build_expr_ctxs) {
            const TypeDescriptor& data_type = _build_expr_ctx->root()->type();
            ColumnPtr column_ptr = _build_expr_ctx->evaluate(_ht.get_build_chunk().get());
            if (column_ptr->is_nullable() && column_ptr->is_constant()) {
                ColumnPtr column = ColumnHelper::create_column(data_type, true);
                column->append_nulls(_ht.get_build_chunk()->num_rows());
                _ht.get_key_columns().emplace_back(column);
            } else if (column_ptr->is_constant()) {
                auto* const_column = ColumnHelper::as_raw_column<ConstColumn>(column_ptr);
                const_column->data_column()->assign(_ht.get_build_chunk()->num_rows(), 0);
                _ht.get_key_columns().emplace_back(const_column->data_column());
            } else {
                _ht.get_key_columns().emplace_back(column_ptr);
            }
        }
    }

    {
        SCOPED_TIMER(_build_ht_timer);
        RETURN_IF_ERROR(_ht.build(state));
    }

    return Status::OK();
}

static inline bool check_chunk_zero_and_create_new(ChunkPtr* chunk) {
    if ((*chunk)->num_rows() <= 0) {
        // TODO: It's better to reuse the chunk object.
        // Use a new chunk to continue call _ht.probe.
        *chunk = std::make_shared<Chunk>();
        return true;
    }
    return false;
}

Status HashJoinNode::_probe(RuntimeState* state, ScopedTimer<MonotonicStopWatch>& probe_timer, ChunkPtr* chunk,
                            bool& eos) {
    while (true) {
        if (!_ht_has_remain) {
            while (true) {
                {
                    // if current chunk size >= vector_chunk_size / 2, direct return the current chunk
                    // if current chunk size < vector_chunk_size and pre chunk size + cur chunk size <= 1024, merge the two chunk
                    // if current chunk size < vector_chunk_size and pre chunk size + cur chunk size > 1024, return pre chunk
                    probe_timer.stop();
                    RETURN_IF_ERROR(child(0)->get_next(state, &_cur_left_input_chunk, &_probe_eos));
                    probe_timer.start();
                    {
                        SCOPED_TIMER(_merge_input_chunk_timer);
                        _probe_chunk_count++;
                        if (_probe_eos) {
                            if (_pre_left_input_chunk != nullptr) {
                                // has reserved probe chunk
                                eos = false;
                                _probing_chunk = std::move(_pre_left_input_chunk);
                            } else {
                                eos = true;
                                return Status::OK();
                            }
                        } else {
                            if (_cur_left_input_chunk->num_rows() <= 0) {
                                continue;
                            } else if (_cur_left_input_chunk->num_rows() >= config::vector_chunk_size / 2) {
                                // the probe chunk size of read from right child >= config::vector_chunk_size, direct return
                                _probing_chunk = std::move(_cur_left_input_chunk);
                            } else if (_pre_left_input_chunk == nullptr) {
                                // the probe chunk size is small, reserve for merge next probe chunk
                                _pre_left_input_chunk = std::move(_cur_left_input_chunk);
                                continue;
                            } else {
                                if (_cur_left_input_chunk->num_rows() + _pre_left_input_chunk->num_rows() >
                                    config::vector_chunk_size) {
                                    // the two chunk size > config::vector_chunk_size, return the first reserved chunk
                                    _probing_chunk = std::move(_pre_left_input_chunk);
                                    _pre_left_input_chunk = std::move(_cur_left_input_chunk);
                                } else {
                                    // TODO: copy the small chunk to big chunk
                                    Columns& dest_columns = _pre_left_input_chunk->columns();
                                    Columns& src_columns = _cur_left_input_chunk->columns();
                                    size_t num_rows = _cur_left_input_chunk->num_rows();
                                    // copy the new read chunk to the reserved
                                    for (size_t i = 0; i < dest_columns.size(); i++) {
                                        dest_columns[i]->append(*src_columns[i], 0, num_rows);
                                    }
                                    _cur_left_input_chunk.reset();
                                    continue;
                                }
                            }
                        }
                    }
                }

                COUNTER_UPDATE(_probe_rows_counter, _probing_chunk->num_rows());

                {
                    SCOPED_TIMER(_probe_conjunct_evaluate_timer);
                    _key_columns.resize(0);
                    for (auto& probe_expr_ctx : _probe_expr_ctxs) {
                        ColumnPtr column_ptr = probe_expr_ctx->evaluate(_probing_chunk.get());
                        if (column_ptr->is_nullable() && column_ptr->is_constant()) {
                            ColumnPtr column = ColumnHelper::create_column(probe_expr_ctx->root()->type(), true);
                            column->append_nulls(_probing_chunk->num_rows());
                            _key_columns.emplace_back(column);
                        } else if (column_ptr->is_constant()) {
                            auto* const_column = ColumnHelper::as_raw_column<ConstColumn>(column_ptr);
                            const_column->data_column()->assign(_probing_chunk->num_rows(), 0);
                            _key_columns.emplace_back(const_column->data_column());
                        } else {
                            _key_columns.emplace_back(column_ptr);
                        }
                    }
                }

                DCHECK_GT(_key_columns.size(), 0);
                DCHECK_NOTNULL(_key_columns[0].get());
                if (!_key_columns[0]->empty()) {
                    break;
                }
            }
        }

        RETURN_IF_ERROR(_ht.probe(_key_columns, &_probing_chunk, chunk, &_ht_has_remain));
        if (!_ht_has_remain) {
            _probing_chunk = nullptr;
        }

        eval_join_runtime_filters(chunk);

        if (check_chunk_zero_and_create_new(chunk)) {
            continue;
        }

        if (!_other_join_conjunct_ctxs.empty()) {
            SCOPED_TIMER(_other_join_conjunct_evaluate_timer);
            _process_other_conjunct(chunk);
            if (check_chunk_zero_and_create_new(chunk)) {
                continue;
            }
        }

        if (!_conjunct_ctxs.empty()) {
            SCOPED_TIMER(_where_conjunct_evaluate_timer);
            eval_conjuncts(_conjunct_ctxs, (*chunk).get());

            if (check_chunk_zero_and_create_new(chunk)) {
                continue;
            }
        }

        break;
    }

    return Status::OK();
}

Status HashJoinNode::_probe_remain(ChunkPtr* chunk, bool& eos) {
    ScopedTimer<MonotonicStopWatch> probe_timer(_probe_timer);

    while (!_build_eos) {
        RETURN_IF_ERROR(_ht.probe_remain(chunk, &_right_table_has_remain));

        eval_join_runtime_filters(chunk);

        if ((*chunk)->num_rows() <= 0) {
            // right table already have no remain data
            _build_eos = true;
            eos = true;
            return Status::OK();
        }

        if (!_conjunct_ctxs.empty()) {
            eval_conjuncts(_conjunct_ctxs, (*chunk).get());

            if (check_chunk_zero_and_create_new(chunk)) {
                _build_eos = !_right_table_has_remain;
                continue;
            }
        }

        eos = false;
        _build_eos = !_right_table_has_remain;
        return Status::OK();
    }

    eos = true;
    return Status::OK();
}

void HashJoinNode::_calc_filter_for_other_conjunct(ChunkPtr* chunk, Column::Filter& filter, bool& filter_all,
                                                   bool& hit_all) {
    filter_all = false;
    hit_all = false;
    filter.assign((*chunk)->num_rows(), 1);

    for (auto* ctx : _other_join_conjunct_ctxs) {
        ColumnPtr column = ctx->evaluate((*chunk).get());
        size_t true_count = ColumnHelper::count_true_with_notnull(column);

        if (true_count == column->size()) {
            // all hit, skip
            continue;
        } else if (0 == true_count) {
            // all not hit, return
            filter_all = true;
            filter.assign((*chunk)->num_rows(), 0);
            break;
        } else {
            bool all_zero = false;
            ColumnHelper::merge_two_filters(column, &filter, &all_zero);
            if (all_zero) {
                filter_all = true;
                break;
            }
        }
    }

    if (!filter_all) {
        int zero_count = SIMD::count_zero(filter.data(), filter.size());
        if (zero_count == 0) {
            hit_all = true;
        }
    }
}

void HashJoinNode::_process_row_for_other_conjunct(ChunkPtr* chunk, size_t start_column, size_t column_count,
                                                   bool filter_all, bool hit_all, const Column::Filter& filter) {
    if (filter_all) {
        for (size_t i = start_column; i < start_column + column_count; i++) {
            auto* null_column = ColumnHelper::as_raw_column<NullableColumn>((*chunk)->columns()[i]);
            auto& null_data = null_column->mutable_null_column()->get_data();
            for (size_t j = 0; j < (*chunk)->num_rows(); j++) {
                null_data[j] = 1;
                null_column->set_has_null(true);
            }
        }
    } else {
        if (hit_all) {
            return;
        }

        for (size_t i = start_column; i < start_column + column_count; i++) {
            auto* null_column = ColumnHelper::as_raw_column<NullableColumn>((*chunk)->columns()[i]);
            auto& null_data = null_column->mutable_null_column()->get_data();
            for (size_t j = 0; j < filter.size(); j++) {
                if (filter[j] == 0) {
                    null_data[j] = 1;
                    null_column->set_has_null(true);
                }
            }
        }
    }
}

void HashJoinNode::_process_outer_join_with_other_conjunct(ChunkPtr* chunk, size_t start_column, size_t column_count) {
    bool filter_all = false;
    bool hit_all = false;
    Column::Filter filter;

    _calc_filter_for_other_conjunct(chunk, filter, filter_all, hit_all);
    _process_row_for_other_conjunct(chunk, start_column, column_count, filter_all, hit_all, filter);

    _ht.remove_duplicate_index(&filter);
    (*chunk)->filter(filter);
}

void HashJoinNode::_process_semi_join_with_other_conjunct(ChunkPtr* chunk) {
    bool filter_all = false;
    bool hit_all = false;
    Column::Filter filter;

    _calc_filter_for_other_conjunct(chunk, filter, filter_all, hit_all);

    _ht.remove_duplicate_index(&filter);
    (*chunk)->filter(filter);
}

void HashJoinNode::_process_right_anti_join_with_other_conjunct(ChunkPtr* chunk) {
    bool filter_all = false;
    bool hit_all = false;
    Column::Filter filter;

    _calc_filter_for_other_conjunct(chunk, filter, filter_all, hit_all);

    _ht.remove_duplicate_index(&filter);
    (*chunk)->set_num_rows(0);
}

void HashJoinNode::_process_other_conjunct(ChunkPtr* chunk) {
    switch (_join_type) {
    case TJoinOp::LEFT_OUTER_JOIN:
    case TJoinOp::FULL_OUTER_JOIN:
        _process_outer_join_with_other_conjunct(chunk, _probe_column_count, _build_column_count);
        break;
    case TJoinOp::RIGHT_OUTER_JOIN:
    case TJoinOp::LEFT_SEMI_JOIN:
    case TJoinOp::LEFT_ANTI_JOIN:
    case TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN:
    case TJoinOp::RIGHT_SEMI_JOIN:
        _process_semi_join_with_other_conjunct(chunk);
        break;
    case TJoinOp::RIGHT_ANTI_JOIN:
        _process_right_anti_join_with_other_conjunct(chunk);
        break;
    default:
        // the other join conjunct for inner join will be convert to other predicate
        // so can't reach here
        eval_conjuncts(_other_join_conjunct_ctxs, (*chunk).get());
    }
}

Status HashJoinNode::_push_down_in_filter(RuntimeState* state) {
    SCOPED_TIMER(_build_push_down_expr_timer);

    if (_ht.get_row_count() > 1024) {
        return Status::OK();
    }

    if (_ht.get_row_count() > 0) {
        // there is a bug (DSDB-3860) in old planner if probe_expr is not slot-ref, and this fix is workaround.
        size_t size = _build_expr_ctxs.size();
        std::vector<bool> to_build(size, true);
        for (int i = 0; i < size; i++) {
            ExprContext* expr_ctx = _probe_expr_ctxs[i];
            to_build[i] = (expr_ctx->root()->is_slotref());
        }

        for (size_t i = 0; i < size; i++) {
            if (!to_build[i]) continue;
            ColumnPtr column = _ht.get_key_columns()[i];
            Expr* probe_expr = _probe_expr_ctxs[i]->root();
            // create and fill runtime IN filter.
            VectorizedInConstPredicateBuilder builder(state, _pool, probe_expr);
            builder.set_eq_null(_is_null_safes[i]);
            builder.use_as_join_runtime_filter();
            Status st = builder.create();
            if (!st.ok()) continue;
            builder.add_values(column, kHashJoinKeyColumnOffset);
            _runtime_in_filters.push_back(builder.get_in_const_predicate());
        }
    }

    if (_runtime_in_filters.empty()) {
        return Status::OK();
    }

    COUNTER_UPDATE(_push_down_expr_num, static_cast<int64_t>(_runtime_in_filters.size()));
    push_down_predicate(state, &_runtime_in_filters, true);

    return Status::OK();
}

Status HashJoinNode::_do_publish_runtime_filters(RuntimeState* state, int64_t limit) {
    SCOPED_TIMER(_build_push_down_expr_timer);

    // we build it even if hash table row count is 0
    // because for global runtime filter, we have to send that.
    for (auto* rf_desc : _build_runtime_filters) {
        // skip if it does not have consumer.
        if (!rf_desc->has_consumer()) continue;
        // skip if ht.size() > limit and it's only for local.
        if (!rf_desc->has_remote_targets() && _ht.get_row_count() > limit) continue;
        PrimitiveType build_type = rf_desc->build_expr_type();
        JoinRuntimeFilter* filter = RuntimeFilterHelper::create_runtime_bloom_filter(_pool, build_type);
        if (filter == nullptr) continue;
        filter->set_join_mode(rf_desc->join_mode());
        filter->init(_ht.get_row_count());
        int expr_order = rf_desc->build_expr_order();
        ColumnPtr column = _ht.get_key_columns()[expr_order];
        bool eq_null = _is_null_safes[expr_order];
        RETURN_IF_ERROR(RuntimeFilterHelper::fill_runtime_bloom_filter(column, build_type, filter,
                                                                       kHashJoinKeyColumnOffset, eq_null));
        rf_desc->set_runtime_filter(filter);
    }

    // publish runtime filters
    state->runtime_filter_port()->publish_runtime_filters(_build_runtime_filters);
    COUNTER_UPDATE(_push_down_expr_num, static_cast<int64_t>(_build_runtime_filters.size()));
    return Status::OK();
}

Status HashJoinNode::_create_implicit_local_join_runtime_filters(RuntimeState* state) {
    if (_build_runtime_filters_from_planner) return Status::OK();
    VLOG_FILE << "create implicit local join runtime filters";

    // to avoid filter id collision between multiple hash join nodes.
    const int implicit_runtime_filter_id_offset = 1000000 * (_id + 1);

    // build publish side.
    for (int i = 0; i < _build_expr_ctxs.size(); i++) {
        auto* desc = _pool->add(new RuntimeFilterBuildDescriptor());
        desc->_filter_id = implicit_runtime_filter_id_offset + i;
        desc->_build_expr_ctx = _build_expr_ctxs[i];
        desc->_build_expr_order = i;
        desc->_has_remote_targets = false;
        desc->_has_consumer = true;
        _build_runtime_filters.push_back(desc);
    }

    // build consume side.
    for (int i = 0; i < _probe_expr_ctxs.size(); i++) {
        auto* desc = _pool->add(new RuntimeFilterProbeDescriptor());
        desc->_filter_id = implicit_runtime_filter_id_offset + i;
        RETURN_IF_ERROR(_probe_expr_ctxs[i]->clone(state, &desc->_probe_expr_ctx));
        desc->_runtime_filter.store(nullptr);
        child(0)->register_runtime_filter_descriptor(state, desc);
    }

    // there are some runtime filters at child(0), try to push down them.
    child(0)->push_down_join_runtime_filter(state, &(child(0)->runtime_filter_collector()));

    return Status::OK();
}

std::string HashJoinNode::_get_join_type_str(TJoinOp::type join_type) {
    switch (join_type) {
    case TJoinOp::INNER_JOIN:
        return "InnerJoin";
    case TJoinOp::LEFT_OUTER_JOIN:
        return "LeftOuterJoin";
    case TJoinOp::LEFT_SEMI_JOIN:
        return "LeftSemiJoin";
    case TJoinOp::RIGHT_OUTER_JOIN:
        return "RightOuterJoin";
    case TJoinOp::FULL_OUTER_JOIN:
        return "FullOuterJoin";
    case TJoinOp::CROSS_JOIN:
        return "CrossJoin";
    case TJoinOp::MERGE_JOIN:
        return "MergeJoin";
    case TJoinOp::RIGHT_SEMI_JOIN:
        return "RightSemiJoin";
    case TJoinOp::LEFT_ANTI_JOIN:
        return "LeftAntiJoin";
    case TJoinOp::RIGHT_ANTI_JOIN:
        return "RightAntiJoin";
    case TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN:
        return "NullAwareLeftAntiJoin";
    default:
        return "";
    }
}

} // namespace starrocks::vectorized

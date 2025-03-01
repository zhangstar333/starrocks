// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/exec/exec_node.h

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

#ifndef STARROCKS_BE_SRC_QUERY_EXEC_EXEC_NODE_H
#define STARROCKS_BE_SRC_QUERY_EXEC_EXEC_NODE_H

#include <functional>
#include <mutex>
#include <sstream>
#include <vector>

#include "column/vectorized_fwd.h"
#include "common/status.h"
#include "exec/pipeline/operator.h"
#include "exprs/vectorized/runtime_filter_bank.h"
#include "gen_cpp/PlanNodes_types.h"
#include "runtime/descriptors.h"
#include "runtime/mem_pool.h"
#include "runtime/query_statistics.h"
#include "service/backend_options.h"
#include "util/blocking_queue.hpp"
#include "util/runtime_profile.h"
#include "util/uid_util.h" // for print_id

namespace starrocks {

class Expr;
class ExprContext;
class ObjectPool;
class RowBatch;
class RuntimeState;
class SlotRef;
class TPlan;
class TupleRow;
class DataSink;

namespace pipeline {
class OperatorFactory;
class PipelineBuilderContext;
} // namespace pipeline

using std::string;
using std::stringstream;
using std::vector;
using std::map;

using vectorized::ChunkPtr;
// Superclass of all executor nodes.
// All subclasses need to make sure to check RuntimeState::is_cancelled()
// periodically in order to ensure timely termination after the cancellation
// flag gets set.

// Our new vectorized query executor is more powerful and stable than old query executor,
// The executor query executor related codes could be deleted safely.
// TODO: Remove old query executor related codes before 2021-09-30

class ExecNode {
public:
    // Init conjuncts.
    ExecNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs);

    virtual ~ExecNode();

    /// Initializes this object from the thrift tnode desc. The subclass should
    /// do any initialization that can fail in Init() rather than the ctor.
    /// If overridden in subclass, must first call superclass's Init().
    virtual Status init(const TPlanNode& tnode, RuntimeState* state = nullptr);

    // Sets up internal structures, etc., without doing any actual work.
    // Must be called prior to open(). Will only be called once in this
    // node's lifetime.
    // All code generation (adding functions to the LlvmCodeGen object) must happen
    // in prepare().  Retrieving the jit compiled function pointer must happen in
    // open().
    // If overridden in subclass, must first call superclass's prepare().
    virtual Status prepare(RuntimeState* state);

    // Performs any preparatory work prior to calling get_next().
    // Can be called repeatedly (after calls to close()).
    // Caller must not be holding any io buffers. This will cause deadlock.
    virtual Status open(RuntimeState* state);

    virtual Status get_next(RuntimeState* state, RowBatch* row_batch, bool* eos) {
        return Status::NotSupported("Don't support old query engine any more");
    }

    virtual Status get_next(RuntimeState* state, ChunkPtr* chunk, bool* eos);

    // Used by sub nodes to get big chunk.
    // specific_get_next is the subclass's implementation to get datas.
    static Status get_next_big_chunk(RuntimeState*, ChunkPtr*, bool*, ChunkPtr& pre_output_chunk,
                                     const std::function<Status(RuntimeState*, ChunkPtr*, bool*)>& specific_get_next);

    // Resets the stream of row batches to be retrieved by subsequent GetNext() calls.
    // Clears all internal state, returning this node to the state it was in after calling
    // Prepare() and before calling Open(). This function must not clear memory
    // still owned by this node that is backing rows returned in GetNext().
    // Prepare() and Open() must have already been called before calling Reset().
    // GetNext() may have optionally been called (not necessarily until eos).
    // Close() must not have been called.
    // Reset() is not idempotent. Calling it multiple times in a row without a preceding
    // call to Open() is invalid.
    // If overridden in a subclass, must call superclass's Reset() at the end. The default
    // implementation calls Reset() on children.
    // Note that this function may be called many times (proportional to the input data),
    // so should be fast.
    virtual Status reset(RuntimeState* state);

    // This should be called before close() and after get_next(), it is responsible for
    // collecting statistics sent with row batch, it can't be called when prepare() returns
    // error.
    virtual Status collect_query_statistics(QueryStatistics* statistics);

    // close() will get called for every exec node, regardless of what else is called and
    // the status of these calls (i.e. prepare() may never have been called, or
    // prepare()/open()/get_next() returned with an error).
    // close() releases all resources that were allocated in open()/get_next(), even if the
    // latter ended with an error. close() can be called if the node has been prepared or
    // the node is closed.
    // After calling close(), the caller calls open() again prior to subsequent calls to
    // get_next(). The default implementation updates runtime profile counters and calls
    // close() on the children. To ensure that close() is called on the entire plan tree,
    // each implementation should start out by calling the default implementation.
    virtual Status close(RuntimeState* state);

    // Creates exec node tree from list of nodes contained in plan via depth-first
    // traversal. All nodes are placed in pool.
    // Returns error if 'plan' is corrupted, otherwise success.
    static Status create_tree(RuntimeState* state, ObjectPool* pool, const TPlan& plan, const DescriptorTbl& descs,
                              ExecNode** root);

    // Set debug action for node with given id in 'tree'
    static void set_debug_options(int node_id, TExecNodePhase::type phase, TDebugAction::type action, ExecNode* tree);

    // Collect all nodes of given 'node_type' that are part of this subtree, and return in
    // 'nodes'.
    void collect_nodes(TPlanNodeType::type node_type, std::vector<ExecNode*>* nodes);

    // Collect all scan node types.
    void collect_scan_nodes(std::vector<ExecNode*>* nodes);

    bool _check_has_vectorized_scan_child();

    // Evaluate exprs over row.  Returns true if all exprs return true.
    // TODO: This doesn't use the vector<Expr*> signature because I haven't figured
    // out how to deal with declaring a templated std:vector type in IR
    static bool eval_conjuncts(ExprContext* const* ctxs, int num_ctxs, TupleRow* row);

    // evaluate exprs over chunk to get a filter
    // if filter_ptr is not null, save filter to filter_ptr.
    // then running filter on chunk.
    static void eval_conjuncts(const std::vector<ExprContext*>& ctxs, vectorized::Chunk* chunk,
                               vectorized::FilterPtr* filter_ptr = nullptr);

    Status init_join_runtime_filters(const TPlanNode& tnode, RuntimeState* state);
    void register_runtime_filter_descriptor(RuntimeState* state, vectorized::RuntimeFilterProbeDescriptor* rf_desc);
    void eval_join_runtime_filters(vectorized::Chunk* chunk);
    void eval_join_runtime_filters(vectorized::ChunkPtr* chunk);
    void eval_filter_null_values(vectorized::Chunk* chunk);

    // Returns a string representation in DFS order of the plan rooted at this.
    std::string debug_string() const;

    virtual void push_down_predicate(RuntimeState* state, std::list<ExprContext*>* expr_ctxs, bool is_vectorized);
    virtual void push_down_join_runtime_filter(RuntimeState* state, vectorized::RuntimeFilterProbeCollector* collector);
    void push_down_join_runtime_filter_to_children(RuntimeState* state,
                                                   vectorized::RuntimeFilterProbeCollector* collector);

    // recursive helper method for generating a string for Debug_string().
    // implementations should call debug_string(int, std::stringstream) on their children.
    // Input parameters:
    //   indentation_level: Current level in plan tree.
    // Output parameters:
    //   out: Stream to accumulate debug string.
    virtual void debug_string(int indentation_level, std::stringstream* out) const;

    // Convert old exec node tree to new pipeline
    virtual pipeline::OpFactories decompose_to_pipeline(pipeline::PipelineBuilderContext* context);

    const std::vector<ExprContext*>& conjunct_ctxs() const { return _conjunct_ctxs; }

    int id() const { return _id; }
    TPlanNodeType::type type() const { return _type; }
    const RowDescriptor& row_desc() const { return _row_descriptor; }
    int64_t rows_returned() const { return _num_rows_returned; }
    int64_t limit() const { return _limit; }
    bool reached_limit() { return _limit != -1 && _num_rows_returned >= _limit; }
    const std::vector<TupleId>& get_tuple_ids() const { return _tuple_ids; }

    RuntimeProfile* runtime_profile() { return _runtime_profile.get(); }
    RuntimeProfile::Counter* memory_used_counter() const { return _memory_used_counter; }

    MemTracker* mem_tracker() const { return _mem_tracker.get(); }

    bool use_vectorized() { return _use_vectorized; }

    vectorized::RuntimeFilterProbeCollector& runtime_filter_collector() { return _runtime_filter_collector; }

    // Extract node id from p->name().
    static int get_node_id_from_profile(RuntimeProfile* p);

    // Names of counters shared by all exec nodes
    static const std::string ROW_THROUGHPUT_COUNTER;

protected:
    friend class DataSink;

    /// Extends blocking queue for row batches. Row batches have a property that
    /// they must be processed in the order they were produced, even in cancellation
    /// paths. Preceding row batches can contain ptrs to memory in subsequent row batches
    /// and we need to make sure those ptrs stay valid.
    /// Row batches that are added after Shutdown() are queued in another queue, which can
    /// be cleaned up during Close().
    /// All functions are thread safe.
    class RowBatchQueue : public TimedBlockingQueue<RowBatch*> {
    public:
        /// max_batches is the maximum number of row batches that can be queued.
        /// When the queue is full, producers will block.
        RowBatchQueue(int max_batches);
        ~RowBatchQueue();

        /// Adds a batch to the queue. This is blocking if the queue is full.
        void AddBatch(RowBatch* batch);

        /// Adds a batch to the queue. If the queue is full, this blocks until space becomes
        /// available or 'timeout_micros' has elapsed.
        /// Returns true if the element was added to the queue, false if it wasn't. If this
        /// method returns false, the queue didn't take ownership of the batch and it must be
        /// managed externally.
        bool AddBatchWithTimeout(RowBatch* batch, int64_t timeout_micros);

        /// Gets a row batch from the queue. Returns NULL if there are no more.
        /// This function blocks.
        /// Returns NULL after Shutdown().
        RowBatch* GetBatch();

        /// Deletes all row batches in cleanup_queue_. Not valid to call AddBatch()
        /// after this is called.
        /// Returns the number of io buffers that were released (for debug tracking)
        int Cleanup();

    private:
        /// Lock protecting cleanup_queue_
        // SpinLock lock_;
        // TODO(dhc): need to modify spinlock
        std::mutex lock_;

        /// Queue of orphaned row batches
        std::list<RowBatch*> cleanup_queue_;
    };

    int _id; // unique w/in single plan tree
    TPlanNodeType::type _type;
    ObjectPool* _pool;
    std::vector<Expr*> _conjuncts;
    std::vector<ExprContext*> _conjunct_ctxs;
    std::vector<TupleId> _tuple_ids;

    vectorized::RuntimeFilterProbeCollector _runtime_filter_collector;
    std::vector<SlotId> _filter_null_value_columns;

    std::vector<ExecNode*> _children;
    RowDescriptor _row_descriptor;

    /// Resource information sent from the frontend.
    const TBackendResourceProfile _resource_profile;

    // debug-only: if _debug_action is not INVALID, node will perform action in
    // _debug_phase
    TExecNodePhase::type _debug_phase;
    TDebugAction::type _debug_action;

    int64_t _limit; // -1: no limit
    int64_t _num_rows_returned;

    std::shared_ptr<RuntimeProfile> _runtime_profile;

    /// Account for peak memory used by this node
    std::shared_ptr<MemTracker> _mem_tracker;

    RuntimeProfile::Counter* _rows_returned_counter;
    RuntimeProfile::Counter* _rows_returned_rate;
    // Account for peak memory used by this node
    RuntimeProfile::Counter* _memory_used_counter;

    bool _use_vectorized;

    ExecNode* child(int i) { return _children[i]; }

    bool is_closed() const { return _is_closed; }

    // TODO(zc)
    /// Pointer to the containing SubplanNode or NULL if not inside a subplan.
    /// Set by SubplanNode::Init(). Not owned.
    // SubplanNode* containing_subplan_;

    /// Returns true if this node is inside the right-hand side plan tree of a SubplanNode.
    /// Valid to call in or after Prepare().
    bool is_in_subplan() const { return false; }

    static Status create_vectorized_node(RuntimeState* state, ObjectPool* pool, const TPlanNode& tnode,
                                         const DescriptorTbl& descs, ExecNode** node);

    static Status create_tree_helper(RuntimeState* state, ObjectPool* pool, const std::vector<TPlanNode>& tnodes,
                                     const DescriptorTbl& descs, ExecNode* parent, int* node_idx, ExecNode** root);

    virtual bool is_scan_node() const { return false; }

    void init_runtime_profile(const std::string& name);

    RuntimeState* runtime_state() { return _runtime_state; }

    // Executes _debug_action if phase matches _debug_phase.
    // 'phase' must not be INVALID.
    Status exec_debug_action(TExecNodePhase::type phase);

private:
    RuntimeState* _runtime_state;
    bool _is_closed;
};
} // namespace starrocks

#endif

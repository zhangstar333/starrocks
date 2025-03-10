// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/planner/SingleNodePlanner.java

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

package com.starrocks.planner;

import com.google.common.base.Preconditions;
import com.google.common.base.Predicate;
import com.google.common.collect.Iterables;
import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.starrocks.analysis.AggregateInfo;
import com.starrocks.analysis.AnalyticInfo;
import com.starrocks.analysis.Analyzer;
import com.starrocks.analysis.AssertNumRowsElement;
import com.starrocks.analysis.BaseTableRef;
import com.starrocks.analysis.BinaryPredicate;
import com.starrocks.analysis.CaseExpr;
import com.starrocks.analysis.CastExpr;
import com.starrocks.analysis.DescriptorTable;
import com.starrocks.analysis.Expr;
import com.starrocks.analysis.ExprSubstitutionMap;
import com.starrocks.analysis.FunctionCallExpr;
import com.starrocks.analysis.GroupByClause;
import com.starrocks.analysis.GroupingInfo;
import com.starrocks.analysis.InPredicate;
import com.starrocks.analysis.InlineViewRef;
import com.starrocks.analysis.IsNullPredicate;
import com.starrocks.analysis.JoinOperator;
import com.starrocks.analysis.LiteralExpr;
import com.starrocks.analysis.NullLiteral;
import com.starrocks.analysis.QueryStmt;
import com.starrocks.analysis.SelectStmt;
import com.starrocks.analysis.SetOperationStmt;
import com.starrocks.analysis.SlotDescriptor;
import com.starrocks.analysis.SlotId;
import com.starrocks.analysis.SlotRef;
import com.starrocks.analysis.TableRef;
import com.starrocks.analysis.TupleDescriptor;
import com.starrocks.analysis.TupleId;
import com.starrocks.analysis.TupleIsNullPredicate;
import com.starrocks.catalog.AggregateFunction;
import com.starrocks.catalog.AggregateType;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.FunctionSet;
import com.starrocks.catalog.MysqlTable;
import com.starrocks.catalog.Table;
import com.starrocks.common.AnalysisException;
import com.starrocks.common.FeConstants;
import com.starrocks.common.IdGenerator;
import com.starrocks.common.Reference;
import com.starrocks.common.UserException;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;

/**
 * Constructs a non-executable single-node plan from an analyzed parse tree.
 * The single-node plan does not contain data exchanges or data-reduction optimizations
 * such as local aggregations that are important for distributed execution.
 * The single-node plan needs to be wrapped in a plan fragment for it to be executable.
 */

// Our new cost based query optimizer is more powerful and stable than old query optimizer,
// The old query optimizer related codes could be deleted safely.
// TODO: Remove old query optimizer related codes before 2021-09-30
@Deprecated
public class SingleNodePlanner {
    private static final Logger LOG = LogManager.getLogger(DistributedPlanner.class);

    private final PlannerContext ctx_;
    private final ArrayList<ScanNode> scanNodes = Lists.newArrayList();
    private Map<Analyzer, List<ScanNode>> selectStmtToScanNodes = Maps.newHashMap();

    public SingleNodePlanner(PlannerContext ctx) {
        ctx_ = ctx;
    }

    public ArrayList<ScanNode> getScanNodes() {
        return scanNodes;
    }

    private final IdGenerator<RuntimeFilterId> runtimeFilterIdIdGenerator = RuntimeFilterId.createGenerator();

    /**
     * Generates and returns the root of the single-node plan for the analyzed parse tree
     * in the planner context. The planning process recursively walks the parse tree and
     * performs the following actions.
     * In the top-down phase over query statements:
     * - Materialize the slots required for evaluating expressions of that statement.
     * - Migrate conjuncts from parent blocks into inline views and union operands.
     * In the bottom-up phase generate the plan tree for every query statement:
     * - Generate the plan for the FROM-clause of a select statement: The plan trees of
     * absolute and uncorrelated table refs are connected via JoinNodes. The relative
     * and correlated table refs are associated with one or more SubplanNodes.
     * - A new SubplanNode is placed on top of an existing plan node whenever the tuples
     * materialized by that plan node enable evaluation of one or more relative or
     * correlated table refs, i.e., SubplanNodes are placed at the lowest possible point
     * in the plan, often right after a ScanNode materializing the (single) parent tuple.
     * - The right-hand side of each SubplanNode is a plan tree generated by joining a
     * SingularRowSrcTableRef with those applicable relative and correlated refs.
     * A SingularRowSrcTableRef represents the current row being processed by the
     * SubplanNode from its input (first child).
     * - Connecting table ref plans via JoinNodes is done in a cost-based fashion
     * (join-order optimization). All materialized slots, including those of tuples
     * materialized inside a SubplanNode, must be known for an accurate estimate of row
     * sizes needed for cost-based join ordering.
     * - The remaining aggregate/analytic/orderby portions of a select statement are added
     * on top of the FROM-clause plan.
     * - Whenever a new node is added to the plan tree, assign conjuncts that can be
     * evaluated at that node and compute the stats of that node (cardinality, etc.).
     * - Apply combined expression substitution map of child plan nodes; if a plan node
     * re-maps its input, set a substitution map to be applied by parents.
     */
    public PlanNode createSingleNodePlan() throws UserException, AnalysisException {
        QueryStmt queryStmt = ctx_.getQueryStmt();
        // Use the stmt's analyzer which is not necessarily the root analyzer
        // to detect empty result sets.
        Analyzer analyzer = queryStmt.getAnalyzer();
        // TODO(zc)
        // analyzer.computeEquivClasses();
        // ctx_.getAnalysisResult().getTimeline().markEvent("Equivalence classes computed");

        // Mark slots referenced by output exprs as materialized, prior to generating the
        // plan tree.
        // We need to mark the result exprs of the topmost select block as materialized, so
        // that PlanNode.init() can compute the final mem layout of materialized tuples
        // (the byte size of tuples is needed for cost computations).
        // TODO: instead of materializing everything produced by the plan root, derive
        // referenced slots from destination fragment and add a materialization node
        // if not all output is needed by destination fragment
        // TODO 2: should the materialization decision be cost-based?
        if (queryStmt.getBaseTblResultExprs() != null) {
            analyzer.materializeSlots(queryStmt.getBaseTblResultExprs());
        }

        if (LOG.isTraceEnabled()) {
            LOG.trace("desctbl: " + analyzer.getDescTbl().debugString());
        }
        PlanNode singleNodePlan = createQueryPlan(queryStmt, analyzer);
        Preconditions.checkNotNull(singleNodePlan);

        return singleNodePlan;
    }

    /**
     * Creates an EmptyNode that 'materializes' the tuples of the given stmt.
     * Marks all collection-typed slots referenced in stmt as non-materialized because
     * they are never unnested, and therefore the corresponding parent scan should not
     * materialize them.
     */
    private PlanNode createEmptyNode(QueryStmt stmt, Analyzer analyzer) {
        TupleDescriptor descriptor = createResultTupleDescriptor((SelectStmt) stmt, "empty", analyzer);
        ExprSubstitutionMap resultSmap = new ExprSubstitutionMap();
        List<Expr> resultExprs = stmt.getResultExprs();
        for (int i = 0; i < resultExprs.size(); ++i) {
            resultSmap.put(resultExprs.get(i), new SlotRef(descriptor.getSlots().get(i)));
        }

        unmarkCollectionSlots(stmt);
        EmptySetNode node = new EmptySetNode(ctx_.getNextNodeId(), descriptor.getId().asList());
        node.init(analyzer);
        // Set the output smap to resolve exprs referencing inline views within stmt.
        // Not needed for a UnionStmt because it materializes its input operands.
        node.setOutputSmap(resultSmap);
        return node;
    }

    /**
     * Mark all collection-typed slots in stmt as non-materialized.
     */
    private void unmarkCollectionSlots(QueryStmt stmt) {
        List<TableRef> tblRefs = Lists.newArrayList();
        stmt.collectTableRefs(tblRefs);
        for (TableRef ref : tblRefs) {
            if (!ref.isRelative()) {
                continue;
            }
            // Preconditions.checkState(ref instanceof CollectionTableRef);
            // CollectionTableRef collTblRef = (CollectionTableRef) ref;
            // Expr collExpr = collTblRef.getCollectionExpr();
            // Preconditions.checkState(collExpr instanceof SlotRef);
            // SlotRef collSlotRef = (SlotRef) collExpr;
            // collSlotRef.getDesc().setIsMaterialized(false);
            // collSlotRef.getDesc().getParent().recomputeMemLayout();
        }
    }

    /**
     * Create plan tree for single-node execution. Generates PlanNodes for the
     * Select/Project/Join/Union [All]/Group by/Having/Order by clauses of the query stmt.
     */
    private PlanNode createQueryPlan(QueryStmt stmt, Analyzer analyzer)
            throws UserException {
        if (analyzer.hasEmptyResultSet()) {
            return createEmptyNode(stmt, analyzer);
        }
        PlanNode root;
        if (stmt instanceof SelectStmt) {
            SelectStmt selectStmt = (SelectStmt) stmt;
            pushDownPredicates(analyzer, selectStmt);

            root = createSelectPlan(selectStmt, analyzer);

            // TODO(zc)
            // insert possible AnalyticEvalNode before SortNode
            if (selectStmt.getAnalyticInfo() != null) {
                AnalyticInfo analyticInfo = selectStmt.getAnalyticInfo();
                AnalyticPlanner analyticPlanner = new AnalyticPlanner(analyticInfo, analyzer, ctx_);
                List<Expr> inputPartitionExprs = Lists.newArrayList();
                AggregateInfo aggInfo = selectStmt.getAggInfo();
                root = analyticPlanner.createSingleNodePlan(root,
                        aggInfo != null ? aggInfo.getGroupingExprs() : null, inputPartitionExprs);
                if (aggInfo != null && !inputPartitionExprs.isEmpty()) {
                    // analytic computation will benefit from a partition on inputPartitionExprs
                    aggInfo.setPartitionExprs(inputPartitionExprs);
                }
            }
        } else {
            Preconditions.checkState(stmt instanceof SetOperationStmt);
            root = createSetOperationPlan((SetOperationStmt) stmt, analyzer);
        }

        // Avoid adding a sort node if the sort tuple has no materialized slots.
        boolean sortHasMaterializedSlots = false;
        if (stmt.evaluateOrderBy()) {
            for (SlotDescriptor sortSlotDesc :
                    stmt.getSortInfo().getSortTupleDescriptor().getSlots()) {
                if (sortSlotDesc.isMaterialized()) {
                    sortHasMaterializedSlots = true;
                    break;
                }
            }
        }

        if (stmt.evaluateOrderBy() && sortHasMaterializedSlots) {
            long limit = stmt.getLimit();
            // TODO: External sort could be used for very large limits
            // not just unlimited order-by
            boolean useTopN = true;
            if (limit == -1) {
                useTopN = false;
            }
            root = new SortNode(ctx_.getNextNodeId(), root, stmt.getSortInfo(),
                    useTopN, limit == -1, stmt.getOffset());
            root.setLimit(limit);
            Preconditions.checkState(root.hasValidStats());
            root.init(analyzer);
            // TODO chenhao, before merge ValueTransferGraph, force evaluate conjuncts
            // from SelectStmt outside
            root = addUnassignedConjuncts(analyzer, root);
        } else {
            root.setLimit(stmt.getLimit());
            root.computeStats(analyzer);
        }

        // adding assert node at the end of single node planner
        if (stmt.getAssertNumRowsElement() != null) {
            root = createAssertRowCountNode(root, stmt.getAssertNumRowsElement(), analyzer);
        }
        return root;
    }

    /**
     * If there are unassigned conjuncts that are bound by tupleIds or if there are slot
     * equivalences for tupleIds that have not yet been enforced, returns a SelectNode on
     * top of root that evaluates those conjuncts; otherwise returns root unchanged.
     * TODO: change this to assign the unassigned conjuncts to root itself, if that is
     * semantically correct
     */
    private PlanNode addUnassignedConjuncts(Analyzer analyzer, PlanNode root)
            throws UserException {
        Preconditions.checkNotNull(root);
        // List<Expr> conjuncts = analyzer.getUnassignedConjuncts(root.getTupleIds());

        List<Expr> conjuncts = analyzer.getUnassignedConjuncts(root);
        if (conjuncts.isEmpty()) {
            return root;
        }
        // evaluate conjuncts in SelectNode
        SelectNode selectNode = new SelectNode(ctx_.getNextNodeId(), root, conjuncts);
        selectNode.init(analyzer);
        Preconditions.checkState(selectNode.hasValidStats());
        return selectNode;
    }

    private PlanNode addUnassignedConjuncts(
            Analyzer analyzer, List<TupleId> tupleIds, PlanNode root) throws UserException {
        // No point in adding SelectNode on top of an EmptyNode.
        if (root instanceof EmptySetNode) {
            return root;
        }
        Preconditions.checkNotNull(root);
        // Gather unassigned conjuncts and generate predicates to enfore
        // slot equivalences for each tuple id.
        List<Expr> conjuncts = analyzer.getUnassignedConjuncts(root);
        if (conjuncts.isEmpty()) {
            return root;
        }
        // evaluate conjuncts in SelectNode
        SelectNode selectNode = new SelectNode(ctx_.getNextNodeId(), root, conjuncts);
        // init() marks conjuncts as assigned
        selectNode.init(analyzer);
        Preconditions.checkState(selectNode.hasValidStats());
        return selectNode;
    }

    private void turnOffPreAgg(AggregateInfo aggInfo, SelectStmt selectStmt, Analyzer analyzer, PlanNode root) {
        String turnOffReason = null;
        do {
            if (!(root instanceof OlapScanNode)) {
                turnOffReason = "left-deep Node is not OlapScanNode";
                break;
            }

            if (((OlapScanNode) root).getForceOpenPreAgg()) {
                ((OlapScanNode) root).setIsPreAggregation(true, "");
                return;
            }

            if (null == aggInfo) {
                turnOffReason = "No AggregateInfo";
                break;
            }

            ArrayList<FunctionCallExpr> aggExprs = aggInfo.getAggregateExprs();
            // multi table join
            boolean aggTableValidate = true;
            if (selectStmt.getTableRefs().size() > 1) {
                for (int i = 1; i < selectStmt.getTableRefs().size(); ++i) {
                    final JoinOperator joinOperator = selectStmt.getTableRefs().get(i).getJoinOp();
                    // TODO chenhao , right out join ?
                    if (joinOperator.isRightOuterJoin() || joinOperator.isFullOuterJoin()) {
                        turnOffReason = selectStmt.getTableRefs().get(i) +
                                " joinOp is full outer join or right outer join.";
                        aggTableValidate = false;
                        break;
                    }

                    // If root scan node is cross join child and table is aggregate table, disable pre aggregate
                    // Join type is inner and no conjuncts means cross join
                    if (i == 1 && joinOperator.isInnerJoin()
                            && analyzer.getAllConjuncts(selectStmt.getTableRefs().get(0).getId()) == null) {
                        OlapScanNode scanNode = (OlapScanNode) root;
                        if (scanNode.getOlapTable().getKeysType().isAggregationFamily()) {
                            turnOffReason = "joinOp is cross join";
                            aggTableValidate = false;
                        }
                    }
                }

                if (!aggTableValidate) {
                    break;
                }
                for (FunctionCallExpr aggExpr : aggExprs) {
                    TableRef olapTableRef = selectStmt.getTableRefs().get(0);
                    if (Expr.isBound(Lists.newArrayList(aggExpr), Lists.newArrayList(olapTableRef.getId()))) {
                        // do nothing
                        LOG.debug("All agg exprs is bound to olapTable: {}" + olapTableRef.getTable().getName());
                    } else {
                        List<TupleId> tupleIds = Lists.newArrayList();
                        List<SlotId> slotIds = Lists.newArrayList();
                        aggExpr.getIds(tupleIds, slotIds);
                        for (TupleId tupleId : tupleIds) {
                            // if tupleid is agg's result tuple, there is no tableref
                            // for only scanNode has the tableref
                            if (analyzer.getTupleDesc(tupleId).getRef() == null) {
                                aggTableValidate = false;
                                break;
                            }

                            if (analyzer.getTupleDesc(tupleId).getRef() != olapTableRef) {
                                if (analyzer.getTupleDesc(tupleId).getTable() != null
                                        && analyzer.getTupleDesc(tupleId).getTable().getType()
                                        == Table.TableType.OLAP) {
                                    turnOffReason = "agg expr [" + aggExpr.debugString() + "] is not bound ["
                                            + selectStmt.getTableRefs().get(0).toSql() + "]";
                                    aggTableValidate = false;
                                } else {
                                    LOG.debug("The table which agg expr [{}] is bound to, is not OLAP table [{}]",
                                            aggExpr.debugString(),
                                            analyzer.getTupleDesc(tupleId).getTable() == null ? "inline view" :
                                                    analyzer.getTupleDesc(tupleId).getTable().getName());
                                }
                            }

                        }
                    }
                }

                if (!aggTableValidate) {
                    break;
                }
            }

            boolean valueColumnValidate = true;
            List<Expr> allConjuncts = analyzer.getAllConjuncts(selectStmt.getTableRefs().get(0).getId());
            List<SlotId> conjunctSlotIds = Lists.newArrayList();
            if (allConjuncts != null) {
                for (Expr conjunct : allConjuncts) {
                    conjunct.getIds(null, conjunctSlotIds);
                }
                for (SlotDescriptor slot : selectStmt.getTableRefs().get(0).getDesc().getSlots()) {
                    if (!slot.getColumn().isKey()) {
                        if (conjunctSlotIds.contains(slot.getId())) {
                            turnOffReason = "conjunct on `" + slot.getColumn().getName() +
                                    "` which is StorageEngine value column";
                            valueColumnValidate = false;
                            break;
                        }
                    }
                }
            }
            if (!valueColumnValidate) {
                break;
            }

            boolean aggExprValidate = true;
            for (FunctionCallExpr aggExpr : aggExprs) {
                if (aggExpr.getChildren().size() != 1) {
                    turnOffReason = "aggExpr has more than one child";
                    aggExprValidate = false;
                    break;
                }

                List<Column> returnColumns = Lists.newArrayList();
                List<Column> conditionColumns = Lists.newArrayList();
                if (!(aggExpr.getChild(0) instanceof SlotRef)) {
                    Expr child = aggExpr.getChild(0);
                    if ((child instanceof CastExpr) && (child.getChild(0) instanceof SlotRef)) {
                        if (child.getType().isNumericType()
                                && child.getChild(0).getType().isNumericType()) {
                            returnColumns.add(((SlotRef) child.getChild(0)).getDesc().getColumn());
                        } else {
                            turnOffReason =
                                    "aggExpr.getChild(0)[" + aggExpr.getChild(0).toSql() + "] is not Numeric CastExpr";
                            aggExprValidate = false;
                            break;
                        }
                    } else if (aggExpr.getChild(0) instanceof CaseExpr) {
                        CaseExpr caseExpr = (CaseExpr) aggExpr.getChild(0);
                        List<Expr> conditionExprs = caseExpr.getConditionExprs();
                        for (Expr conditionExpr : conditionExprs) {
                            List<TupleId> conditionTupleIds = Lists.newArrayList();
                            List<SlotId> conditionSlotIds = Lists.newArrayList();
                            conditionExpr.getIds(conditionTupleIds, conditionSlotIds);

                            for (SlotId conditionSlotId : conditionSlotIds) {
                                DescriptorTable descTable = analyzer.getDescTbl();
                                SlotDescriptor slotDesc = descTable.getSlotDesc(conditionSlotId);
                                conditionColumns.add(analyzer.getDescTbl().getSlotDesc(conditionSlotId).getColumn());
                            }
                        }

                        boolean caseReturnExprValidate = true;
                        List<Expr> returnExprs = caseExpr.getReturnExprs();
                        for (Expr returnExpr : returnExprs) {
                            if (returnExpr instanceof SlotRef) {
                                returnColumns.add(((SlotRef) returnExpr).getDesc().getColumn());
                            } else if (returnExpr instanceof NullLiteral) {
                                // do nothing
                            } else {
                                turnOffReason = "aggExpr.getChild(0)[" + aggExpr.getChild(0).toSql()
                                        + "] is not SlotExpr";
                                caseReturnExprValidate = false;
                                break;
                            }
                        }

                        if (!caseReturnExprValidate) {
                            aggExprValidate = false;
                            break;
                        }

                    } else {
                        turnOffReason = "aggExpr.getChild(0)[" + aggExpr.getChild(0).debugString()
                                + "] is not SlotRef or CastExpr|CaseExpr";
                        aggExprValidate = false;
                        break;
                    }
                } else {
                    returnColumns.add(((SlotRef) aggExpr.getChild(0)).getDesc().getColumn());
                }

                // check condition columns
                boolean conditionColumnValidate = true;
                for (Column col : conditionColumns) {
                    // TODO(zc): Here column is null is too bad
                    // Only column of Inline-view will be null
                    if (col == null) {
                        continue;
                    }
                    if (!col.isKey()) {
                        turnOffReason = "the condition column [" + col.getName() + "] is not key type in aggr expr ["
                                + aggExpr.toSql() + "].";
                        conditionColumnValidate = false;
                        break;
                    }
                }
                if (!conditionColumnValidate) {
                    aggExprValidate = false;
                    break;
                }

                // check return columns
                boolean returnColumnValidate = true;
                for (Column col : returnColumns) {
                    // TODO(zc): Here column is null is too bad
                    // Only column of Inline-view will be null
                    if (col == null) {
                        continue;
                    }
                    if (col.isKey()) {
                        if (aggExpr.getFnName().getFunction().equalsIgnoreCase("MAX")
                                && aggExpr.getFnName().getFunction().equalsIgnoreCase("MIN")) {
                            returnColumnValidate = false;
                            turnOffReason = "the type of agg on StorageEngine's Key column should only be MAX or MIN."
                                    + "agg expr: " + aggExpr.toSql();
                            break;
                        }
                    }

                    if (aggExpr.getFnName().getFunction().equalsIgnoreCase("SUM")) {
                        if (col.getAggregationType() != AggregateType.SUM) {
                            turnOffReason = "Aggregate Operator not match: SUM <--> " + col.getAggregationType();
                            returnColumnValidate = false;
                            break;
                        }
                    } else if (aggExpr.getFnName().getFunction().equalsIgnoreCase("MAX")) {
                        if ((!col.isKey()) && col.getAggregationType() != AggregateType.MAX) {
                            turnOffReason = "Aggregate Operator not match: MAX <--> " + col.getAggregationType();
                            returnColumnValidate = false;
                            break;
                        }
                    } else if (aggExpr.getFnName().getFunction().equalsIgnoreCase("MIN")) {
                        if ((!col.isKey()) && col.getAggregationType() != AggregateType.MIN) {
                            turnOffReason = "Aggregate Operator not match: MIN <--> " + col.getAggregationType();
                            returnColumnValidate = false;
                            break;
                        }
                    } else if (aggExpr.getFnName().getFunction().equalsIgnoreCase("HLL_UNION_AGG")) {
                        // do nothing
                    } else if (aggExpr.getFnName().getFunction().equalsIgnoreCase("HLL_RAW_AGG")) {
                        // do nothing
                    } else if (aggExpr.getFnName().getFunction().equalsIgnoreCase("NDV")) {
                        if ((!col.isKey())) {
                            turnOffReason = "NDV function with non-key column: " + col.getName();
                            returnColumnValidate = false;
                            break;
                        }
                    } else if (aggExpr.getFnName().getFunction().equalsIgnoreCase(FunctionSet.BITMAP_UNION_INT)) {
                        if ((!col.isKey())) {
                            turnOffReason = "BITMAP_UNION_INT function with non-key column: " + col.getName();
                            returnColumnValidate = false;
                            break;
                        }
                    } else if (aggExpr.getFnName().getFunction().equalsIgnoreCase(FunctionSet.BITMAP_UNION)
                            || aggExpr.getFnName().getFunction().equalsIgnoreCase(FunctionSet.BITMAP_UNION_COUNT)) {
                        if (col.getAggregationType() != AggregateType.BITMAP_UNION) {
                            turnOffReason =
                                    "Aggregate Operator not match: BITMAP_UNION <--> " + col.getAggregationType();
                            returnColumnValidate = false;
                            break;
                        }
                    } else if (aggExpr.getFnName().getFunction().equalsIgnoreCase("multi_distinct_count")) {
                        // count(distinct k1), count(distinct k2) / count(distinct k1,k2) can turn on pre aggregation
                        if ((!col.isKey())) {
                            turnOffReason = "Multi count or sum distinct with non-key column: " + col.getName();
                            returnColumnValidate = false;
                            break;
                        }
                    } else if (aggExpr.getFnName().getFunction().equalsIgnoreCase("percentile_union")) {
                        if (col.getAggregationType() != AggregateType.PERCENTILE_UNION) {
                            turnOffReason =
                                    "Aggregate Operator not match: PERCENTILE_UNION <--> " + col.getAggregationType();
                            returnColumnValidate = false;
                            break;
                        }
                    } else {
                        turnOffReason = "Invalid Aggregate Operator: " + aggExpr.getFnName().getFunction();
                        returnColumnValidate = false;
                        break;
                    }
                }
                if (!returnColumnValidate) {
                    aggExprValidate = false;
                    break;
                }
            }

            if (!aggExprValidate) {
                break;
            }

            boolean groupExprValidate = true;
            ArrayList<Expr> groupExprs = aggInfo.getGroupingExprs();
            for (Expr groupExpr : groupExprs) {
                List<SlotId> groupSlotIds = Lists.newArrayList();
                groupExpr.getIds(null, groupSlotIds);

                for (SlotDescriptor slot : selectStmt.getTableRefs().get(0).getDesc().getSlots()) {
                    if (!slot.getColumn().isKey()) {
                        if (groupSlotIds.contains(slot.getId())) {
                            turnOffReason = "groupExpr contains StorageEngine's Value";
                            groupExprValidate = false;
                            break;
                        }
                    }
                }
                if (!groupExprValidate) {
                    break;
                }
            }

            if (!groupExprValidate) {
                break;
            }

            OlapScanNode olapNode = (OlapScanNode) root;
            if (!olapNode.getCanTurnOnPreAggr()) {
                turnOffReason = "this olap scan node[" + olapNode.debugString()
                        + "] has already been turned off pre-aggregation.";
                break;
            }

            olapNode.setIsPreAggregation(true, null);
        } while (false);

        if ((root instanceof OlapScanNode) && turnOffReason != null) {
            ((OlapScanNode) root).setIsPreAggregation(false, turnOffReason);
        }
    }

    /**
     * Create tree of PlanNodes that implements the Select/Project/Join/Group by/Having
     * of the selectStmt query block.
     */
    private PlanNode createSelectPlan(SelectStmt selectStmt, Analyzer analyzer)
            throws UserException, AnalysisException {
        // no from clause -> nothing to plan
        if (selectStmt.getTableRefs().isEmpty()) {
            return createConstantSelectPlan(selectStmt, analyzer);
        }

        // Slot materialization:
        // We need to mark all slots as materialized that are needed during the execution
        // of selectStmt, and we need to do that prior to creating plans for the TableRefs
        // (because createTableRefNode() might end up calling computeMemLayout() on one or
        // more TupleDescriptors, at which point all referenced slots need to be marked).
        //
        // For non-join predicates, slots are marked as follows:
        // - for base table scan predicates, this is done directly by ScanNode.init(), which
        //   can do a better job because it doesn't need to materialize slots that are only
        //   referenced for partition pruning, for instance
        // - for inline views, non-join predicates are pushed down, at which point the
        //   process repeats itself.
        selectStmt.materializeRequiredSlots(analyzer);

        // collect ids of tuples materialized by the subtree that includes all joins
        // and scans
        ArrayList<TupleId> rowTuples = Lists.newArrayList();
        for (TableRef tblRef : selectStmt.getTableRefs()) {
            rowTuples.addAll(tblRef.getMaterializedTupleIds());
        }

        if (analyzer.hasEmptySpjResultSet() && selectStmt.getAggInfo() != null) {
            final PlanNode emptySetNode = new EmptySetNode(ctx_.getNextNodeId(), rowTuples);
            emptySetNode.init(analyzer);
            emptySetNode.setOutputSmap(selectStmt.getBaseTblSmap());
            return createAggregationPlan(selectStmt, analyzer, emptySetNode);
        }

        // create left-deep sequence of binary hash joins; assign node ids as we go along
        TableRef tblRef = selectStmt.getTableRefs().get(0);
        materializeTableResultForCrossJoinOrCountStar(tblRef, analyzer);
        PlanNode root = createTableRefNode(analyzer, tblRef, selectStmt);
        // to change the inner contains analytic function
        // selectStmt.seondSubstituteInlineViewExprs(analyzer.getChangeResSmap());

        // add aggregate node here
        AggregateInfo aggInfo = selectStmt.getAggInfo();

        turnOffPreAgg(aggInfo, selectStmt, analyzer, root);

        if (root instanceof OlapScanNode) {
            OlapScanNode olapNode = (OlapScanNode) root;
            // this olap scan node has been turn off pre-aggregation, should not be turned on again.
            // e.g. select sum(v1) from (select v1 from test_table);
            if (!olapNode.isPreAggregation()) {
                olapNode.setCanTurnOnPreAggr(false);
            }
        }

        for (int i = 1; i < selectStmt.getTableRefs().size(); ++i) {
            TableRef outerRef = selectStmt.getTableRefs().get(i - 1);
            TableRef innerRef = selectStmt.getTableRefs().get(i);
            root = createJoinNode(analyzer, root, outerRef, innerRef, selectStmt);
            root.assignConjuncts(analyzer);
        }

        if (root != null) {
            // add unassigned conjuncts before aggregation
            // (scenario: agg input comes from an inline view which wasn't able to
            // evaluate all Where clause conjuncts from this scope)
            if (!selectStmt.hasOrderByClause()) {
                root = addUnassignedConjuncts(analyzer, root);
            }
        }

        // add aggregation, if required
        if (aggInfo != null) {
            // introduce repeatNode for group by extension
            GroupByClause groupByClause = selectStmt.getGroupByClause();
            if (groupByClause != null && groupByClause.isGroupByExtension()) {
                root = createRepeatNodePlan(selectStmt, analyzer, root);
            }

            root = createAggregationPlan(selectStmt, analyzer, root);
        }

        return root;
    }

    /**
     * Returns a new RepeatNode.
     */
    private PlanNode createRepeatNodePlan(SelectStmt selectStmt, Analyzer analyzer,
                                          PlanNode root) throws UserException {
        GroupByClause groupByClause = selectStmt.getGroupByClause();
        GroupingInfo groupingInfo = selectStmt.getGroupingInfo();
        Preconditions.checkState(groupByClause != null && groupByClause.isGroupByExtension()
                && groupingInfo != null);
        root = new RepeatNode(ctx_.getNextNodeId(), root, groupingInfo, groupByClause);
        root.init(analyzer);
        return root;
    }

    public boolean selectMaterializedView(QueryStmt queryStmt, Analyzer analyzer)
            throws UserException {
        boolean selectFailed = false;
        if (queryStmt instanceof SelectStmt) {
            SelectStmt selectStmt = (SelectStmt) queryStmt;
            for (TableRef tableRef : selectStmt.getTableRefs()) {
                if (tableRef instanceof InlineViewRef) {
                    selectFailed |= selectMaterializedView(((InlineViewRef) tableRef).getViewStmt(),
                            ((InlineViewRef) tableRef).getAnalyzer());
                }
            }
            List<ScanNode> scanNodeList = selectStmtToScanNodes.get(selectStmt.getAnalyzer());
            if (scanNodeList == null) {
                return selectFailed;
            }
            MaterializedViewSelector materializedViewSelector = new MaterializedViewSelector(selectStmt, analyzer);
            for (ScanNode scanNode : scanNodeList) {
                if (!(scanNode instanceof OlapScanNode)) {
                    continue;
                }
                OlapScanNode olapScanNode = (OlapScanNode) scanNode;
                if (olapScanNode.getSelectedPartitionIds().size() == 0 && !FeConstants.runningUnitTest) {
                    continue;
                }
                // select index by the old Rollup selector
                olapScanNode.selectBestRollupByRollupSelector(analyzer);
                // select index by the new Materialized selector
                MaterializedViewSelector.BestIndexInfo bestIndexInfo =
                        materializedViewSelector.selectBestMV(olapScanNode);
                if (bestIndexInfo == null) {
                    selectFailed = true;
                    TupleId tupleId = olapScanNode.getTupleId();
                    selectStmt.updateDisableTuplesMVRewriter(tupleId);
                    LOG.debug("MV rewriter of tuple {} will be disable", tupleId);
                    continue;
                }
                // if the new selected index id is different from the old one, scan node will be updated.
                olapScanNode.updateScanRangeInfoByNewMVSelector(bestIndexInfo.getBestIndexId(),
                        bestIndexInfo.isPreAggregation(), bestIndexInfo.getReasonOfDisable());
                if (selectStmt.getAggInfo() != null) {
                    selectStmt.getAggInfo().updateTypeOfAggregateExprs();
                }
            }

        } else {
            Preconditions.checkState(queryStmt instanceof SetOperationStmt);
            SetOperationStmt unionStmt = (SetOperationStmt) queryStmt;
            for (SetOperationStmt.SetOperand unionOperand : unionStmt.getOperands()) {
                selectFailed |= selectMaterializedView(unionOperand.getQueryStmt(), analyzer);
            }
        }
        return selectFailed;
    }

    /**
     * Returns a new AggregationNode that materializes the aggregation of the given stmt.
     * Assigns conjuncts from the Having clause to the returned node.
     */
    private PlanNode createAggregationPlan(SelectStmt selectStmt, Analyzer analyzer,
                                           PlanNode root) throws UserException {
        Preconditions.checkState(selectStmt.getAggInfo() != null);
        // add aggregation, if required
        AggregateInfo aggInfo = selectStmt.getAggInfo();
        // aggInfo.substitueGroupingExpr(analyzer);
        PlanNode newRoot = new AggregationNode(ctx_.getNextNodeId(), root, aggInfo);
        newRoot.init(analyzer);
        Preconditions.checkState(newRoot.hasValidStats());
        // if we're computing DISTINCT agg fns, the analyzer already created the
        // 2nd phase agginfo
        if (aggInfo.isDistinctAgg()) {
            ((AggregationNode) newRoot).unsetNeedsFinalize();
            // The output of the 1st phase agg is the 1st phase intermediate.
            ((AggregationNode) newRoot).setIntermediateTuple();
            newRoot = new AggregationNode(ctx_.getNextNodeId(), newRoot,
                    aggInfo.getSecondPhaseDistinctAggInfo());
            newRoot.init(analyzer);
            Preconditions.checkState(newRoot.hasValidStats());
        }
        // add Having clause
        newRoot.assignConjuncts(analyzer);
        return newRoot;
    }

    /**
     * Returns a MergeNode that materializes the exprs of the constant selectStmt. Replaces the resultExprs of the
     * selectStmt with SlotRefs into the materialized tuple.
     */
    private PlanNode createConstantSelectPlan(SelectStmt selectStmt, Analyzer analyzer) {
        Preconditions.checkState(selectStmt.getTableRefs().isEmpty());
        ArrayList<Expr> resultExprs = selectStmt.getResultExprs();
        // Create tuple descriptor for materialized tuple.
        TupleDescriptor tupleDesc = createResultTupleDescriptor(selectStmt, "union", analyzer);
        UnionNode unionNode = new UnionNode(ctx_.getNextNodeId(), tupleDesc.getId());

        // Analysis guarantees that selects without a FROM clause only have constant exprs.
        if (selectStmt.getValueList() != null) {
            for (ArrayList<Expr> row : selectStmt.getValueList().getRows()) {
                unionNode.addConstExprList(row);
            }
        } else {
            unionNode.addConstExprList(Lists.newArrayList(resultExprs));
        }

        // Replace the select stmt's resultExprs with SlotRefs into tupleDesc.
        for (int i = 0; i < resultExprs.size(); ++i) {
            SlotRef slotRef = new SlotRef(tupleDesc.getSlots().get(i));
            resultExprs.set(i, slotRef);
            selectStmt.getBaseTblResultExprs().set(i, slotRef);
        }

        // UnionNode.init() needs tupleDesc to have been initialized
        unionNode.init(analyzer);
        return unionNode;
    }

    /**
     * Create tuple descriptor that can hold the results of the given SelectStmt, with one
     * slot per result expr.
     */
    private TupleDescriptor createResultTupleDescriptor(SelectStmt selectStmt,
                                                        String debugName, Analyzer analyzer) {
        TupleDescriptor tupleDesc = analyzer.getDescTbl().createTupleDescriptor(
                debugName);
        tupleDesc.setIsMaterialized(true);

        List<Expr> resultExprs = selectStmt.getResultExprs();
        List<String> colLabels = selectStmt.getColLabels();
        for (int i = 0; i < resultExprs.size(); ++i) {
            Expr resultExpr = resultExprs.get(i);
            String colLabel = colLabels.get(i);
            SlotDescriptor slotDesc = analyzer.addSlotDescriptor(tupleDesc);
            slotDesc.setLabel(colLabel);
            slotDesc.setSourceExpr(resultExpr);
            slotDesc.setType(resultExpr.getType());
            // slotDesc.setStats(ColumnStats.fromExpr(resultExpr));
            slotDesc.setIsMaterialized(true);
        }
        tupleDesc.computeMemLayout();
        return tupleDesc;
    }

    // no need to remove?
    private PartitionColumnFilter createPartitionFilter(SlotDescriptor desc, List<Expr> conjuncts) {
        PartitionColumnFilter partitionColumnFilter = null;
        for (Expr expr : conjuncts) {
            if (!expr.isBound(desc.getId())) {
                continue;
            }
            if (expr instanceof BinaryPredicate) {
                BinaryPredicate binPredicate = (BinaryPredicate) expr;
                Expr slotBinding = binPredicate.getSlotBinding(desc.getId());
                if (slotBinding == null || !slotBinding.isConstant()) {
                    continue;
                }
                if (binPredicate.getOp() == BinaryPredicate.Operator.NE
                        || !(slotBinding instanceof LiteralExpr)) {
                    continue;
                }

                if (null == partitionColumnFilter) {
                    partitionColumnFilter = new PartitionColumnFilter();
                }
                LiteralExpr literal = (LiteralExpr) slotBinding;
                BinaryPredicate.Operator op = binPredicate.getOp();
                if (!binPredicate.slotIsLeft()) {
                    op = op.commutative();
                }
                switch (op) {
                    case EQ:
                        partitionColumnFilter.setLowerBound(literal, true);
                        partitionColumnFilter.setUpperBound(literal, true);
                        break;
                    case LE:
                        partitionColumnFilter.setUpperBound(literal, true);
                        partitionColumnFilter.lowerBoundInclusive = true;
                        break;
                    case LT:
                        partitionColumnFilter.setUpperBound(literal, false);
                        partitionColumnFilter.lowerBoundInclusive = true;
                        break;
                    case GE:
                        partitionColumnFilter.setLowerBound(literal, true);
                        break;
                    case GT:
                        partitionColumnFilter.setLowerBound(literal, false);
                        break;
                    default:
                        break;
                }
            } else if (expr instanceof InPredicate) {
                InPredicate inPredicate = (InPredicate) expr;
                if (!inPredicate.isLiteralChildren() || inPredicate.isNotIn()) {
                    continue;
                }
                if (inPredicate.getChild(0).unwrapExpr(false) instanceof LiteralExpr) {
                    // If child(0) of the in predicate is a constant expression,
                    // then other children of in predicate should not be used as a condition for partition prune.
                    // Such as "where  'Hi' in ('Hi', 'hello') and ... "
                    continue;
                }
                if (!(inPredicate.getChild(0).unwrapExpr(false) instanceof SlotRef)) {
                    continue;
                }
                if (null == partitionColumnFilter) {
                    partitionColumnFilter = new PartitionColumnFilter();
                }
                partitionColumnFilter.setInPredicate(inPredicate);
            } else if (expr instanceof IsNullPredicate) {
                IsNullPredicate isNullPredicate = (IsNullPredicate) expr;
                if (!isNullPredicate.isSlotRefChildren() || isNullPredicate.isNotNull()) {
                    continue;
                }

                // If we meet a IsNull predicate on partition column, then other predicates are useless
                // eg: (xxxx) and (col is null), only the IsNull predicate has an effect on partition pruning.
                partitionColumnFilter = new PartitionColumnFilter();
                NullLiteral nullLiteral = new NullLiteral();
                partitionColumnFilter.setLowerBound(nullLiteral, true);
                partitionColumnFilter.setUpperBound(nullLiteral, true);
                break;
            }
        }
        LOG.debug("partitionColumnFilter: {}", partitionColumnFilter);
        return partitionColumnFilter;
    }

    /**
     * Returns plan tree for an inline view ref:
     * - predicates from the enclosing scope that can be evaluated directly within
     * the inline-view plan are pushed down
     * - predicates that cannot be evaluated directly within the inline-view plan
     * but only apply to the inline view are evaluated in a SelectNode placed
     * on top of the inline view plan
     * - all slots that are referenced by predicates from the enclosing scope that cannot
     * be pushed down are marked as materialized (so that when computeMemLayout() is
     * called on the base table descriptors materialized by the inline view it has a
     * complete picture)
     */
    private PlanNode createInlineViewPlan(Analyzer analyzer, InlineViewRef inlineViewRef)
            throws UserException, AnalysisException {
        // If possible, "push down" view predicates; this is needed in order to ensure
        // that predicates such as "x + y = 10" are evaluated in the view's plan tree
        // rather than a SelectNode grafted on top of that plan tree.
        // This doesn't prevent predicate propagation, because predicates like
        // "x = 10" that get pushed down are still connected to equivalent slots
        // via the equality predicates created for the view's select list.
        // Include outer join conjuncts here as well because predicates from the
        // On-clause of an outer join may be pushed into the inline view as well.
        migrateConjunctsToInlineView(analyzer, inlineViewRef);

        // Turn a constant select into a MergeNode that materializes the exprs.
        QueryStmt viewStmt = inlineViewRef.getViewStmt();
        if (viewStmt instanceof SelectStmt) {
            SelectStmt selectStmt = (SelectStmt) viewStmt;
            if (selectStmt.getTableRefs().isEmpty()) {
                if (inlineViewRef.getAnalyzer().hasEmptyResultSet()) {
                    PlanNode emptySetNode = createEmptyNode(viewStmt, inlineViewRef.getAnalyzer());
                    // Still substitute exprs in parent nodes with the inline-view's smap to make
                    // sure no exprs reference the non-materialized inline view slots. No wrapping
                    // with TupleIsNullPredicates is necessary here because we do not migrate
                    // conjuncts into outer-joined inline views, so hasEmptyResultSet() cannot be
                    // true for an outer-joined inline view that has no table refs.
                    Preconditions.checkState(!analyzer.isOuterJoined(inlineViewRef.getId()));
                    emptySetNode.setOutputSmap(inlineViewRef.getSmap());
                    return emptySetNode;
                }
                // Analysis should have generated a tuple id into which to materialize the exprs.
                Preconditions.checkState(inlineViewRef.getMaterializedTupleIds().size() == 1);
                // we need to materialize all slots of our inline view tuple
                analyzer.getTupleDesc(inlineViewRef.getId()).materializeSlots();
                UnionNode unionNode = new UnionNode(ctx_.getNextNodeId(),
                        inlineViewRef.getMaterializedTupleIds().get(0));
                if (analyzer.hasEmptyResultSet()) {
                    return unionNode;
                }
                unionNode.addConstExprList(selectStmt.getBaseTblResultExprs());
                unionNode.init(analyzer);
                return unionNode;
            }
        }

        PlanNode rootNode = createQueryPlan(inlineViewRef.getViewStmt(), inlineViewRef.getAnalyzer());

        // The output smap is the composition of the inline view's smap and the output smap
        // of the inline view's plan root. This ensures that all downstream exprs referencing
        // the inline view are replaced with exprs referencing the physical output of the
        // inline view's plan.
        ExprSubstitutionMap outputSmap = ExprSubstitutionMap.compose(
                inlineViewRef.getSmap(), rootNode.getOutputSmap(), analyzer);
        if (analyzer.isOuterJoined(inlineViewRef.getId())) {
            rootNode.setWithoutTupleIsNullOutputSmap(outputSmap);
            // Exprs against non-matched rows of an outer join should always return NULL.
            // Make the rhs exprs of the output smap nullable, if necessary. This expr wrapping
            // must be performed on the composed smap, and not on the the inline view's smap,
            // because the rhs exprs must first be resolved against the physical output of
            // 'planRoot' to correctly determine whether wrapping is necessary.
            List<Expr> nullableRhs = TupleIsNullPredicate.wrapExprs(
                    outputSmap.getRhs(), rootNode.getTupleIds(), analyzer);
            outputSmap = new ExprSubstitutionMap(outputSmap.getLhs(), nullableRhs);
        }
        // Set output smap of rootNode *before* creating a SelectNode for proper resolution.
        rootNode.setOutputSmap(outputSmap);

        // rootNode.setOutputSmap(ExprSubstitutionMap.compose(inlineViewRef.getBaseTblSmap(),
        //         rootNode.getOutputSmap(), analyzer));
        // Expr.substituteList(inlineViewRef.getViewStmt().getResultExprs(), analyzer.getChangeResSmap());
        // analyzer.setChangeResSmap(inlineViewRef.getAnalyzer().getChangeResSmap());

        //        Expr.SubstitutionMap inlineViewSmap = inlineViewRef.getExprSMap();
        //        if (analyzer.isOuterJoined(inlineViewRef.getId())) {
        //            // Exprs against non-matched rows of an outer join should always return NULL.
        //            // Make the rhs exprs of the inline view's smap nullable, if necessary.
        //            List<Expr> nullableRhs = TupleIsNullPredicate.wrapExprs(
        //                    inlineViewSmap.getRhs(), node.getTupleIds(), analyzer);
        //            inlineViewSmap = new Expr.SubstitutionMap(inlineViewSmap.getLhs(), nullableRhs);
        //        }
        //
        //        // Set output smap of rootNode *before* creating a SelectNode for proper resolution.
        //        // The output smap is the composition of the inline view's smap and the output smap
        //        // of the inline view's plan root. This ensures that all downstream exprs referencing
        //        // the inline view are replaced with exprs referencing the physical output of
        //        // the inline view's plan.
        //        Expr.SubstitutionMap composedSmap = Expr.SubstitutionMap.compose(inlineViewSmap,
        //                node.getOutputSmap(), analyzer);
        //        node.setOutputSmap(composedSmap);

        // If the inline view has a LIMIT/OFFSET or unassigned conjuncts due to analytic
        // functions, we may have conjuncts that need to be assigned to a SELECT node on
        // top of the current plan root node.
        //
        // TODO: This check is also repeated in migrateConjunctsToInlineView() because we
        // need to make sure that equivalences are not enforced multiple times. Consolidate
        // the assignment of conjuncts and the enforcement of equivalences into a single
        // place.
        if (!canMigrateConjuncts(inlineViewRef)) {
            rootNode = addUnassignedConjuncts(
                    analyzer, inlineViewRef.getDesc().getId().asList(), rootNode);
        }
        return rootNode;
    }

    /**
     * Migrates unassigned conjuncts into an inline view. Conjuncts are not
     * migrated into the inline view if the view has a LIMIT/OFFSET clause or if the
     * view's stmt computes analytic functions (see IMPALA-1243/IMPALA-1900).
     * The reason is that analytic functions compute aggregates over their entire input,
     * and applying filters from the enclosing scope *before* the aggregate computation
     * would alter the results. This is unlike regular aggregate computation, which only
     * makes the *output* of the computation visible to the enclosing scope, so that
     * filters from the enclosing scope can be safely applied (to the grouping cols, say).
     */
    public void migrateConjunctsToInlineView(Analyzer analyzer,
                                             InlineViewRef inlineViewRef) throws AnalysisException {
        // All conjuncts
        final List<Expr> unassignedConjuncts =
                analyzer.getUnassignedConjuncts(inlineViewRef.getId().asList(), true);

        // Constant conjuncts
        final List<Expr> unassignedConstantConjuncts = Lists.newArrayList();
        for (Expr e : unassignedConjuncts) {
            if (e.isConstant()) {
                unassignedConstantConjuncts.add(e);
            }
        }
        // Non constant conjuncts
        unassignedConjuncts.removeAll(unassignedConstantConjuncts);
        migrateNonconstantConjuncts(inlineViewRef, unassignedConjuncts, analyzer);
        migrateConstantConjuncts(inlineViewRef, unassignedConstantConjuncts);
    }

    /**
     * For handling non-constant conjuncts. This only substitute conjunct's tuple with InlineView's
     * and register it in InlineView's Analyzer, whcih will be assigned by the next planning.
     *
     * @param inlineViewRef
     * @param unassignedConjuncts
     * @param analyzer
     * @throws AnalysisException
     */
    private void migrateNonconstantConjuncts(
            InlineViewRef inlineViewRef, List<Expr> unassignedConjuncts, Analyzer analyzer) throws AnalysisException {
        final List<Expr> preds = Lists.newArrayList();
        for (Expr e : unassignedConjuncts) {
            if (analyzer.canEvalPredicate(inlineViewRef.getId().asList(), e)) {
                preds.add(e);
            }
        }

        final List<Expr> pushDownFailedPredicates = Lists.newArrayList();
        final List<Expr> viewPredicates = getPushDownPredicatesForInlineView(
                inlineViewRef, preds, analyzer, pushDownFailedPredicates);
        if (viewPredicates.size() <= 0) {
            // mark (fully resolve) slots referenced by unassigned conjuncts as
            // materialized
            List<Expr> substUnassigned = Expr.substituteList(unassignedConjuncts,
                    inlineViewRef.getBaseTblSmap(), analyzer, false);
            analyzer.materializeSlots(substUnassigned);
            return;
        }
        preds.removeAll(pushDownFailedPredicates);
        unassignedConjuncts.remove(preds);
        unassignedConjuncts.addAll(pushDownFailedPredicates);

        // Remove unregistered predicates that reference the same slot on
        // both sides (e.g. a = a). Such predicates have been generated from slot
        // equivalences and may incorrectly reject rows with nulls (IMPALA-1412/IMPALA-2643).
        // TODO(zc)
        final Predicate<Expr> isIdentityPredicate = new Predicate<Expr>() {
            @Override
            public boolean apply(Expr expr) {
                return com.starrocks.analysis.Predicate.isEquivalencePredicate(expr)
                        && ((BinaryPredicate) expr).isInferred()
                        && expr.getChild(0).equals(expr.getChild(1));
            }
        };
        Iterables.removeIf(viewPredicates, isIdentityPredicate);

        // Migrate the conjuncts by marking the original ones as assigned, and
        // re-registering the substituted ones with new ids.
        analyzer.markConjunctsAssigned(preds);
        // Unset the On-clause flag of the migrated conjuncts because the migrated conjuncts
        // apply to the post-join/agg/analytic result of the inline view.
        for (Expr e : viewPredicates) {
            e.setIsOnClauseConjunct(false);
        }
        inlineViewRef.getAnalyzer().registerConjuncts(viewPredicates, inlineViewRef.getAllTupleIds());

        // mark (fully resolve) slots referenced by remaining unassigned conjuncts as
        // materialized
        List<Expr> substUnassigned = Expr.substituteList(unassignedConjuncts,
                inlineViewRef.getBaseTblSmap(), analyzer, false);
        analyzer.materializeSlots(substUnassigned);

    }

    /**
     * For handling constant conjuncts when migrating conjuncts to InlineViews. Constant conjuncts
     * should be assigned to query block from top to bottom, it will try to push down constant conjuncts.
     *
     * @param inlineViewRef
     * @param conjuncts
     * @throws AnalysisException
     */
    private void migrateConstantConjuncts(InlineViewRef inlineViewRef, List<Expr> conjuncts) throws AnalysisException {
        if (conjuncts.isEmpty()) {
            return;
        }

        if (!canMigrateConjuncts(inlineViewRef)) {
            return;
        }

        final List<Expr> newConjuncts = cloneExprs(conjuncts);
        final QueryStmt stmt = inlineViewRef.getViewStmt();
        final Analyzer viewAnalyzer = inlineViewRef.getAnalyzer();
        viewAnalyzer.markConjunctsAssigned(conjuncts);
        if (stmt instanceof SelectStmt) {
            final SelectStmt select = (SelectStmt) stmt;
            if (select.getAggInfo() != null) {
                viewAnalyzer.registerConjuncts(newConjuncts, select.getAggInfo().getOutputTupleId().asList());
            } else if (select.getTableRefs().size() > 1) {
                // Conjuncts will be assigned to the lowest outer join node or non-outer join's leaf children.
                for (int i = select.getTableRefs().size(); i > 1; i--) {
                    final TableRef joinInnerChild = select.getTableRefs().get(i - 1);
                    if (!joinInnerChild.getJoinOp().isOuterJoin()) {
                        // lowest join is't outer join.
                        if (i == 2) {
                            // Register constant for inner.
                            viewAnalyzer.registerConjuncts(newConjuncts, joinInnerChild.getDesc().getId().asList());
                            // Register constant for outer.
                            final TableRef joinOuterChild = select.getTableRefs().get(0);
                            final List<Expr> cloneConjuncts = cloneExprs(newConjuncts);
                            viewAnalyzer.registerConjuncts(cloneConjuncts, joinOuterChild.getDesc().getId().asList());
                        }
                        continue;
                    }
                    viewAnalyzer.registerOnClauseConjuncts(newConjuncts, joinInnerChild);
                    break;
                }
            } else {
                Preconditions.checkArgument(select.getTableRefs().size() == 1);
                viewAnalyzer.registerConjuncts(newConjuncts, select.getTableRefs().get(0).getDesc().getId().asList());
            }
        } else {
            Preconditions.checkArgument(stmt instanceof SetOperationStmt);
            final SetOperationStmt union = (SetOperationStmt) stmt;
            viewAnalyzer.registerConjuncts(newConjuncts, union.getTupleId().asList());
        }
    }

    private List<Expr> cloneExprs(List<Expr> candidates) {
        final List<Expr> clones = Lists.newArrayList();
        candidates.forEach(expr -> clones.add(expr.clone()));
        return clones;
    }

    /**
     * Get predicates can be migrated into an inline view.
     */
    private List<Expr> getPushDownPredicatesForInlineView(
            InlineViewRef inlineViewRef, List<Expr> viewPredicates,
            Analyzer analyzer, List<Expr> pushDownFailedPredicates) {
        // TODO chenhao, remove evaluateOrderBy when SubQuery's default limit is removed.
        final List<Expr> pushDownPredicates = Lists.newArrayList();
        if (inlineViewRef.getViewStmt().evaluateOrderBy()
                || inlineViewRef.getViewStmt().hasLimit()
                || inlineViewRef.getViewStmt().hasOffset()) {
            return pushDownPredicates;
        }

        // UnionNode will handle predicates and assigns predicates to it's children.
        final List<Expr> candicatePredicates =
                Expr.substituteList(viewPredicates, inlineViewRef.getSmap(), analyzer, false);
        if (inlineViewRef.getViewStmt() instanceof SetOperationStmt) {
            final SetOperationStmt setOperationStmt = (SetOperationStmt) inlineViewRef.getViewStmt();
            for (int i = 0; i < candicatePredicates.size(); i++) {
                final Expr predicate = candicatePredicates.get(i);
                if (predicate.isBound(setOperationStmt.getTupleId())) {
                    pushDownPredicates.add(predicate);
                } else {
                    pushDownFailedPredicates.add(viewPredicates.get(i));
                }
            }
            return pushDownPredicates;
        }

        final SelectStmt selectStmt = (SelectStmt) inlineViewRef.getViewStmt();
        if (selectStmt.hasAnalyticInfo()) {
            pushDownPredicates.addAll(getWindowsPushDownPredicates(candicatePredicates, viewPredicates,
                    selectStmt.getAnalyticInfo(), pushDownFailedPredicates));
        } else {
            pushDownPredicates.addAll(candicatePredicates);
        }
        return pushDownPredicates;
    }

    /**
     * Get predicates which can be pushed down past Windows.
     *
     * @param predicates
     * @param viewPredicates
     * @param analyticInfo
     * @param pushDownFailedPredicates
     * @return
     */
    private List<Expr> getWindowsPushDownPredicates(
            List<Expr> predicates, List<Expr> viewPredicates,
            AnalyticInfo analyticInfo, List<Expr> pushDownFailedPredicates) {
        final List<Expr> pushDownPredicates = Lists.newArrayList();
        final List<Expr> partitionExprs = analyticInfo.getCommonPartitionExprs();
        final List<SlotId> partitionByIds = Lists.newArrayList();
        for (Expr expr : partitionExprs) {
            if (expr instanceof SlotRef) {
                final SlotRef slotRef = (SlotRef) expr;
                partitionByIds.add(slotRef.getSlotId());
            }
        }

        if (partitionByIds.size() <= 0) {
            return pushDownPredicates;
        }

        for (int i = 0; i < predicates.size(); i++) {
            final Expr predicate = predicates.get(i);
            if (predicate.isBound(partitionByIds)) {
                pushDownPredicates.add(predicate);
            } else {
                pushDownFailedPredicates.add(viewPredicates.get(i));
            }
        }
        return pushDownPredicates;
    }

    /**
     * Checks if conjuncts can be migrated into an inline view.
     */
    private boolean canMigrateConjuncts(InlineViewRef inlineViewRef) {
        // TODO chenhao, remove evaluateOrderBy when SubQuery's default limit is removed.
        return inlineViewRef.getViewStmt().evaluateOrderBy() ? false :
                (!inlineViewRef.getViewStmt().hasLimit()
                        && !inlineViewRef.getViewStmt().hasOffset()
                        && (!(inlineViewRef.getViewStmt() instanceof SelectStmt)
                        || !((SelectStmt) inlineViewRef.getViewStmt()).hasAnalyticInfo()));
    }

    /**
     * Create node for scanning all data files of a particular table.
     */
    private PlanNode createScanNode(Analyzer analyzer, TableRef tblRef, SelectStmt selectStmt)
            throws UserException {
        ScanNode scanNode = null;

        switch (tblRef.getTable().getType()) {
            case OLAP:
                OlapScanNode olapNode = new OlapScanNode(ctx_.getNextNodeId(), tblRef.getDesc(), "OlapScanNode");
                olapNode.setForceOpenPreAgg(tblRef.isForcePreAggOpened());
                scanNode = olapNode;
                break;
            case MYSQL:
                scanNode = new MysqlScanNode(ctx_.getNextNodeId(), tblRef.getDesc(), (MysqlTable) tblRef.getTable());
                break;
            case SCHEMA:
                scanNode = new SchemaScanNode(ctx_.getNextNodeId(), tblRef.getDesc());
                break;
            case BROKER:
                scanNode = new FileScanNode(ctx_.getNextNodeId(), tblRef.getDesc(), "FileScanNode",
                        null, -1);
                break;
            case ELASTICSEARCH:
                scanNode = new EsScanNode(ctx_.getNextNodeId(), tblRef.getDesc(), "EsScanNode");
                break;
            case HIVE:
                scanNode = new HdfsScanNode(ctx_.getNextNodeId(), tblRef.getDesc(), "HdfsScanNode");
                break;
            default:
                break;
        }
        if (scanNode instanceof OlapScanNode || scanNode instanceof EsScanNode || scanNode instanceof HdfsScanNode) {
            Map<String, PartitionColumnFilter> columnFilters = Maps.newHashMap();
            List<Expr> conjuncts = analyzer.getUnassignedConjuncts(scanNode);

            // push down join predicate
            List<Expr> pushDownConjuncts = Lists.newArrayList();
            TupleId tupleId = tblRef.getId();
            List<Expr> eqJoinPredicates = analyzer.getEqJoinConjuncts(tupleId);
            if (eqJoinPredicates != null) {
                // only inner and left outer join
                if ((tblRef.getJoinOp().isInnerJoin() || tblRef.getJoinOp().isLeftOuterJoin())) {
                    List<Expr> allConjuncts = analyzer.getConjuncts(analyzer.getAllTupleIds());
                    allConjuncts.removeAll(conjuncts);
                    for (Expr conjunct : allConjuncts) {
                        if (com.starrocks.analysis.Predicate.canPushDownPredicate(conjunct)) {
                            for (Expr eqJoinPredicate : eqJoinPredicates) {
                                // we can ensure slot is left node, because NormalizeBinaryPredicatesRule
                                SlotRef otherSlot = conjunct.getChild(0).unwrapSlotRef();

                                // ensure the children for eqJoinPredicate both be SlotRef
                                if (eqJoinPredicate.getChild(0).unwrapSlotRef() == null ||
                                        eqJoinPredicate.getChild(1).unwrapSlotRef() == null) {
                                    continue;
                                }

                                SlotRef leftSlot = eqJoinPredicate.getChild(0).unwrapSlotRef();
                                SlotRef rightSlot = eqJoinPredicate.getChild(1).unwrapSlotRef();

                                // ensure the type is match
                                if (!leftSlot.getDesc().getType().matchesType(rightSlot.getDesc().getType())) {
                                    continue;
                                }

                                // example: t1.id = t2.id and t1.id = 1  => t2.id =1
                                if (otherSlot.isBound(leftSlot.getSlotId()) && rightSlot.isBound(tupleId)) {
                                    pushDownConjuncts.add(rewritePredicate(analyzer, conjunct, rightSlot));
                                } else if (otherSlot.isBound(rightSlot.getSlotId()) && leftSlot.isBound(tupleId)) {
                                    pushDownConjuncts.add(rewritePredicate(analyzer, conjunct, leftSlot));
                                }
                            }
                        }
                    }
                }

                LOG.debug("pushDownConjuncts: {}", pushDownConjuncts);
                conjuncts.addAll(pushDownConjuncts);
            }

            for (Column column : tblRef.getTable().getBaseSchema()) {
                SlotDescriptor slotDesc = tblRef.getDesc().getColumnSlot(column.getName());
                if (null == slotDesc) {
                    continue;
                }
                PartitionColumnFilter keyFilter = createPartitionFilter(slotDesc, conjuncts);
                if (null != keyFilter) {
                    columnFilters.put(column.getName(), keyFilter);
                }
            }
            scanNode.setColumnFilters(columnFilters);
            scanNode.setSortColumn(tblRef.getSortColumn());
            scanNode.addConjuncts(pushDownConjuncts);
        }
        // assignConjuncts(scanNode, analyzer);
        scanNode.init(analyzer);
        // TODO chenhao add
        // materialize conjuncts in where
        analyzer.materializeSlots(scanNode.getConjuncts());

        scanNodes.add(scanNode);
        List<ScanNode> scanNodeList =
                selectStmtToScanNodes.computeIfAbsent(selectStmt.getAnalyzer(), k -> Lists.newArrayList());
        scanNodeList.add(scanNode);
        return scanNode;
    }

    // Rewrite the oldPredicate with new leftChild
    // For example: oldPredicate is t1.id = 1, leftChild is t2.id, will return t2.id = 1
    private Expr rewritePredicate(Analyzer analyzer, Expr oldPredicate, Expr leftChild) {
        if (oldPredicate instanceof BinaryPredicate) {
            BinaryPredicate oldBP = (BinaryPredicate) oldPredicate;
            BinaryPredicate bp = new BinaryPredicate(oldBP.getOp(), leftChild, oldBP.getChild(1));
            bp.analyzeNoThrow(analyzer);
            return bp;
        }

        if (oldPredicate instanceof InPredicate) {
            InPredicate oldIP = (InPredicate) oldPredicate;
            InPredicate ip = new InPredicate(leftChild, oldIP.getListChildren(), oldIP.isNotIn());
            ip.analyzeNoThrow(analyzer);
            return ip;
        }

        return oldPredicate;
    }

    /**
     * Return join conjuncts that can be used for hash table lookups. - for inner joins, those are equi-join predicates
     * in which one side is fully bound by lhsIds and the other by rhs' id; - for outer joins: same type of conjuncts as
     * inner joins, but only from the JOIN clause Returns the original form in 'joinPredicates'.
     */
    private void getHashLookupJoinConjuncts(Analyzer analyzer, PlanNode left, PlanNode right,
                                            List<Expr> joinConjuncts,
                                            Reference<String> errMsg, JoinOperator op) {
        joinConjuncts.clear();
        final List<TupleId> lhsIds = left.getTupleIds();
        final List<TupleId> rhsIds = right.getTupleIds();
        List<Expr> candidates;
        candidates = analyzer.getEqJoinConjuncts(lhsIds, rhsIds);
        if (candidates == null) {
            if (op.isOuterJoin() || op.isSemiAntiJoin()) {
                errMsg.setRef("non-equal " + op.toString() + " is not supported");
                LOG.warn(errMsg);
            }
            LOG.debug("no candidates for join.");
            return;
        }

        for (Expr e : candidates) {
            // Ignore predicate if one of its children is a constant.
            if (e.getChild(0).isConstant() || e.getChild(1).isConstant()) {
                LOG.debug("double is constant.");
                continue;
            }

            /**
             * The left and right child of origin predicates need to be swap sometimes.
             * Case A:
             * select * from t1 join t2 on t2.id=t1.id
             * The left plan node is t1 and the right plan node is t2.
             * The left child of origin predicate is t2.id and the right child of origin predicate is t1.id.
             * In this situation, the children of predicate need to be swap => t1.id=t2.id.
             */
            Expr rhsExpr = null;
            if (e.getChild(0).isBoundByTupleIds(rhsIds)) {
                rhsExpr = e.getChild(0);
            } else {
                Preconditions.checkState(e.getChild(1).isBoundByTupleIds(rhsIds));
                rhsExpr = e.getChild(1);
            }

            Expr lhsExpr = null;
            if (e.getChild(1).isBoundByTupleIds(lhsIds)) {
                lhsExpr = e.getChild(1);
            } else if (e.getChild(0).isBoundByTupleIds(lhsIds)) {
                lhsExpr = e.getChild(0);
            } else {
                LOG.debug("not an equi-join condition between lhsIds and rhsId");
                continue;
            }

            Preconditions.checkState(lhsExpr != rhsExpr);
            Preconditions.checkState(e instanceof BinaryPredicate);
            // The new predicate id must same as the origin predicate.
            // This expr id is used to mark as assigned in the future.
            BinaryPredicate newEqJoinPredicate = (BinaryPredicate) e.clone();
            newEqJoinPredicate.setChild(0, lhsExpr);
            newEqJoinPredicate.setChild(1, rhsExpr);
            joinConjuncts.add(newEqJoinPredicate);
        }
    }

    /**
     * Creates a new node to join outer with inner. Collects and assigns join conjunct
     * as well as regular conjuncts. Calls init() on the new join node.
     * Throws if the JoinNode.init() fails.
     */
    private PlanNode createJoinNode(Analyzer analyzer, PlanNode outer, TableRef outerRef, TableRef innerRef,
                                    SelectStmt selectStmt)
            throws UserException, AnalysisException {
        materializeTableResultForCrossJoinOrCountStar(innerRef, analyzer);
        // the rows coming from the build node only need to have space for the tuple
        // materialized by that node
        PlanNode inner = createTableRefNode(analyzer, innerRef, selectStmt);

        List<Expr> eqJoinConjuncts = Lists.newArrayList();
        Reference<String> errMsg = new Reference<String>();
        // get eq join predicates for the TableRefs' ids (not the PlanNodes' ids, which
        // are materialized)
        getHashLookupJoinConjuncts(analyzer, outer, inner,
                eqJoinConjuncts, errMsg, innerRef.getJoinOp());
        if (eqJoinConjuncts.isEmpty()) {

            // only inner join can change to cross join
            if (innerRef.getJoinOp().isOuterJoin() || innerRef.getJoinOp().isSemiAntiJoin()) {
                throw new AnalysisException("non-equal " + innerRef.getJoinOp().toString()
                        + " is not supported");
            }

            // construct cross join node
            LOG.debug(
                    "Join between {} and {} requires at least one conjunctive equality predicate between the two tables",
                    outerRef.getAliasAsName(), innerRef.getAliasAsName());
            // TODO If there are eq join predicates then we should construct a hash join
            CrossJoinNode result =
                    new CrossJoinNode(ctx_.getNextNodeId(), outer, inner, innerRef);
            result.init(analyzer);
            return result;
        }
        analyzer.markConjunctsAssigned(eqJoinConjuncts);

        List<Expr> ojConjuncts = Lists.newArrayList();
        if (innerRef.getJoinOp().isOuterJoin()) {
            // Also assign conjuncts from On clause. All remaining unassigned conjuncts
            // that can be evaluated by this join are assigned in createSelectPlan().
            ojConjuncts = analyzer.getUnassignedOjConjuncts(innerRef);
            analyzer.markConjunctsAssigned(ojConjuncts);
        } else if (innerRef.getJoinOp().isSemiAntiJoin()) {
            final List<TupleId> tupleIds = innerRef.getAllTupleIds();
            ojConjuncts = analyzer.getUnassignedConjuncts(tupleIds, false);

            for (TupleId tupleId : tupleIds) {
                // Single table's WhereClauseConjunct can't assign direct over outer-join
                if (analyzer.isOuterJoined(tupleId)) {
                    ojConjuncts = ojConjuncts.stream().filter(d -> (!d.isBound(tupleId) || d.isOnClauseConjunct()))
                            .collect(Collectors.toList());
                }
            }

            List<Expr> otherJoinConjuncts = Lists.newArrayList(ojConjuncts);
            for (Expr e : otherJoinConjuncts) {
                TableRef tableRef = analyzer.getAntiJoinRef(e);
                // Ensure the join tupleIds contains table id related with the conjunct
                // The concrete example could refer to QueryPlanTest::testMultiNotExistPredicatePushDown
                if (tableRef != null && !tupleIds.contains(tableRef.getId())) {
                    ojConjuncts.remove(e);
                }
            }
            analyzer.markConjunctsAssigned(ojConjuncts);
        }

        HashJoinNode result =
                new HashJoinNode(ctx_.getNextNodeId(), outer, inner, innerRef, eqJoinConjuncts,
                        ojConjuncts);
        result.init(analyzer);
        return result;
    }

    /**
     * Create a tree of PlanNodes for the given tblRef, which can be a BaseTableRef,
     * CollectionTableRef or an InlineViewRef.
     * <p>
     * 'fastPartitionKeyScans' indicates whether to try to produce the slots with
     * metadata instead of table scans. Only applicable to BaseTableRef which is also
     * an HDFS table.
     * <p>
     * Throws if a PlanNode.init() failed or if planning of the given
     * table ref is not implemented.
     */
    private PlanNode createTableRefNode(Analyzer analyzer, TableRef tblRef, SelectStmt selectStmt)
            throws UserException, AnalysisException {
        if (tblRef instanceof BaseTableRef) {
            return createScanNode(analyzer, tblRef, selectStmt);
        }
        if (tblRef instanceof InlineViewRef) {
            return createInlineViewPlan(analyzer, (InlineViewRef) tblRef);
        }
        throw new UserException("unknown TableRef node");
    }

    /**
     * Create a plan tree corresponding to 'setOperands' for the given SetOperationStmt.
     * The individual operands' plan trees are attached to a single SetOperationNode.
     * If result is not null, it is expected to contain the plan for the
     * distinct portion of the given SetOperationStmt. The result is then added
     * as a child of the returned SetOperationNode.
     */
    private SetOperationNode createSetOperationPlan(
            Analyzer analyzer, SetOperationStmt setOperationStmt, List<SetOperationStmt.SetOperand> setOperands,
            PlanNode result)
            throws UserException, AnalysisException {
        SetOperationNode setOpNode;
        SetOperationStmt.Operation operation = null;
        for (SetOperationStmt.SetOperand setOperand : setOperands) {
            if (setOperand.getOperation() != null) {
                if (operation == null) {
                    operation = setOperand.getOperation();
                }
                Preconditions.checkState(operation == setOperand.getOperation(), "can not support mixed set "
                        + "operations at here");
            }
        }
        switch (operation) {
            case UNION:
                setOpNode = new UnionNode(ctx_.getNextNodeId(), setOperationStmt.getTupleId(),
                        setOperationStmt.getSetOpsResultExprs(), false);
                break;
            case INTERSECT:
                setOpNode = new IntersectNode(ctx_.getNextNodeId(), setOperationStmt.getTupleId(),
                        setOperationStmt.getSetOpsResultExprs(), false);
                break;
            case EXCEPT:
                setOpNode = new ExceptNode(ctx_.getNextNodeId(), setOperationStmt.getTupleId(),
                        setOperationStmt.getSetOpsResultExprs(), false);
                break;
            default:
                throw new AnalysisException("not supported set operations: " + operation);
        }
        // If it is a union or other same operation, there are only two possibilities,
        // one is the root node, and the other is a distinct node in front, so the setOperationDistinctPlan will
        // be aggregate node, if this is a mixed operation
        // e.g. :
        // a union b -> result == null
        // a union b union all c -> result == null -> result == AggregationNode
        // a union all b except c -> result == null -> result == UnionNode
        // a union b except c -> result == null -> result == AggregationNode
        if (result != null && result instanceof SetOperationNode) {
            Preconditions.checkState(!result.getClass().equals(setOpNode.getClass()));
            setOpNode.addChild(result, setOperationStmt.getResultExprs());
        } else if (result != null) {
            Preconditions.checkState(setOperationStmt.hasDistinctOps());
            Preconditions.checkState(result instanceof AggregationNode);
            setOpNode.addChild(result,
                    setOperationStmt.getDistinctAggInfo().getGroupingExprs());
        }
        for (SetOperationStmt.SetOperand op : setOperands) {
            if (op.getAnalyzer().hasEmptyResultSet()) {
                unmarkCollectionSlots(op.getQueryStmt());
                continue;
            }
            QueryStmt queryStmt = op.getQueryStmt();
            if (queryStmt instanceof SelectStmt) {
                SelectStmt selectStmt = (SelectStmt) queryStmt;
                if (selectStmt.getTableRefs().isEmpty() && setOpNode instanceof UnionNode) {
                    setOpNode.addConstExprList(selectStmt.getResultExprs());
                    continue;
                }
            }
            PlanNode opPlan = createQueryPlan(queryStmt, op.getAnalyzer());
            // There may still be unassigned conjuncts if the operand has an order by + limit.
            // Place them into a SelectNode on top of the operand's plan.
            opPlan = addUnassignedConjuncts(analyzer, opPlan.getTupleIds(), opPlan);
            if (opPlan instanceof EmptySetNode) {
                continue;
            }
            setOpNode.addChild(opPlan, op.getQueryStmt().getResultExprs());
        }
        setOpNode.init(analyzer);
        return setOpNode;
    }

    /**
     * Returns plan tree for unionStmt:
     * - distinctOperands' plan trees are collected in a single UnionNode
     * and duplicates removed via distinct aggregation
     * - the output of that plus the allOperands' plan trees are collected in
     * another UnionNode which materializes the result of unionStmt
     * - if any of the union operands contains analytic exprs, we avoid pushing
     * predicates directly into the operands and instead evaluate them
     * *after* the final UnionNode (see createInlineViewPlan() for the reasoning)
     * TODO: optimize this by still pushing predicates into the union operands
     * that don't contain analytic exprs and evaluating the conjuncts in Select
     * directly above the AnalyticEvalNodes
     * TODO: Simplify the plan of unions with empty operands using an empty set node.
     * TODO: Simplify the plan of unions with only a single non-empty operand to not
     * use a union node (this is tricky because a union materializes a new tuple).
     */
    private PlanNode createSetOperationPlan(SetOperationStmt setOperationStmt, Analyzer analyzer)
            throws UserException, AnalysisException {
        // TODO(zc): get unassigned conjuncts
        // List<Expr> conjuncts =
        //         analyzer.getUnassignedConjuncts(unionStmt.getTupleId().asList(), false);
        List<Expr> conjuncts = analyzer.getUnassignedConjuncts(setOperationStmt.getTupleId().asList());
        // TODO chenhao
        // Because Conjuncts can't be assigned to UnionNode and starrocks's fe can't evaluate conjuncts,
        // it needs to add SelectNode as UnionNode's parent, when UnionStmt's Ops contains constant 
        // Select.
        boolean hasConstantOp = false;
        if (!setOperationStmt.hasAnalyticExprs()) {
            // Turn unassigned predicates for unionStmt's tupleId_ into predicates for
            // the individual operands.
            // Do this prior to creating the operands' plan trees so they get a chance to
            // pick up propagated predicates.
            for (SetOperationStmt.SetOperand op : setOperationStmt.getOperands()) {
                List<Expr> opConjuncts =
                        Expr.substituteList(conjuncts, op.getSmap(), analyzer, false);
                boolean selectHasTableRef = true;
                final QueryStmt queryStmt = op.getQueryStmt();
                // Check whether UnionOperand is constant Select.
                if (queryStmt instanceof SelectStmt) {
                    final SelectStmt selectStmt = (SelectStmt) queryStmt;
                    if (selectStmt.getTableRefs().isEmpty()) {
                        selectHasTableRef = false;
                        hasConstantOp = !selectHasTableRef;
                    }
                }
                // Forbid to register Conjuncts with SelectStmt' tuple when Select is constant
                if ((queryStmt instanceof SelectStmt) && selectHasTableRef) {
                    final SelectStmt select = (SelectStmt) queryStmt;
                    op.getAnalyzer().registerConjuncts(opConjuncts, select.getTableRefIds());
                } else if (queryStmt instanceof SetOperationStmt) {
                    final SetOperationStmt subSetOp = (SetOperationStmt) queryStmt;
                    op.getAnalyzer().registerConjuncts(opConjuncts, subSetOp.getTupleId().asList());
                } else {
                    if (selectHasTableRef) {
                        Preconditions.checkArgument(false);
                    }
                }
            }
            if (!hasConstantOp) {
                analyzer.markConjunctsAssigned(conjuncts);
            }
        } else {
            // mark slots referenced by the yet-unassigned conjuncts
            analyzer.materializeSlots(conjuncts);
        }
        // mark slots after predicate propagation but prior to plan tree generation
        setOperationStmt.materializeRequiredSlots(analyzer);

        PlanNode result = null;
        SetOperationStmt.Operation operation = null;
        List<SetOperationStmt.SetOperand> partialOperands = new ArrayList<>();
        // create plan for a union b intersect c except b to three fragments
        // 3:[2:[1:[a union b] intersect c] except c]
        for (SetOperationStmt.SetOperand op : setOperationStmt.getOperands()) {
            if (op.getOperation() == null) {
                partialOperands.add(op);
            } else if (operation == null && op.getOperation() != null) {
                operation = op.getOperation();
                partialOperands.add(op);
            } else if (operation != null && op.getOperation() == operation) {
                partialOperands.add(op);
            } else if (operation != null && op.getOperation() != operation) {
                if (partialOperands.size() > 0) {
                    if (operation == SetOperationStmt.Operation.INTERSECT
                            || operation == SetOperationStmt.Operation.EXCEPT) {
                        result = createSetOperationPlan(analyzer, setOperationStmt, partialOperands, result);
                    } else {
                        result =
                                createUnionPartialSetOperationPlan(analyzer, setOperationStmt, partialOperands, result);
                    }
                    partialOperands.clear();
                }
                operation = op.getOperation();
                partialOperands.add(op);
            } else {
                throw new AnalysisException("invalid set operation statement.");
            }
        }
        if (partialOperands.size() > 0) {
            if (operation == SetOperationStmt.Operation.INTERSECT
                    || operation == SetOperationStmt.Operation.EXCEPT) {
                result = createSetOperationPlan(analyzer, setOperationStmt, partialOperands, result);
            } else {
                result = createUnionPartialSetOperationPlan(analyzer, setOperationStmt, partialOperands, result);
            }
        }

        if (setOperationStmt.hasAnalyticExprs() || hasConstantOp) {
            result = addUnassignedConjuncts(
                    analyzer, setOperationStmt.getTupleId().asList(), result);
        }
        return result;
    }

    // create the partial plan, or example: a union b intersect c
    // the first partial plan is a union b as a result d,
    // the second partial plan d intersect c
    // notice that when query is a union b the union operation is in right-hand child(b),
    // while the left-hand child(a)'s operation is null
    private PlanNode createUnionPartialSetOperationPlan(Analyzer analyzer, SetOperationStmt setOperationStmt,
                                                        List<SetOperationStmt.SetOperand> setOperands,
                                                        PlanNode result)
            throws UserException {
        boolean hasDistinctOps = false;
        boolean hasAllOps = false;
        List<SetOperationStmt.SetOperand> allOps = new ArrayList<>();
        List<SetOperationStmt.SetOperand> distinctOps = new ArrayList<>();
        for (SetOperationStmt.SetOperand op : setOperands) {
            if (op.getQualifier() == SetOperationStmt.Qualifier.DISTINCT) {
                hasDistinctOps = true;
                distinctOps.add(op);
            }
            if (op.getQualifier() == SetOperationStmt.Qualifier.ALL) {
                hasAllOps = true;
                allOps.add(op);
            }
        }
        // create DISTINCT tree
        if (hasDistinctOps) {
            result = createSetOperationPlan(
                    analyzer, setOperationStmt, distinctOps, result);
            result = new AggregationNode(ctx_.getNextNodeId(), result,
                    setOperationStmt.getDistinctAggInfo());
            result.init(analyzer);
        }
        // create ALL tree
        if (hasAllOps) {
            result = createSetOperationPlan(analyzer, setOperationStmt, allOps, result);
        }
        return result;
    }

    private PlanNode createAssertRowCountNode(PlanNode input, AssertNumRowsElement assertNumRowsElement,
                                              Analyzer analyzer) throws UserException {
        AssertNumRowsNode root = new AssertNumRowsNode(ctx_.getNextNodeId(), input, assertNumRowsElement);
        root.init(analyzer);
        return root;
    }

    /**
     * According to the way to materialize slots from top to bottom, Materialization will prune columns
     * which are not referenced by Statement outside. However, in some cases, in order to ensure The
     * correct execution, it is necessary to materialize the slots that are not needed by Statement
     * outside.
     *
     * @param tblRef
     * @param analyzer
     */
    private void materializeTableResultForCrossJoinOrCountStar(TableRef tblRef, Analyzer analyzer) {
        if (tblRef instanceof BaseTableRef) {
            materializeBaseTableRefResultForCrossJoinOrCountStar((BaseTableRef) tblRef, analyzer);
        } else if (tblRef instanceof InlineViewRef) {
            materializeInlineViewResultExprForCrossJoinOrCountStar((InlineViewRef) tblRef, analyzer);
        } else {
            Preconditions.checkArgument(false);
        }
    }

    /**
     * materialize BaseTableRef result' exprs for Cross Join or Count Star
     *
     * @param tblRef
     * @param analyzer
     */
    private void materializeBaseTableRefResultForCrossJoinOrCountStar(BaseTableRef tblRef, Analyzer analyzer) {
        if (tblRef.getDesc().getSlots().isEmpty()) {
            Column minimuColumn = null;
            for (Column col : tblRef.getTable().getBaseSchema()) {
                if (minimuColumn == null || col.getPrimitiveType().getSlotSize() < minimuColumn
                        .getPrimitiveType().getSlotSize()) {
                    minimuColumn = col;
                }
            }
            if (minimuColumn != null) {
                SlotDescriptor slot = analyzer.getDescTbl().addSlotDescriptor(tblRef.getDesc());
                slot.setColumn(minimuColumn);
                slot.setIsMaterialized(true);
            }
        }
    }

    /**
     * materialize InlineViewRef result'exprs for Cross Join or Count Star
     *
     * @param inlineView
     * @param analyzer
     */
    private void materializeInlineViewResultExprForCrossJoinOrCountStar(InlineViewRef inlineView, Analyzer analyzer) {
        final List<Expr> baseResultExprs = inlineView.getViewStmt().getBaseTblResultExprs();
        if (baseResultExprs.size() <= 0) {
            return;
        }
        Expr resultExprSelected = null;
        int resultExprSelectedSize = 0;
        // check whether inlineView contains materialized result expr
        for (Expr e : baseResultExprs) {
            final List<SlotId> slotIds = Lists.newArrayList();
            e.getIds(null, slotIds);
            boolean exprIsMaterialized = true;
            int exprSize = 0;
            for (SlotId id : slotIds) {
                final SlotDescriptor slot = analyzer.getDescTbl().getSlotDesc(id);
                if (!slot.isMaterialized()) {
                    exprIsMaterialized = false;
                }
                exprSize += slot.getByteSize();
            }

            // Result Expr contains materialized expr, return
            if (exprIsMaterialized) {
                return;
            }

            if (exprSize <= resultExprSelectedSize) {
                resultExprSelectedSize = exprSize;
                resultExprSelected = e;
            }
        }

        // materialize slots which expr refer and It's total size is smallest
        final List<SlotId> slotIds = Lists.newArrayList();
        final List<TupleId> tupleIds = Lists.newArrayList();
        resultExprSelected.getIds(tupleIds, slotIds);
        for (SlotId id : slotIds) {
            final SlotDescriptor slot = analyzer.getDescTbl().getSlotDesc(id);
            slot.setIsMaterialized(true);
        }
        for (TupleId id : tupleIds) {
            final TupleDescriptor tuple = analyzer.getDescTbl().getTupleDesc(id);
            tuple.setIsMaterialized(true);
        }
    }

    /**
     ------------------------------------------------------------------------------
     */
    /**
     * Push down predicates rules
     */

    /**
     * Entrance for push-down rules, it will execute possible push-down rules from top to down
     * and the planner will be responsible for assigning all predicates to PlanNode.
     */
    private void pushDownPredicates(Analyzer analyzer, SelectStmt stmt) throws AnalysisException {
        // Push down predicates according to the semantic requirements of SQL.
        pushDownPredicatesPastSort(analyzer, stmt);
        pushDownPredicatesPastWindows(analyzer, stmt);
        pushDownPredicatesPastAggregation(analyzer, stmt);
    }

    private void pushDownPredicatesPastSort(Analyzer analyzer, SelectStmt stmt) throws AnalysisException {
        // TODO chenhao, remove isEvaluateOrderBy when SubQuery's default limit is removed. 
        if (stmt.evaluateOrderBy() || stmt.getLimit() >= 0 || stmt.getOffset() > 0 || stmt.getSortInfo() == null) {
            return;
        }
        final List<Expr> predicates = getBoundPredicates(analyzer, stmt.getSortInfo().getSortTupleDescriptor());
        if (predicates.size() <= 0) {
            return;
        }
        final List<Expr> pushDownPredicates = getPredicatesReplacedSlotWithSourceExpr(predicates, analyzer);
        if (pushDownPredicates.size() <= 0) {
            return;
        }

        // Push down predicates to sort's child until they are assigned successfully.
        if (putPredicatesOnWindows(stmt, analyzer, pushDownPredicates)) {
            return;
        }
        if (putPredicatesOnAggregation(stmt, analyzer, pushDownPredicates)) {
            return;
        }
        putPredicatesOnFrom(stmt, analyzer, pushDownPredicates);
    }

    private void pushDownPredicatesPastWindows(Analyzer analyzer, SelectStmt stmt) throws AnalysisException {
        final AnalyticInfo analyticInfo = stmt.getAnalyticInfo();
        if (analyticInfo == null || analyticInfo.getCommonPartitionExprs().size() == 0) {
            return;
        }
        final List<Expr> predicates = getBoundPredicates(analyzer, analyticInfo.getOutputTupleDesc());
        if (predicates.size() <= 0) {
            return;
        }

        // Push down predicates to Windows' child until they are assigned successfully.
        final List<Expr> pushDownPredicates = getPredicatesBoundedByGroupbysSourceExpr(predicates, analyzer, stmt);
        if (pushDownPredicates.size() <= 0) {
            return;
        }
        if (putPredicatesOnAggregation(stmt, analyzer, pushDownPredicates)) {
            return;
        }
        putPredicatesOnFrom(stmt, analyzer, pushDownPredicates);
    }

    private void pushDownPredicatesPastAggregation(Analyzer analyzer, SelectStmt stmt) throws AnalysisException {
        final AggregateInfo aggregateInfo = stmt.getAggInfo();
        if (aggregateInfo == null || aggregateInfo.getGroupingExprs().size() <= 0) {
            return;
        }
        final List<Expr> predicates = getBoundPredicates(analyzer, aggregateInfo.getOutputTupleDesc());
        if (predicates.size() <= 0) {
            return;
        }

        // Push down predicates to aggregation's child until they are assigned successfully.
        final List<Expr> pushDownPredicates = getPredicatesBoundedByGroupbysSourceExpr(predicates, analyzer, stmt);
        if (pushDownPredicates.size() <= 0) {
            return;
        }
        putPredicatesOnFrom(stmt, analyzer, pushDownPredicates);
    }

    private List<Expr> getPredicatesBoundedByGroupbysSourceExpr(List<Expr> predicates, Analyzer analyzer,
                                                                SelectStmt stmt) {
        final List<Expr> predicatesCanPushDown = Lists.newArrayList();
        for (Expr predicate : predicates) {
            if (predicate.isConstant() || predicate.getType().isNull()) {
                // Constant and null predicates can't be pushed down past Groupby.
                continue;
            }

            final List<TupleId> tupleIds = Lists.newArrayList();
            final List<SlotId> slotIds = Lists.newArrayList();
            predicate.getIds(tupleIds, slotIds);

            boolean isAllSlotReferingGroupBys = true;
            for (SlotId slotId : slotIds) {
                final SlotDescriptor slotDesc = analyzer.getDescTbl().getSlotDesc(slotId);
                Expr sourceExpr = slotDesc.getSourceExprs().get(0);
                // if grouping set is given and column is not in all grouping set list
                // we cannot push the predicate since the column value can be null
                if (stmt.getGroupByClause() == null) {
                    //group by clause may be null when distinct grouping. 
                    //eg: select distinct c from ( select distinct c from table) t where c > 1;
                    continue;
                }
                if (stmt.getGroupByClause().isGroupByExtension()
                        && stmt.getGroupByClause().getGroupingExprs().contains(sourceExpr)) {
                    // if grouping type is CUBE or ROLLUP will definitely produce null
                    if (stmt.getGroupByClause().getGroupingType() == GroupByClause.GroupingType.CUBE
                            || stmt.getGroupByClause().getGroupingType() == GroupByClause.GroupingType.ROLLUP) {
                        isAllSlotReferingGroupBys = false;
                    } else {
                        // if grouping type is GROUPING_SETS and the predicate not in all grouping list,
                        // the predicate cannot be push down
                        for (List<Expr> exprs : stmt.getGroupByClause().getGroupingSetList()) {
                            if (!exprs.contains(sourceExpr)) {
                                isAllSlotReferingGroupBys = false;
                                break;
                            }
                        }
                    }
                }
                if (sourceExpr.getFn() instanceof AggregateFunction) {
                    isAllSlotReferingGroupBys = false;
                }
            }

            if (isAllSlotReferingGroupBys) {
                predicatesCanPushDown.add(predicate);
            }
        }
        return getPredicatesReplacedSlotWithSourceExpr(predicatesCanPushDown, analyzer);
    }

    private List<Expr> getPredicatesReplacedSlotWithSourceExpr(List<Expr> predicates, Analyzer analyzer) {
        final List<Expr> predicatesCanPushDown = Lists.newArrayList();
        analyzer.markConjunctsAssigned(predicates);
        for (Expr predicate : predicates) {
            final Expr newPredicate = predicate.clone();
            replacePredicateSlotRefWithSource(newPredicate, analyzer);
            predicatesCanPushDown.add(newPredicate);
        }
        return predicatesCanPushDown;
    }

    private void replacePredicateSlotRefWithSource(Expr predicate, Analyzer analyzer) {
        replacePredicateSlotRefWithSource(null, predicate, -1, analyzer);
    }

    private void replacePredicateSlotRefWithSource(Expr parent, Expr predicate, int childIndex, Analyzer analyzer) {
        if (predicate instanceof SlotRef) {
            final SlotRef slotRef = (SlotRef) predicate;
            if (parent != null && childIndex >= 0) {
                final Expr newReplacedExpr = slotRef.getDesc().getSourceExprs().get(0).clone();
                parent.setChild(childIndex, newReplacedExpr);
            }
        }

        for (int i = 0; i < predicate.getChildren().size(); i++) {
            final Expr child = predicate.getChild(i);
            replacePredicateSlotRefWithSource(predicate, child, i, analyzer);
        }
    }

    // Register predicates with Aggregation's output tuple id.
    private boolean putPredicatesOnAggregation(SelectStmt stmt, Analyzer analyzer,
                                               List<Expr> predicates) throws AnalysisException {
        final AggregateInfo aggregateInfo = stmt.getAggInfo();
        if (aggregateInfo != null) {
            analyzer.registerConjuncts(predicates, aggregateInfo.getOutputTupleId());
            return true;
        }
        return false;
    }

    // Register predicates with Windows's tuple id.
    private boolean putPredicatesOnWindows(SelectStmt stmt, Analyzer analyzer,
                                           List<Expr> predicates) throws AnalysisException {
        final AnalyticInfo analyticInfo = stmt.getAnalyticInfo();
        if (analyticInfo != null) {
            analyzer.registerConjuncts(predicates, analyticInfo.getOutputTupleId());
            return true;
        }
        return false;
    }

    // Register predicates with TableRef's tuple id.
    private void putPredicatesOnFrom(SelectStmt stmt, Analyzer analyzer, List<Expr> predicates)
            throws AnalysisException {
        final List<TupleId> tableTupleIds = Lists.newArrayList();
        for (TableRef tableRef : stmt.getTableRefs()) {
            tableTupleIds.add(tableRef.getId());
        }

        for (Expr predicate : predicates) {
            Preconditions.checkArgument(predicate.isBoundByTupleIds(tableTupleIds),
                    "Predicate:" + predicate.toSql() + " can't be assigned to some PlanNode.");
            final List<TupleId> predicateTupleIds = Lists.newArrayList();
            predicate.getIds(predicateTupleIds, null);
            analyzer.registerConjunct(predicate, predicateTupleIds);
        }
    }

    /**
     * ------------------------------------------------------------------------------
     */

    private List<Expr> getBoundPredicates(Analyzer analyzer, TupleDescriptor tupleDesc) {
        final List<TupleId> tupleIds = Lists.newArrayList();
        if (tupleDesc != null) {
            tupleIds.add(tupleDesc.getId());
        }
        return analyzer.getUnassignedConjuncts(tupleIds);
    }
}

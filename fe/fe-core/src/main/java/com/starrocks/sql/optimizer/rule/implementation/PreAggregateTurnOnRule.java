// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

package com.starrocks.sql.optimizer.rule.implementation;

import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.Lists;
import com.starrocks.catalog.AggregateType;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.FunctionSet;
import com.starrocks.catalog.OlapTable;
import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptExpressionVisitor;
import com.starrocks.sql.optimizer.Utils;
import com.starrocks.sql.optimizer.base.ColumnRefSet;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.Projection;
import com.starrocks.sql.optimizer.operator.physical.PhysicalHashAggregateOperator;
import com.starrocks.sql.optimizer.operator.physical.PhysicalHashJoinOperator;
import com.starrocks.sql.optimizer.operator.physical.PhysicalOlapScanOperator;
import com.starrocks.sql.optimizer.operator.scalar.BinaryPredicateOperator;
import com.starrocks.sql.optimizer.operator.scalar.CallOperator;
import com.starrocks.sql.optimizer.operator.scalar.CaseWhenOperator;
import com.starrocks.sql.optimizer.operator.scalar.CastOperator;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;
import com.starrocks.sql.optimizer.operator.scalar.ConstantOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.sql.optimizer.rewrite.ReplaceColumnRefRewriter;
import com.starrocks.sql.optimizer.rule.transformation.JoinPredicateUtils;

import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.stream.Collectors;

/**
 * This rule will turn on PreAggregation if conditions are met,
 * and turning on PreAggregation will help optimize the storage layer merge on read.
 * This rule traverses the query plan from the top down, and the Olap Scan node determines
 * whether preAggregation can be turned on or not based on the information recorded by the PreAggregationContext.
 * <p>
 * A cross join cannot turn on PreAggregation, and other types of joins can only be turn on on one side.
 * If both sides are opened, many-to-many join results will appear, leading to errors in the upper aggregation results
 */
public class PreAggregateTurnOnRule {
    private static final PreAggregateVisitor VISITOR = new PreAggregateVisitor();

    public static void tryOpenPreAggregate(OptExpression root) {
        root.getOp().accept(VISITOR, root, new PreAggregationContext());
    }

    private static class PreAggregateVisitor extends OptExpressionVisitor<Void, PreAggregationContext> {
        private static final List<String> AGGREGATE_ONLY_KEY = ImmutableList.<String>builder()
                .add("NDV")
                .add("MULTI_DISTINCT_COUNT")
                .add("APPROX_COUNT_DISTINCT")
                .add(FunctionSet.BITMAP_UNION_INT.toUpperCase()).build();

        @Override
        public Void visit(OptExpression optExpression, PreAggregationContext context) {
            // Avoid left child modify context will effect right child
            if (optExpression.getInputs().size() <= 1) {
                for (OptExpression opt : optExpression.getInputs()) {
                    if (opt.getOp().getProjection() != null) {
                        rewriteProject(opt, context);
                    }

                    opt.getOp().accept(this, opt, context);
                }
            } else {
                for (OptExpression opt : optExpression.getInputs()) {
                    if (opt.getOp().getProjection() != null) {
                        rewriteProject(opt, context);
                    }

                    opt.getOp().accept(this, opt, context.clone());
                }
            }

            return null;
        }

        void rewriteProject(OptExpression opt, PreAggregationContext context) {
            Projection projection = opt.getOp().getProjection();
            ReplaceColumnRefRewriter rewriter = new ReplaceColumnRefRewriter(projection.getColumnRefMap());
            ReplaceColumnRefRewriter subRewriter =
                    new ReplaceColumnRefRewriter(projection.getCommonSubOperatorMap(), true);

            context.aggregations = context.aggregations.stream()
                    .map(d -> d.accept(rewriter, null).accept(subRewriter, null))
                    .collect(Collectors.toList());

            context.groupings = context.groupings.stream()
                    .map(d -> d.accept(rewriter, null).accept(subRewriter, null))
                    .collect(Collectors.toList());
        }

        @Override
        public Void visitPhysicalHashAggregate(OptExpression optExpression, PreAggregationContext context) {
            PhysicalHashAggregateOperator aggregate = (PhysicalHashAggregateOperator) optExpression.getOp();
            // Only save the recently aggregate
            context.aggregations =
                    aggregate.getAggregations().values().stream().map(CallOperator::clone).collect(Collectors.toList());
            context.groupings =
                    aggregate.getGroupBys().stream().map(ScalarOperator::clone).collect(Collectors.toList());
            context.notPreAggregationJoin = false;

            return visit(optExpression, context);
        }

        @Override
        public Void visitPhysicalOlapScan(OptExpression optExpression, PreAggregationContext context) {
            PhysicalOlapScanOperator scan = (PhysicalOlapScanOperator) optExpression.getOp();

            // default false
            scan.setPreAggregation(false);

            // Duplicate table
            if (!((OlapTable) scan.getTable()).getKeysType().isAggregationFamily()) {
                scan.setPreAggregation(true);
                scan.setTurnOffReason("");
                return null;
            }

            if (context.notPreAggregationJoin) {
                scan.setTurnOffReason("Has can not pre-aggregation Join");
                return null;
            }

            if (context.aggregations.isEmpty() && context.groupings.isEmpty()) {
                scan.setTurnOffReason("None aggregate function");
                return null;
            }
            // check has value conjunct
            boolean allKeyConjunct =
                    Utils.extractColumnRef(
                                    Utils.compoundAnd(scan.getPredicate(), Utils.compoundAnd(context.joinPredicates))).stream()
                            .map(ref -> scan.getColRefToColumnMetaMap().get(ref)).filter(Objects::nonNull)
                            .allMatch(Column::isKey);
            if (!allKeyConjunct) {
                scan.setTurnOffReason("Predicates include the value column");
                return null;
            }

            // check grouping
            if (checkGroupings(context, scan)) {
                return null;
            }

            // check aggregation function
            if (checkAggregations(context, scan)) {
                return null;
            }

            scan.setPreAggregation(true);
            scan.setTurnOffReason("");
            return null;
        }

        private boolean checkGroupings(PreAggregationContext context, PhysicalOlapScanOperator scan) {
            Map<ColumnRefOperator, Column> refColumnMap = scan.getColRefToColumnMetaMap();

            List<ColumnRefOperator> groups = Lists.newArrayList();
            context.groupings.stream().map(Utils::extractColumnRef).forEach(groups::addAll);

            if (groups.stream().anyMatch(d -> !refColumnMap.containsKey(d))) {
                scan.setTurnOffReason("Group columns isn't bound table " + scan.getTable().getName());
                return true;
            }

            if (groups.stream().anyMatch(d -> !refColumnMap.get(d).isKey())) {
                scan.setTurnOffReason("Group columns isn't Key column");
                return true;
            }
            return false;
        }

        private boolean checkAggregations(PreAggregationContext context, PhysicalOlapScanOperator scan) {
            Map<ColumnRefOperator, Column> refColumnMap = scan.getColRefToColumnMetaMap();

            for (final ScalarOperator so : context.aggregations) {
                Preconditions.checkState(OperatorType.CALL.equals(so.getOpType()));

                CallOperator call = (CallOperator) so;
                if (call.getChildren().size() != 1) {
                    scan.setTurnOffReason("Aggregate function has more than one parameter");
                    return true;
                }

                ScalarOperator child = call.getChild(0);

                if (child instanceof CallOperator &&
                        FunctionSet.IF.equalsIgnoreCase(((CallOperator) child).getFnName())) {
                    child = new CaseWhenOperator(child.getType(), null, child.getChild(2),
                            Lists.newArrayList(child.getChild(0), child.getChild(1)));
                }

                List<ColumnRefOperator> returns = Lists.newArrayList();
                List<ColumnRefOperator> conditions = Lists.newArrayList();

                if (OperatorType.VARIABLE.equals(child.getOpType())) {
                    returns.add((ColumnRefOperator) child);
                } else if (child instanceof CastOperator
                        && OperatorType.VARIABLE.equals(child.getChild(0).getOpType())) {
                    if (child.getType().isNumericType() && child.getChild(0).getType().isNumericType()) {
                        returns.add((ColumnRefOperator) child.getChild(0));
                    } else {
                        scan.setTurnOffReason("The parameter of aggregate function isn't numeric type");
                        return true;
                    }
                } else if (child instanceof CaseWhenOperator) {
                    CaseWhenOperator cwo = (CaseWhenOperator) child;

                    for (int i = 0; i < cwo.getWhenClauseSize(); i++) {
                        if (!cwo.getThenClause(i).isColumnRef()) {
                            scan.setTurnOffReason("The result of THEN isn't value column");
                            return true;
                        }

                        conditions.addAll(Utils.extractColumnRef(cwo.getWhenClause(i)));
                        returns.add((ColumnRefOperator) cwo.getThenClause(i));
                    }

                    if (cwo.hasCase()) {
                        conditions.addAll(Utils.extractColumnRef(cwo.getCaseClause()));
                    }

                    if (cwo.hasElse()) {
                        if (OperatorType.VARIABLE.equals(cwo.getElseClause().getOpType())) {
                            returns.add((ColumnRefOperator) cwo.getElseClause());
                        } else if (OperatorType.CONSTANT.equals(cwo.getElseClause().getOpType())
                                && ((ConstantOperator) cwo.getElseClause()).isNull()) {
                            // NULL don't effect result, can open PreAggregate
                        } else {
                            scan.setTurnOffReason("The result of ELSE isn't value column");
                            return true;
                        }
                    }
                } else {
                    scan.setTurnOffReason(
                            "The parameter of aggregate function isn't value column or CAST/CASE-WHEN expression");
                    return true;
                }

                // check conditions
                if (conditions.stream().anyMatch(d -> !refColumnMap.containsKey(d))) {
                    scan.setTurnOffReason("The column of aggregate function isn't bound " + scan.getTable().getName());
                    return true;
                }

                if (conditions.stream().anyMatch(d -> !refColumnMap.get(d).isKey())) {
                    scan.setTurnOffReason("The column of aggregate function isn't key");
                    return true;
                }

                // check returns
                for (ColumnRefOperator ref : returns) {
                    if (!refColumnMap.containsKey(ref)) {
                        scan.setTurnOffReason(
                                "The column of aggregate function isn't bound " + scan.getTable().getName());
                        return true;
                    }

                    Column column = refColumnMap.get(ref);
                    // key column
                    if (column.isKey()) {
                        if (!"MAX|MIN".contains(call.getFnName().toUpperCase()) &&
                                !AGGREGATE_ONLY_KEY.contains(call.getFnName().toUpperCase())) {
                            scan.setTurnOffReason("The key column don't support aggregate function: "
                                    + call.getFnName().toUpperCase());
                            return true;
                        }
                        continue;
                    }

                    // value column
                    if ("HLL_UNION_AGG|HLL_RAW_AGG".contains(call.getFnName().toUpperCase())) {
                        // skip
                    } else if (AGGREGATE_ONLY_KEY.contains(call.getFnName().toUpperCase())) {
                        scan.setTurnOffReason(
                                "Aggregation function " + call.getFnName().toUpperCase() + " just work on key column");
                        return true;
                    } else if ((FunctionSet.BITMAP_UNION.equalsIgnoreCase(call.getFnName())
                            || FunctionSet.BITMAP_UNION_COUNT.equalsIgnoreCase(call.getFnName()))) {
                        if (!AggregateType.BITMAP_UNION.equals(column.getAggregationType())) {
                            scan.setTurnOffReason(
                                    "Aggregate Operator not match: BITMAP_UNION <--> " + column.getAggregationType());
                        }
                    } else if (!call.getFnName().equalsIgnoreCase(column.getAggregationType().name())) {
                        scan.setTurnOffReason(
                                "Aggregate Operator not match: " + call.getFnName().toUpperCase() + " <--> " + column
                                        .getAggregationType().name().toUpperCase());
                        return true;
                    }
                }
            }
            return false;
        }

        @Override
        public Void visitPhysicalHashJoin(OptExpression optExpression, PreAggregationContext context) {
            PhysicalHashJoinOperator hashJoinOperator = (PhysicalHashJoinOperator) optExpression.getOp();
            OptExpression leftChild = optExpression.getInputs().get(0);
            OptExpression rightChild = optExpression.getInputs().get(1);

            ColumnRefSet leftOutputColumns = optExpression.getInputs().get(0).getOutputColumns();
            ColumnRefSet rightOutputColumns = optExpression.getInputs().get(1).getOutputColumns();

            List<BinaryPredicateOperator> eqOnPredicates = JoinPredicateUtils.getEqConj(leftOutputColumns,
                    rightOutputColumns, Utils.extractConjuncts(hashJoinOperator.getJoinPredicate()));
            // cross join can not do pre-aggregation
            if (hashJoinOperator.getJoinType().isCrossJoin() || eqOnPredicates.isEmpty()) {
                context.notPreAggregationJoin = true;
                context.groupings.clear();
                context.aggregations.clear();
                return visit(optExpression, context);
            }

            // For other types of joins, only one side can turn on pre aggregation which side has aggregation.
            // The columns used by the aggregate functions can only be the columns of one of the children,
            // the olap scan node will turn off pre aggregation if aggregation function used both sides columns,
            // this can be guaranteed by checkAggregations in visitPhysicalOlapScan.
            ColumnRefSet aggregationColumns = new ColumnRefSet();
            List<ScalarOperator> leftGroupOperator = Lists.newArrayList();
            List<ScalarOperator> rightGroupOperator = Lists.newArrayList();

            context.groupings.forEach(g -> {
                if (leftOutputColumns.contains(g.getUsedColumns())) {
                    leftGroupOperator.add(g);
                }
            });
            context.groupings.forEach(g -> {
                if (rightOutputColumns.contains(g.getUsedColumns())) {
                    rightGroupOperator.add(g);
                }
            });
            context.aggregations.forEach(a -> aggregationColumns.union(a.getUsedColumns()));
            boolean checkLeft = leftOutputColumns.contains(aggregationColumns);
            boolean checkRight = rightOutputColumns.contains(aggregationColumns);
            // Add join on predicate and predicate to context
            context.joinPredicates.add(hashJoinOperator.getJoinPredicate());
            context.joinPredicates.add(hashJoinOperator.getPredicate());

            PreAggregationContext disableContext = new PreAggregationContext();
            disableContext.notPreAggregationJoin = true;

            if (checkLeft) {
                context.groupings = leftGroupOperator;
                leftChild.getOp().accept(this, leftChild, context);
                rightChild.getOp().accept(this, rightChild, disableContext);
            } else if (checkRight) {
                context.groupings = rightGroupOperator;
                rightChild.getOp().accept(this, rightChild, context);
                leftChild.getOp().accept(this, leftChild, disableContext);
            } else {
                leftChild.getOp().accept(this, leftChild, disableContext);
                rightChild.getOp().accept(this, rightChild, disableContext);
            }
            return null;
        }
    }

    public static class PreAggregationContext implements Cloneable {
        // Indicates that a pre-aggregation can not be turned on below the join
        public boolean notPreAggregationJoin = false;
        public List<ScalarOperator> aggregations = Lists.newArrayList();
        public List<ScalarOperator> groupings = Lists.newArrayList();
        public List<ScalarOperator> joinPredicates = Lists.newArrayList();

        @Override
        public PreAggregationContext clone() {
            try {
                PreAggregationContext context = (PreAggregationContext) super.clone();
                // Just shallow copy
                context.aggregations = Lists.newArrayList(aggregations);
                context.groupings = Lists.newArrayList(groupings);
                context.joinPredicates = Lists.newArrayList(joinPredicates);
                return context;
            } catch (CloneNotSupportedException ignored) {
            }

            return null;
        }
    }
}

// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.
package com.starrocks.sql.optimizer.rule.transformation;

import com.google.common.collect.Lists;
import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptimizerContext;
import com.starrocks.sql.optimizer.base.ColumnRefSet;
import com.starrocks.sql.optimizer.base.Ordering;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.logical.LogicalWindowOperator;
import com.starrocks.sql.optimizer.operator.pattern.Pattern;
import com.starrocks.sql.optimizer.operator.scalar.CallOperator;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;
import com.starrocks.sql.optimizer.rule.RuleType;

import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class PruneWindowColumnsRule extends TransformationRule {
    public PruneWindowColumnsRule() {
        super(RuleType.TF_PRUNE_ANALYTIC_COLUMNS, Pattern.create(OperatorType.LOGICAL_WINDOW).
                addChildren(Pattern.create(OperatorType.PATTERN_LEAF)
                        .addChildren(Pattern.create(OperatorType.PATTERN_MULTI_LEAF))));
    }

    @Override
    public List<OptExpression> transform(OptExpression input, OptimizerContext context) {
        LogicalWindowOperator windowOperator = (LogicalWindowOperator) input.getOp();
        ColumnRefSet requiredOutputColumns = context.getTaskContext().get(0).getRequiredColumns();
        ColumnRefSet requiredInputColumns = new ColumnRefSet();
        requiredInputColumns.union(requiredOutputColumns);

        Map<ColumnRefOperator, CallOperator> newWindowCall = new HashMap<>();
        windowOperator.getWindowCall().forEach((columnRefOperator, callOperator) -> {
            if (requiredOutputColumns.contains(columnRefOperator)) {
                newWindowCall.put(columnRefOperator, callOperator);
                requiredOutputColumns.union(callOperator.getUsedColumns());
            }
        });

        windowOperator.getPartitionExpressions().forEach(e -> requiredOutputColumns.union(e.getUsedColumns()));
        windowOperator.getOrderByElements().stream().map(Ordering::getColumnRef).forEach(
                e -> requiredOutputColumns.union(e.getUsedColumns()));

        if (newWindowCall.keySet().equals(windowOperator.getWindowCall().keySet())) {
            return Collections.emptyList();
        }

        //If newWindowCall is empty, it will be clipped in PruneEmptyWindowRule,
        // so it is directly transmitted requiredOutputColumns here
        if (newWindowCall.isEmpty()) {
            requiredOutputColumns.clear();
            requiredOutputColumns.union(requiredInputColumns);
        }

        return Lists.newArrayList(OptExpression.create(
                new LogicalWindowOperator.Builder().withOperator(windowOperator).setWindowCall(newWindowCall).build(),
                input.getInputs()));
    }
}

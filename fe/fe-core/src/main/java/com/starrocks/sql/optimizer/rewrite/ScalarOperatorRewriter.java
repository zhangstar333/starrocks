// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

package com.starrocks.sql.optimizer.rewrite;

import com.google.common.collect.Lists;
import com.starrocks.common.Config;
import com.starrocks.sql.common.ErrorType;
import com.starrocks.sql.common.StarRocksPlannerException;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.sql.optimizer.rewrite.scalar.ArithmeticCommutativeRule;
import com.starrocks.sql.optimizer.rewrite.scalar.ExtractCommonPredicateRule;
import com.starrocks.sql.optimizer.rewrite.scalar.FoldConstantsRule;
import com.starrocks.sql.optimizer.rewrite.scalar.ImplicitCastRule;
import com.starrocks.sql.optimizer.rewrite.scalar.NormalizePredicateRule;
import com.starrocks.sql.optimizer.rewrite.scalar.ReduceCastRule;
import com.starrocks.sql.optimizer.rewrite.scalar.SimplifiedPredicateRule;
import com.starrocks.sql.optimizer.rewrite.scalar.SimplifiedSameColumnRule;

import java.util.List;

public class ScalarOperatorRewriter {
    public static final List<ScalarOperatorRewriteRule> DEFAULT_TYPE_CAST_RULE = Lists.newArrayList(
            new ImplicitCastRule()
    );

    public static final List<ScalarOperatorRewriteRule> DEFAULT_REWRITE_RULES = Lists.newArrayList(
            // required
            new ImplicitCastRule(),
            // optional
            new ReduceCastRule(),
            new NormalizePredicateRule(),
            new FoldConstantsRule(),
            new SimplifiedPredicateRule(),
            new ExtractCommonPredicateRule(),
            new ArithmeticCommutativeRule()
    );

    public static final List<ScalarOperatorRewriteRule> DEFAULT_REWRITE_SCAN_PREDICATE_RULES = Lists.newArrayList(
            // required
            new ImplicitCastRule(),
            // optional
            new ReduceCastRule(),
            new NormalizePredicateRule(),
            new FoldConstantsRule(),
            new SimplifiedSameColumnRule(),
            new SimplifiedPredicateRule(),
            new ExtractCommonPredicateRule(),
            new ArithmeticCommutativeRule()
    );
    private final ScalarOperatorRewriteContext context;

    public ScalarOperatorRewriter() {
        context = new ScalarOperatorRewriteContext();
    }

    public ScalarOperator rewrite(ScalarOperator root, List<ScalarOperatorRewriteRule> ruleList) {
        ScalarOperator result = root;

        context.reset();
        int changeNums;
        do {
            changeNums = context.changeNum();
            for (ScalarOperatorRewriteRule rule : ruleList) {
                result = rewriteByRule(result, rule);
            }

            if (changeNums > Config.max_planner_scalar_rewrite_num) {
                throw new StarRocksPlannerException("Planner rewrite scalar operator over limit",
                        ErrorType.INTERNAL_ERROR);
            }
        } while (changeNums != context.changeNum());

        return result;
    }

    private ScalarOperator rewriteByRule(ScalarOperator root, ScalarOperatorRewriteRule rule) {
        ScalarOperator result = root;
        int changeNums;
        if (rule.isBottomUp()) {
            do {
                changeNums = context.changeNum();
                result = applyRuleBottomUp(result, rule);
            } while (changeNums != context.changeNum());
        } else if (rule.isTopDown()) {
            do {
                changeNums = context.changeNum();
                result = applyRuleTopDown(result, rule);
            } while (changeNums != context.changeNum());
        }

        return result;
    }

    private ScalarOperator applyRuleBottomUp(ScalarOperator operator, ScalarOperatorRewriteRule rule) {
        for (int i = 0; i < operator.getChildren().size(); i++) {
            operator.setChild(i, applyRuleBottomUp(operator.getChild(i), rule));
        }

        ScalarOperator op = rule.apply(operator, context);
        if (op != operator) {
            context.change();
        }
        return op;
    }

    private ScalarOperator applyRuleTopDown(ScalarOperator operator, ScalarOperatorRewriteRule rule) {
        ScalarOperator op = rule.apply(operator, context);
        if (op != operator) {
            context.change();
        }

        for (int i = 0; i < op.getChildren().size(); i++) {
            op.setChild(i, applyRuleTopDown(op.getChild(i), rule));
        }
        return op;
    }
}

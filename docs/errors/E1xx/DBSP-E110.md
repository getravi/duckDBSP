# DBSP-E110: Plan Operator Not Supported

The planner frontend translates view SQL through DuckDB's own
binder/planner. When the bound logical plan contains an operator the
circuit translator cannot map, view creation fails with this code and the
message names the operator.

## Supported SQL (no E110)

Filter/projection with arbitrary expressions, GROUP BY aggregation
(COUNT/SUM/AVG/MIN/MAX/FIRST incl. exact DECIMAL SUM, HAVING, expression
keys, global aggregates, DISTINCT and FILTER (WHERE ...) modifiers,
ROLLUP / CUBE / GROUPING SETS with GROUPING()), inner and outer joins
(LEFT/RIGHT/FULL; equi +
residual predicates), cross joins, subqueries (IN / NOT IN, EXISTS /
NOT EXISTS, scalar comparisons — correlated included), DISTINCT, DISTINCT ON,
UNION/INTERSECT/EXCEPT (ALL and DISTINCT), window functions over plain
columns, non-recursive CTEs, WITH RECURSIVE (including deletions),
ORDER BY / LIMIT / OFFSET (constant).

## Common triggers and rewrites

**WITH RECURSIVE ... USING KEY** — use plain WITH RECURSIVE.

**Non-constant / percentage LIMIT** — use a constant LIMIT.

**Window ORDER BY / PARTITION BY over expressions** — project the
expression to a column in an inner view first, then window over the
column.

**Order-sensitive aggregate functions (string_agg, array_agg, ...)** —
not yet translated; ORDER BY inside the supported order-insensitive
aggregates (SUM/COUNT/AVG/MIN/MAX) is accepted and ignored.

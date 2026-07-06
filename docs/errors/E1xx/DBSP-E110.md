# DBSP-E110: Plan Operator Not Supported

The planner frontend translates view SQL through DuckDB's own
binder/planner. When the bound logical plan contains an operator the
circuit translator cannot map, view creation fails with this code and the
message names the operator.

## Supported SQL (no E110)

Filter/projection with arbitrary expressions, GROUP BY aggregation
(COUNT/SUM/AVG/MIN/MAX/FIRST/MEDIAN/QUANTILE_CONT/QUANTILE_DISC/MODE
incl. exact DECIMAL SUM, HAVING, expression
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

**Non-constant (expression) LIMIT** — use a constant or percentage
LIMIT.

**approx_quantile / reservoir_quantile / approx_top_k** — approximate
algorithms; results cannot match DuckDB's, and mapping them to exact
equivalents would silently differ. Use median/quantile_cont/
quantile_disc/mode/mad (exact, supported).

**string_agg / array_agg without ORDER BY** — add an ORDER BY inside
the aggregate (e.g. `string_agg(x, ',' ORDER BY x)`). Unordered results
follow DuckDB's scan order, which cannot be reproduced incrementally.
Ties on the order keys are broken by value; use unique keys for exact
DuckDB parity. ORDER BY inside the order-insensitive aggregates
(SUM/COUNT/AVG/MIN/MAX) is accepted and ignored.

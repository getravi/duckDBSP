# DBSP-E110: Plan Operator Not Supported

The planner frontend translates view SQL through DuckDB's own
binder/planner. When the bound logical plan contains an operator the
circuit translator cannot map, view creation fails with this code and the
message names the operator.

## Supported SQL (no E110)

Filter/projection with arbitrary expressions, GROUP BY aggregation
(COUNT/SUM/AVG/MIN/MAX/FIRST incl. exact DECIMAL SUM, HAVING, expression
keys, global aggregates), inner and outer joins (LEFT/RIGHT/FULL; equi +
residual predicates), cross joins, IN / NOT IN subqueries, uncorrelated
scalar subquery comparisons, DISTINCT, DISTINCT ON,
UNION/INTERSECT/EXCEPT (ALL and DISTINCT), window functions over plain
columns, non-recursive CTEs, WITH RECURSIVE (including deletions),
ORDER BY / LIMIT / OFFSET (constant).

## Common triggers and rewrites

**Correlated subquery (DELIM_JOIN)** — the subquery references the outer
row. (Uncorrelated scalar subqueries and IN/NOT IN translate directly.)

```sql
-- Instead of:
SELECT * FROM orders o
WHERE amount > (SELECT AVG(amount) FROM orders o2
                WHERE o2.customer = o.customer);

-- Do this:
SELECT * FROM dbsp_create_view('avg_by_customer',
    'SELECT customer, AVG(amount) AS a FROM orders GROUP BY customer');
SELECT * FROM dbsp_create_view('big_orders',
    'SELECT o.* FROM orders o JOIN avg_by_customer v '
    'ON o.customer = v.customer AND o.amount > v.a');
```

**WITH RECURSIVE ... USING KEY** — use plain WITH RECURSIVE.

**Non-constant / percentage LIMIT** — use a constant LIMIT.

**Window ORDER BY / PARTITION BY over expressions** — project the
expression to a column in an inner view first, then window over the
column.

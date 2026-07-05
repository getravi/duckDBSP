# DBSP-E110: Plan Operator Not Supported

The planner frontend translates view SQL through DuckDB's own
binder/planner. When the bound logical plan contains an operator the
circuit translator cannot map, view creation fails with this code and the
message names the operator.

## Supported SQL (no E110)

Filter/projection with arbitrary expressions, GROUP BY aggregation
(COUNT/SUM/AVG/MIN/MAX incl. exact DECIMAL SUM, HAVING, expression keys,
global aggregates), inner joins (equi + residual predicates), cross joins,
DISTINCT, DISTINCT ON, UNION/INTERSECT/EXCEPT (ALL and DISTINCT), window
functions over plain columns, non-recursive CTEs, WITH RECURSIVE
(including deletions), ORDER BY / LIMIT / OFFSET (constant).

## Common triggers and rewrites

**Correlated subquery (DELIM_JOIN)**

```sql
-- Instead of:
SELECT * FROM orders o WHERE amount > (SELECT AVG(amount) FROM orders);

-- Do this:
SELECT * FROM dbsp_create_view('avg_amount',
    'SELECT AVG(amount) AS a FROM orders');
SELECT * FROM dbsp_create_view('big_orders',
    'SELECT o.* FROM orders o JOIN avg_amount v ON o.amount > v.a');
```

**Outer joins (LEFT/RIGHT/FULL)** — not yet supported; restructure as an
inner join plus a set-operation view, or track the unmatched side
separately.

**WITH RECURSIVE ... USING KEY** — use plain WITH RECURSIVE.

**Non-constant / percentage LIMIT** — use a constant LIMIT.

**Window ORDER BY / PARTITION BY over expressions** — project the
expression to a column in an inner view first, then window over the
column.

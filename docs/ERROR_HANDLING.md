# Error Handling in duckDBSP

This guide explains the error handling system in duckDBSP and how to troubleshoot common issues.

## Error Code System

All errors in duckDBSP use structured error codes in the format `DBSP-Exxx`:

- **E1xx**: Unsupported SQL (plan shapes the planner frontend cannot translate)
- **E2xx**: Validation errors (invalid input)
- **E3xx**: Runtime errors (execution failures)
- **E4xx**: Resource errors (limits exceeded)
- **E5xx**: Persistence errors (I/O failures)

## Error Message Format

Every error includes:
- **Error code**: e.g., DBSP-E101
- **Description**: What went wrong
- **SQL**: The SQL that caused the error (if applicable)
- **Position marker**: Points to the problem in your SQL (^)
- **Workaround**: How to achieve the same result
- **Documentation**: Link to detailed error documentation

## Example

```
DBSP-E101: HAVING clause in GROUP BY

SQL:
SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 10
                                             ^

Workaround:
Use a nested view: create a view with GROUP BY, then create another view
with WHERE clause to filter the aggregated results. Tracked in TODO #3.

Documentation: docs/errors/E1xx/DBSP-E101.md
```

## Finding Solutions

1. **Check the error code** - Each code links to specific documentation
2. **Try the workaround** - Most errors include alternative approaches
3. **Check TODO.md** - See if the feature is planned
4. **Review examples/** - See working code patterns

## Common Errors

### E101: HAVING Not Supported

**Solution**: Use nested views

```sql
-- Instead of:
SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 10

-- Do this:
SELECT * FROM dbsp_create_view('dept_counts',
    'SELECT dept, COUNT(*) as cnt FROM emp GROUP BY dept');
SELECT * FROM dbsp_create_view('large_depts',
    'SELECT * FROM dept_counts WHERE cnt > 10');
```

### E110: Plan Operator Not Supported

The planner frontend names the operator it could not translate (e.g.
correlated subqueries / DELIM_JOIN, outer joins, non-constant LIMIT).

**Solution**: Rewrite the query — e.g. turn a correlated subquery into a
JOIN, or split it into an intermediate view:

```sql
-- Instead of:
SELECT * FROM orders o WHERE amount > (SELECT AVG(amount) FROM orders)

-- Do this:
SELECT * FROM dbsp_create_view('avg_amount',
    'SELECT AVG(amount) AS a FROM orders');
SELECT * FROM dbsp_create_view('big_orders',
    'SELECT o.* FROM orders o JOIN avg_amount v ON o.amount > v.a');
```

Note: ORDER BY / LIMIT are fully supported since Phase C1 — `dbsp_query`
returns rows in ORDER BY order for sorted views.

### E206: View Not Found (replace)

`dbsp_replace_view(name, sql)` / `CREATE OR REPLACE MATERIALIZED VIEW`
requires `name` to already exist — it changes a view's definition, it
doesn't create one.

**Solution**: create the view first, or check the spelling against
`dbsp_views()`:

```sql
-- Instead of:
SELECT * FROM dbsp_replace_view('typo_name', 'SELECT ...');

-- Do this:
SELECT * FROM dbsp_create_view('correct_name', 'SELECT ...');
```

### E304: Cascade Update Failed (replace partial failure)

`dbsp_replace_view` rebuilds the named view and its whole dependent
subtree; there's no transactional rollback in v1. Two things can put a
view in the "lost" list the error message reports:

- A dependent's saved SQL fails to recreate (e.g. it referenced a column
  the new definition dropped) — the remaining dependents are still
  attempted with their saved DDL, so one failure can cascade into further
  ones further down the chain (each reported by name).
- A view created concurrently onto the subtree, in the narrow window
  between `dbsp_replace_view` snapshotting dependent definitions and the
  cascade actually dropping them, gets swept up by the drop but was never
  in that snapshot — its definition is unknown, so it's reported instead
  of silently recreated wrong (or not at all).

Every view named in the "lost" list is fully gone: its live registry
entry, its DuckDB-visible query surface, and its `_dbsp_views` persisted
row are all removed together — a later `dbsp_load()` or auto-load won't
try to resurrect it from a stale row. The error message also lists which
views (the replaced one, and any dependents that did recreate cleanly)
are still queryable.

**Solution**: fix the dependent's SQL (or the replacement SQL) and
retry — recreate any view named in the "lost" list explicitly with
`dbsp_create_view`.

## Getting Help

- Browse [Error Catalog](errors/README.md) for all error codes
- Check [API.md](API.md) for supported features
- See [ARCHITECTURE.md](ARCHITECTURE.md) for how the system works
- Report issues on GitHub

## For Contributors

When adding features that may error:
1. Define error code in `dbsp_errors.hpp`
2. Add workaround to `get_workaround()`
3. Create documentation in `docs/errors/E{category}xx/`
4. Update error catalog README
5. Add test cases in `test/integration/test_error_handling_e2e.cpp`

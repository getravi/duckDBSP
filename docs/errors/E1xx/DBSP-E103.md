# DBSP-E103: LIMIT Not Supported

**Category**: Parser Error
**Severity**: Error
**Since**: Version 1.0

## Description

The LIMIT clause restricts the number of rows returned, but duckDBSP does not support this in materialized view definitions as it complicates incremental updates.

## Common Causes

- Using LIMIT to get top-N results
- Pagination with LIMIT and OFFSET
- Attempting to restrict view size

## Example

### This will fail:
```sql
SELECT * FROM orders LIMIT 100;
```

### Error message:
```
DBSP-E103: LIMIT clause detected

SQL:
SELECT * FROM orders LIMIT 100
                      ^

Workaround:
Query the view and apply LIMIT in the outer query. LIMIT support is tracked in TODO #4.

Documentation: docs/errors/E1xx/DBSP-E103.md
```

## Workaround

Apply LIMIT when querying, not in view definition:

```sql
-- Create view without LIMIT
SELECT * FROM dbsp_create_view('all_orders', 'SELECT * FROM orders');

-- Apply LIMIT when querying
SELECT * FROM dbsp_query('all_orders') LIMIT 100;
```

## Related

- TODO #4 - Add ORDER BY and LIMIT support
- [API.md](../API.md) - Query syntax

## Roadmap

LIMIT support for materialized views is tracked in TODO #4.

## Technical Details

LIMIT on materialized views is challenging because which rows to keep changes as data updates. Combined with ORDER BY, it requires maintaining sorted order incrementally.

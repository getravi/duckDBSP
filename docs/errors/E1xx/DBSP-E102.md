# DBSP-E102: ORDER BY Not Supported

**Category**: Parser Error
**Severity**: Error
**Since**: Version 1.0

## Description

The ORDER BY clause is used to sort query results, but duckDBSP does not yet support this for materialized views as it complicates incremental maintenance.

## Common Causes

- Attempting to sort view results with ORDER BY
- Expecting sorted output from materialized views
- Migrating queries that require specific ordering

## Example

### This will fail:
```sql
SELECT * FROM orders ORDER BY created_at DESC;
```

### Error message:
```
DBSP-E102: ORDER BY clause detected

SQL:
SELECT * FROM orders ORDER BY created_at DESC
                      ^

Workaround:
Query the view using dbsp_query() and sort results client-side. ORDER BY support is tracked in TODO #4.

Documentation: docs/errors/E1xx/DBSP-E102.md
```

## Workaround

Create the view without ORDER BY, then sort when querying:

```sql
-- Create view without ORDER BY
SELECT * FROM dbsp_create_view('all_orders', 'SELECT * FROM orders');

-- Sort results when querying
SELECT * FROM dbsp_query('all_orders') ORDER BY created_at DESC;
```

## Related

- TODO #4 - Add ORDER BY and LIMIT support
- [API.md](../API.md) - View query syntax

## Roadmap

ORDER BY support is tracked in TODO #4. The challenge is maintaining sorted order incrementally as updates arrive.

## Technical Details

Incremental ORDER BY requires complex data structures (like B-trees) to maintain sorted order efficiently as rows are inserted/deleted. This is deferred for future implementation.

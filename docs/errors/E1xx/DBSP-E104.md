# DBSP-E104: Window Functions Not Supported

**Category**: Parser Error
**Severity**: Error
**Since**: Version 1.0

## Description

Window functions (ROW_NUMBER, RANK, LAG, LEAD, etc.) are not yet supported for incremental materialized views in duckDBSP.

## Common Causes

- Using ROW_NUMBER() for ranking
- Attempting to use RANK() or DENSE_RANK()
- Using LAG/LEAD for time-series analysis
- PARTITION BY with window functions

## Example

### This will fail:
```sql
SELECT *, ROW_NUMBER() OVER (PARTITION BY dept ORDER BY salary DESC)
FROM employees;
```

### Error message:
```
DBSP-E104: Window function detected in SELECT list

SQL:
SELECT *, ROW_NUMBER() OVER (PARTITION BY dept ORDER BY salary DESC) FROM employees
          ^

Workaround:
Window functions are not yet supported for incremental computation. Tracked in TODO #8.

Documentation: docs/errors/E1xx/DBSP-E104.md
```

## Workaround

Currently, there is no direct workaround for window functions in duckDBSP. Options:

1. **Compute client-side**: Query the view and apply window functions in your application
2. **Materialize in DuckDB**: Use regular DuckDB views (without incremental updates)
3. **Alternative logic**: Some window functions can be approximated with JOINs and aggregates

## Related

- TODO #8 - Add window function support
- [API.md](../API.md) - Supported SQL features

## Roadmap

Window function support is planned for TODO #8. This is a complex feature requiring sophisticated incremental algorithms.

## Technical Details

Window functions are challenging for incremental computation because:
- ROW_NUMBER requires maintaining global order (expensive to update)
- RANK requires comparing values across partitions
- LAG/LEAD require tracking previous/next rows as data changes

This is an advanced feature deferred to future versions.

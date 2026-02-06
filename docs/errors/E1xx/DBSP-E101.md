# DBSP-E101: HAVING Clause Not Supported

**Category**: Parser Error
**Severity**: Error
**Since**: Version 1.0

## Description

The HAVING clause is used to filter aggregated results after GROUP BY, but duckDBSP does not yet support this SQL feature for incremental materialized views.

## Common Causes

- Using HAVING with GROUP BY to filter aggregated counts, sums, or averages
- Attempting to filter on aggregate functions
- Migrating SQL from other databases that use HAVING

## Example

### This will fail:
```sql
SELECT dept, COUNT(*) as emp_count
FROM employees
GROUP BY dept
HAVING COUNT(*) > 10;
```

### Error message:
```
DBSP-E101: HAVING clause in GROUP BY

SQL:
SELECT dept, COUNT(*) as emp_count FROM employees GROUP BY dept HAVING COUNT(*) > 10
                                                                 ^

Workaround:
Use a nested view: create a view with GROUP BY, then create another view with WHERE clause to filter the aggregated results. Tracked in TODO #3.

Documentation: docs/errors/E1xx/DBSP-E101.md
```

## Workaround

Create two views instead of one:

### Step 1: Create aggregation view
```sql
SELECT * FROM dbsp_create_view('dept_counts',
    'SELECT dept, COUNT(*) as emp_count FROM employees GROUP BY dept');
```

### Step 2: Create filtering view
```sql
SELECT * FROM dbsp_create_view('large_depts',
    'SELECT * FROM dept_counts WHERE emp_count > 10');
```

This achieves the same result and maintains incremental updates through the view cascade.

## Related

- TODO #3 - Implement HAVING clause for aggregate filtering
- [API.md](../API.md#dbsp_create_view) - View creation syntax
- [ARCHITECTURE.md](../ARCHITECTURE.md#cascading-views) - How cascading views work

## Roadmap

HAVING clause support is planned and tracked in TODO #3. It requires:
- Post-aggregation filter logic in `NativeAggregateView`
- Ensuring incremental updates work correctly with HAVING conditions
- Comprehensive testing for edge cases

## Technical Details

The HAVING clause filters rows AFTER aggregation, while WHERE filters BEFORE. For incremental computation:
- WHERE can be processed incrementally per input row
- HAVING requires checking aggregate state after updates

Implementation requires adding a post-aggregation filter stage that re-evaluates conditions when aggregates change.

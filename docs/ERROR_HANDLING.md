# Error Handling in duckDBSP

This guide explains the error handling system in duckDBSP and how to troubleshoot common issues.

## Error Code System

All errors in duckDBSP use structured error codes in the format `DBSP-Exxx`:

- **E1xx**: Parser errors (unsupported SQL features)
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

### E102: ORDER BY Not Supported

**Solution**: Sort results after querying

```sql
-- Instead of:
SELECT * FROM orders ORDER BY created_at DESC

-- Do this:
SELECT * FROM dbsp_create_view('all_orders', 'SELECT * FROM orders');

-- Then in your application:
SELECT * FROM dbsp_query('all_orders') ORDER BY created_at DESC;
```

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
5. Add test cases in `test/unit/test_parser_errors.cpp`

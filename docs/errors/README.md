# duckDBSP Error Catalog

Complete reference for all error codes in duckDBSP.

## Error Categories

- [E1xx: Parser Errors](#e1xx-parser-errors) - Unsupported SQL features
- [E2xx: Validation Errors](#e2xx-validation-errors) - Invalid input
- [E3xx: Runtime Errors](#e3xx-runtime-errors) - Execution failures
- [E4xx: Resource Errors](#e4xx-resource-errors) - Limits exceeded
- [E5xx: Persistence Errors](#e5xx-persistence-errors) - I/O failures

## E1xx: Parser Errors

Unsupported SQL features that are detected during query parsing.

| Code | Description | Workaround Available | TODO |
|------|-------------|---------------------|------|
| E101 | HAVING clause not supported | Yes | #3 |
| E102 | ORDER BY not supported | Yes | #4 |
| E103 | LIMIT not supported | Yes | #4 |
| E104 | Window functions not supported | No | #8 |
| E105 | Subqueries not supported | Yes | #6 |
| E106 | UNION not supported | Yes | #7 |
| E107 | INTERSECT not supported | Yes | #7 |
| E108 | EXCEPT not supported | Yes | #7 |

## E2xx: Validation Errors

Invalid input caught during validation.

| Code | Description | Workaround Available | TODO |
|------|-------------|---------------------|------|
| E201 | Invalid identifier name | Yes | - |
| E202 | Path traversal attempt | Yes | - |
| E203 | Circular dependency detected | Yes | - |
| E204 | Identifier too long | Yes | - |

## E3xx: Runtime Errors

Errors that occur during view updates or query execution.

| Code | Description | Workaround Available | TODO |
|------|-------------|---------------------|------|
| E301 | View update failed | Yes | - |
| E302 | Type mismatch | Yes | - |
| E303 | NULL constraint violation | Yes | #11 |

## E4xx: Resource Errors

Errors related to resource limits and constraints.

| Code | Description | Workaround Available | TODO |
|------|-------------|---------------------|------|
| E401 | Memory limit exceeded | No | #10 |
| E402 | Too many views | No | #10 |
| E403 | View too large | No | #10 |

## E5xx: Persistence Errors

Errors related to saving/loading views.

| Code | Description | Workaround Available | TODO |
|------|-------------|---------------------|------|
| E501 | File read error | Yes | - |
| E502 | File write error | Yes | - |
| E503 | Serialization error | Yes | - |

## Quick Reference

### Most Common Errors

1. **E102: ORDER BY not supported** - Sort results after querying with dbsp_query()
2. **E101: HAVING not supported** - Use nested views: GROUP BY first, then WHERE
3. **E105: Subqueries not supported** - Rewrite with JOINs or create intermediate views
4. **E103: LIMIT not supported** - Apply LIMIT in outer query after dbsp_query()

### Getting Help

- Read the specific error documentation page (click error code above)
- Check [TODO.md](../../TODO.md) for roadmap and planned features
- Review [docs/API.md](../API.md) for currently supported features
- See [examples/](../../examples/) directory for working code patterns
- Report issues at [GitHub Issues](https://github.com/yourusername/duckDBSP/issues)

## Contributing

When adding new error codes:

1. Add error code to `dbsp_errors.hpp` enum
2. Add workaround to `get_workaround()` function
3. Create documentation page using `templates/error-template.md`
4. Update this README.md with new error in appropriate category
5. Add test cases in `test/unit/test_parser_errors.cpp`

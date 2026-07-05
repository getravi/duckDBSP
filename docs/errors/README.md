# duckDBSP Error Catalog

Reference for `DBSP-Exxx` error codes.

## Categories

| Range | Category | Codes |
|-------|----------|-------|
| E1xx | Unsupported SQL | [E110](E1xx/DBSP-E110.md) — plan operator not supported |
| E2xx | Validation errors | E201 invalid identifier, E202 path traversal, E203 circular dependency, E204 identifier too long, E205 reserved keyword |
| E3xx | Runtime errors | E301 view update failed, E302 type mismatch, E303 NULL constraint, E304 cascade update failed |
| E4xx | Resource errors | E401 memory limit, E402 too many views, E403 view too large, E404 nesting depth |
| E5xx | Persistence errors | E501 file read, E502 file write, E503 serialization, E504 deserialization |

## Notes

- E101–E109 were parser-era codes for SQL features the planner frontend
  now supports (HAVING, ORDER BY/LIMIT, window functions, set operations,
  CTEs). They were retired with the bespoke parser in Phase C5. Any
  unsupported SQL now surfaces as a single E110 whose message names the
  logical operator that could not be translated.
- Error strings and workarounds live in `include/dbsp_errors.hpp`
  (`get_message`, `get_workaround`, `format_error`).

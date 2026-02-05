# Security Vulnerability Fixes

## Summary

Fixed two HIGH severity security vulnerabilities identified in security review:

1. **SQL Injection via View/Table Names** - FIXED
2. **Path Traversal in File Operations** - FIXED

## Vulnerabilities Fixed

### 1. SQL Injection in View/Table Names (HIGH)

**Vulnerability:** View and table names were concatenated directly into SQL queries without proper validation, allowing SQL injection attacks.

**Affected Code:**
- `save_to_table()` - View names in INSERT statements (lines 511-513, 529)
- `create_view()` - View name used without validation
- `track_table()` - Table name used without validation

**Fix Implemented:**
- Added `is_valid_identifier()` function to validate SQL identifiers
- Validates identifiers are alphanumeric + underscore only
- First character must be letter or underscore
- Length limited to 255 characters
- Applied validation at entry points: `create_view()`, `track_table()`
- Applied validation in persistence: `save_to_table()`
- Added `escape_string()` calls for defense-in-depth

**Attack Vector Blocked:**
```sql
-- Attempted injection via view name
dbsp_create_view('malicious\'; DROP TABLE users; --', 'SELECT * FROM data')
-- Now rejected with error: "Invalid view name (must be alphanumeric/underscore only)"
```

**Code Location:** `/Users/ravi/Documents/Dev/duckDBSP/duckdb_extension/dbsp_cdc.hpp`
- Lines 30-48: `is_valid_identifier()` function
- Lines 293-298: Validation in `create_view()`
- Lines 260-268: Validation in `track_table()`
- Lines 500-526: Validation in `save_to_table()`

### 2. Path Traversal in File Operations (HIGH)

**Vulnerability:** File paths in `save_to_file()` and `load_from_file()` were used directly without validation, allowing path traversal attacks to read/write arbitrary files.

**Affected Code:**
- `save_to_file()` - Direct use of filepath parameter (line 628)
- `load_from_file()` - Direct use of filepath parameter (line 689)

**Fix Implemented:**
- Added `validate_filepath()` function to prevent path traversal
- Rejects absolute paths (starting with `/`)
- Rejects path traversal patterns (`..`)
- Rejects home directory expansion (`~`)
- Rejects null bytes
- Rejects Windows drive letters and backslashes
- Only accepts relative paths within current directory

**Attack Vector Blocked:**
```sql
-- Attempted path traversal
dbsp_save('../../../etc/passwd')
-- Now rejected with error: "Invalid file path (must be relative, no path traversal)"

-- Attempted absolute path
dbsp_save('/etc/passwd')
-- Now rejected with error: "Invalid file path (must be relative, no path traversal)"
```

**Code Location:** `/Users/ravi/Documents/Dev/duckDBSP/duckdb_extension/dbsp_cdc.hpp`
- Lines 50-88: `validate_filepath()` function
- Lines 624-634: Validation in `save_to_file()`
- Lines 685-695: Validation in `load_from_file()`

## Testing

### Unit Tests

Created comprehensive unit tests for validation functions:

**File:** `/Users/ravi/Documents/Dev/duckDBSP/test/unit/test_security_validation.cpp`

Tests cover:
- Valid identifier acceptance (alphanumeric, underscores)
- Special character rejection (dash, dot, semicolon, quotes, spaces)
- SQL injection attempt blocking
- Identifiers starting with numbers
- Empty and oversized identifiers
- Valid relative path acceptance
- Absolute path rejection
- Path traversal blocking (`..`, `/`, `~`)
- Null byte rejection
- Windows path rejection

**Results:** All validation tests pass ✅

### Integration Tests

Created integration tests for end-to-end security:

**File:** `/Users/ravi/Documents/Dev/duckDBSP/test/integration/test_security.cpp`

Tests cover:
- SQL injection via view names (rejected)
- SQL injection via table names (rejected)
- Path traversal in save operations (rejected)
- Path traversal in load operations (rejected)
- Valid operations still work correctly

## Verification

Standalone test run:
```
Testing is_valid_identifier()...
  ✓ Valid identifiers accepted
  ✓ Special characters rejected
  ✓ SQL injection attempts blocked
  ✓ Identifiers starting with numbers rejected
  ✓ Length validation works

Testing validate_filepath()...
  ✓ Valid relative paths accepted
  ✓ Absolute paths rejected
  ✓ Path traversal attempts blocked
  ✓ Home directory expansion blocked

✅ ALL SECURITY VALIDATION TESTS PASSED!
```

## Impact

**Before Fix:**
- Attackers could inject arbitrary SQL via view/table names
- Attackers could read/write arbitrary files on system

**After Fix:**
- All identifiers strictly validated (alphanumeric + underscore)
- All file paths restricted to safe relative paths
- Clear error messages on validation failure
- Defense-in-depth with both validation and escaping

## Files Modified

1. `/Users/ravi/Documents/Dev/duckDBSP/duckdb_extension/dbsp_cdc.hpp`
   - Added security validation functions
   - Applied validation at all entry points
   - Enhanced persistence security

2. `/Users/ravi/Documents/Dev/duckDBSP/test/unit/test_security_validation.cpp` (NEW)
   - Unit tests for validation functions

3. `/Users/ravi/Documents/Dev/duckDBSP/test/integration/test_security.cpp` (NEW)
   - Integration tests for security fixes

4. `/Users/ravi/Documents/Dev/duckDBSP/CMakeLists.txt`
   - Added security test files to build

## Backward Compatibility

**Breaking Changes:**
- View/table names with special characters (dash, dot, etc.) now rejected
- Absolute file paths now rejected
- Path traversal patterns now rejected

**Mitigation:**
- Standard SQL naming conventions already use alphanumeric + underscore
- Relative paths are safer and more portable
- Error messages clearly explain requirements

## Recommendations

1. ✅ **Completed:** Input validation at entry points
2. ✅ **Completed:** Path restriction for file operations
3. ✅ **Completed:** Comprehensive test coverage
4. **Future:** Consider using prepared statements for SQL operations
5. **Future:** Add configurable allowed directories for file operations
6. **Future:** Add security audit logging

## References

- OWASP SQL Injection: https://owasp.org/www-community/attacks/SQL_Injection
- OWASP Path Traversal: https://owasp.org/www-community/attacks/Path_Traversal
- CWE-89: SQL Injection
- CWE-22: Path Traversal

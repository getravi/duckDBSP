# Function Registration Fix Design

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix extension function registration by using proper DuckDB init signature

**Architecture:** Replace DatabaseInstance-based init with ExtensionLoader-based init using DUCKDB_CPP_EXTENSION_ENTRY macro

**Tech Stack:** DuckDB C++ Extension API, CMake

---

## Problem Statement

Current init function signature causes "Missing DB manager" error:
```cpp
void dbsp_duckdb_cpp_init(duckdb::DatabaseInstance &db)
```

When we create `ExtensionLoader(db, "dbsp")` ourselves, it accesses the database manager before it's initialized. This is a timing issue in the extension loading lifecycle.

## Solution: Use DUCKDB_CPP_EXTENSION_ENTRY Macro

### Core Change

Replace current init with proper signature:

**Before:**
```cpp
extern "C" {
DUCKDB_EXTENSION_API void dbsp_duckdb_cpp_init(duckdb::DatabaseInstance &db) {
  (void)db; // Suppressed - couldn't use it
}

DUCKDB_EXTENSION_API void dbsp_init(duckdb::DatabaseInstance &db) {
  dbsp_duckdb_cpp_init(db);
}
}
```

**After:**
```cpp
extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(dbsp, loader) {
  duckdb::LoadInternal(loader);
}

DUCKDB_EXTENSION_API const char *dbsp_version() {
  return "0.1.0";
}
}
```

### What the Macro Does

`DUCKDB_CPP_EXTENSION_ENTRY(dbsp, loader)` expands to:
```cpp
DUCKDB_EXTENSION_API void dbsp_duckdb_cpp_init(duckdb::ExtensionLoader &loader)
```

Key difference: Takes `ExtensionLoader &` directly instead of `DatabaseInstance &`. DuckDB passes us an already-initialized loader with proper database context.

## Function Registration Flow

### LoadInternal Remains Unchanged

```cpp
void LoadInternal(ExtensionLoader &loader) {
  // All 13 function registrations stay the same
  TableFunction track_fn("dbsp_track", {LogicalType::VARCHAR}, TrackFunc, TrackBind);
  loader.RegisterFunction(track_fn);

  TableFunction create_fn("dbsp_create_view", {}, CreateViewFunc, CreateViewBind);
  create_fn.varargs = LogicalType::VARCHAR;
  loader.RegisterFunction(create_fn);

  // ... remaining functions
}
```

### Functions Being Registered

1. `dbsp_track` - Track table for CDC
2. `dbsp_create_view` - Create materialized view
3. `dbsp_notify_insert` - Manual insert notification
4. `dbsp_notify_delete` - Manual delete notification
5. `dbsp_sync` - Sync table state
6. `dbsp_query` - Query materialized view
7. `dbsp_views` - List all views
8. `dbsp_tables` - List tracked tables
9. `dbsp_drop` - Drop view (scalar function)
10. `dbsp_drop_cascade` - Drop view with cascade (scalar function)
11. `dbsp_save` - Save state to file
12. `dbsp_load` - Load state from file
13. `dbsp_deps` - View dependencies

All functions work without modification.

## Parser Extension - Deferred

### Current State

Parser extension implementation exists but is not registered:
```cpp
// CREATE/DROP/REFRESH MATERIALIZED VIEW DDL parsing
// Implementation in dbsp_parser_extension.hpp
// NOT REGISTERED - needs different API
```

### Why Deferred

- Parser extensions need DatabaseInstance config access
- Neither macro nor Extension base class provides this
- Table function API already provides all functionality
- Can be investigated separately as enhancement

### Current Workaround

Users call via table functions:
```sql
SELECT * FROM dbsp_create_view('my_view', 'SELECT * FROM source');
```

Instead of native DDL:
```sql
CREATE MATERIALIZED VIEW my_view AS SELECT * FROM source;
```

Both achieve the same result. Native DDL is syntactic sugar for future enhancement.

## Testing & Verification

### Build Verification

```bash
# Clean rebuild
rm -rf build && mkdir build && cd build
cmake .. && make dbsp_loadable_extension

# Add metadata
cmake -DEXTENSION=build/dbsp.duckdb_extension \
      -DNULL_FILE=null.txt \
      -DPLATFORM_FILE=platform.txt \
      -DVERSION_FIELD=v1.4.4 \
      -DEXTENSION_VERSION=v0.1.0 \
      -DABI_TYPE=CPP \
      -P duckdb/scripts/append_metadata.cmake
```

Expected: Clean compilation, no errors.

### Load Verification

```bash
echo "LOAD 'build/dbsp.duckdb_extension'; SELECT 'OK';" | duckdb -unsigned
```

Expected: No "Missing DB manager" error, extension loads successfully.

### Function Verification

```sql
-- Create test table
CREATE TABLE test (id INT, value INT);
INSERT INTO test VALUES (1, 10), (2, 20);

-- Track table
SELECT * FROM dbsp_track('test');

-- Create view
SELECT * FROM dbsp_create_view('sum_view', 'SELECT SUM(value) as total FROM test');

-- Query view
SELECT * FROM dbsp_query('sum_view');

-- List views
SELECT * FROM dbsp_views();

-- Expected: All functions work
```

### Unit Test Suite

```bash
cd build_test
ctest --output-on-failure
```

Expected: All 9 unit tests + 7 integration tests pass.

## Implementation Steps

### Task 1: Update Init Function

**Files:** `dbsp_extension.cpp`

1. Replace current `extern "C"` block
2. Use `DUCKDB_CPP_EXTENSION_ENTRY(dbsp, loader)`
3. Call `duckdb::LoadInternal(loader)` directly
4. Remove legacy `dbsp_init()` function
5. Keep `dbsp_version()` function

### Task 2: Clean Up Comments

**Files:** `dbsp_extension.cpp`

1. Remove TODO comments about timing issue
2. Remove comments about "Missing DB manager"
3. Add brief comment explaining macro usage

### Task 3: Rebuild and Test

1. Clean rebuild
2. Add metadata
3. Test extension loading
4. Test function availability
5. Run unit tests

### Task 4: Update Documentation

**Files:** `README.md`, `docs/ARCHITECTURE.md`

1. Document function-based API
2. Note parser extension as future work
3. Update build instructions if needed

## Success Criteria

- ✅ Extension loads without errors
- ✅ All 13 functions registered and callable
- ✅ Basic table tracking works
- ✅ View creation and querying works
- ✅ Test suite passes
- ⏭️ Parser extension deferred to future work

## Future Enhancements

### Parser Extension Registration

Research needed:
- How to access DatabaseInstance config from Extension lifecycle
- Proper timing for parser extension registration
- Alternative registration hooks

Possible approaches:
1. Extension class with additional lifecycle hooks
2. Delayed registration after database fully initialized
3. Different DuckDB API we haven't discovered yet

### Extension Base Class Refactor

If we want better structure later:
1. Create `DBSPExtension : public Extension`
2. Implement `Load()`, `Name()`, `Version()`
3. Keep macro for init entry point (hybrid approach)
4. Benefits: better testability, extensibility

## Notes

- This fix unblocks all current functionality
- Parser extension is additive feature, not blocker
- Can refactor to Extension class later without breaking changes
- Follows DuckDB best practices (core_functions pattern)

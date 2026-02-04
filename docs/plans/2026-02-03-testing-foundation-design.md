# Testing Foundation Design

**Date:** 2026-02-03
**Status:** Approved
**Goal:** Establish comprehensive test coverage for DBSP DuckDB extension

## Problem Statement

Current test coverage is ~10%, covering only the core DBSP library. The DuckDB extension (735 lines), SQL parser (660 lines), and CDC manager (1,078 lines) have zero tests. This creates risks:
- No verification that extension functions work correctly
- Refactoring is dangerous without regression detection
- Edge cases and error paths are untested
- Performance claims (O(delta)) are unvalidated

## Design Goals

1. **Comprehensive coverage**: Target 80%+ test coverage across all components
2. **Integration testing**: Test real SQL workflows end-to-end
3. **Performance validation**: Benchmarks proving O(delta) incremental updates
4. **Developer experience**: Easy to run tests, clear failure messages
5. **Foundation for growth**: Enable confident feature development

## Architecture

### Test Structure

```
test/
├── unit/                          # Unit tests for components
│   ├── test_zset.cpp             # Existing - Z-set operations
│   ├── test_sql_parser.cpp       # NEW - SQL parsing
│   ├── test_cdc_manager.cpp      # NEW - CDC and dependency graph
│   └── test_native_views.cpp     # NEW - View implementations
├── integration/                   # NEW - End-to-end tests
│   ├── test_extension_basic.cpp  # Basic extension functions
│   ├── test_extension_cdc.cpp    # Change detection & sync
│   ├── test_cascading_views.cpp  # View dependencies
│   ├── test_persistence.cpp      # Save/load functionality
│   └── test_sql_coverage.cpp     # SQL feature coverage
├── benchmarks/                    # NEW - Performance tests
│   ├── bench_incremental.cpp     # O(delta) validation
│   ├── bench_aggregates.cpp      # Aggregation performance
│   └── bench_joins.cpp           # Join performance
└── test_helpers.hpp              # NEW - Shared test utilities

CMakeLists.txt                    # Updated build configuration
```

### Test Framework Choice

**Decision: Use Catch2 v3**

Rationale:
- Header-only option available (matches project style)
- Excellent BDD-style syntax (`SECTION`, `GIVEN`, `WHEN`, `THEN`)
- Built-in benchmarking support
- Good DuckDB compatibility
- Modern C++17 support

Alternative considered: Google Test - rejected due to heavier dependencies.

### DuckDB Test Harness

Create helper class for integration tests:

```cpp
class DuckDBTestHarness {
    DuckDB db;
    Connection conn;

public:
    DuckDBTestHarness() : db(nullptr), conn(db) {
        conn.Query("LOAD 'build/dbsp.duckdb_extension'");
    }

    auto Query(const string& sql) { return conn.Query(sql); }

    void CreateTestTable(const string& name, const string& schema);
    void AssertViewEquals(const string& view_name,
                         const vector<vector<Value>>& expected);
    void AssertRowCount(const string& view_name, size_t expected);
};
```

## Component Test Coverage

### 1. Integration Tests (Priority: CRITICAL)

**test_extension_basic.cpp:**
- `dbsp_track()`: Track table, verify schema detection
- `dbsp_create_view()`: Create filter/aggregate/join/distinct views
- `dbsp_query()`: Query views, verify results
- `dbsp_views()`: List views, verify metadata
- `dbsp_tables()`: List tracked tables
- `dbsp_drop()`: Drop view, verify cleanup

**test_extension_cdc.cpp:**
- `dbsp_sync()`: Single table sync, all tables sync
- `dbsp_notify_insert/delete()`: Manual CDC
- Change detection: INSERT, UPDATE, DELETE
- Batch changes: Multiple rows in one sync
- Incremental propagation: Verify O(delta) behavior

**test_cascading_views.cpp:**
- Create view on view
- Three-level cascade
- Dependency resolution order
- Cycle detection (should error)
- Drop cascade

**test_persistence.cpp:**
- `dbsp_save()` to DuckDB table
- `dbsp_save('file.json')` to JSON
- `dbsp_load()` round-trip
- Load into new database instance
- Verify views rebuild correctly

**test_sql_coverage.cpp:**
- All supported SQL constructs
- Error cases: Unsupported features
- Complex predicates (AND, OR, NOT)
- Multiple aggregates
- Self-joins

### 2. Unit Tests (Priority: HIGH)

**test_sql_parser.cpp:**
- Parse SELECT statements
- Extract tables, columns, predicates
- Parse GROUP BY, JOIN
- Detect view type (filter/aggregate/join)
- Error handling: Invalid SQL
- Edge cases: Empty SELECT, missing FROM

**test_cdc_manager.cpp:**
- Table tracking: Add, remove, list
- View registry: Create, drop, query
- Dependency graph: Add dependencies, detect cycles, topological sort
- Change propagation: Single source, cascading
- Thread safety: Concurrent queries (if applicable)

**test_native_views.cpp:**
- NativeFilterView: Apply filter changes
- NativeAggregateView: SUM, COUNT, AVG with deltas
- NativeJoinView: Bilinear join formula
- NativeDistinctView: Incremental distinct
- Edge cases: Empty input, NULL values

### 3. Benchmarks (Priority: MEDIUM)

**bench_incremental.cpp:**
- Measure: INSERT 1 row into 1M row table
- Compare: Traditional view recompute vs DBSP incremental
- Validate: O(delta) claim (should be ~constant time)
- Measure: Batch insert (1K rows)
- Graph: Latency vs table size

**bench_aggregates.cpp:**
- GROUP BY with varying group counts
- Multiple aggregates (SUM, COUNT, AVG)
- High cardinality vs low cardinality groups

**bench_joins.cpp:**
- Join two tables with varying sizes
- Incremental join updates
- Self-joins

## Test Utilities

**test_helpers.hpp:**
```cpp
// Create test data
DuckDBRow makeRow(vector<Value> values);
DuckDBZSet makeZSet(vector<pair<DuckDBRow, Weight>> data);

// Assertions
void ASSERT_ZSET_EQUALS(const DuckDBZSet& actual,
                        const DuckDBZSet& expected);
void ASSERT_VIEW_CONTAINS(const string& view, const DuckDBRow& row);

// Benchmarking
auto measure(function<void()> fn) -> milliseconds;
```

## CMake Integration

Update `CMakeLists.txt`:
```cmake
option(BUILD_TESTS "Build test programs" ON)
option(BUILD_BENCHMARKS "Build benchmarks" OFF)

if(BUILD_TESTS)
    # Fetch Catch2
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.0)
    FetchContent_MakeAvailable(Catch2)

    # Unit tests
    add_executable(unit_tests
        test/unit/test_zset.cpp
        test/unit/test_sql_parser.cpp
        test/unit/test_cdc_manager.cpp
        test/unit/test_native_views.cpp)
    target_link_libraries(unit_tests PRIVATE Catch2::Catch2WithMain)

    # Integration tests
    add_executable(integration_tests
        test/integration/test_extension_basic.cpp
        test/integration/test_extension_cdc.cpp
        test/integration/test_cascading_views.cpp
        test/integration/test_persistence.cpp
        test/integration/test_sql_coverage.cpp)
    target_link_libraries(integration_tests PRIVATE
        Catch2::Catch2WithMain
        duckdb)

    enable_testing()
    add_test(NAME unit_tests COMMAND unit_tests)
    add_test(NAME integration_tests COMMAND integration_tests)
endif()

if(BUILD_BENCHMARKS)
    add_executable(benchmarks
        test/benchmarks/bench_incremental.cpp
        test/benchmarks/bench_aggregates.cpp
        test/benchmarks/bench_joins.cpp)
    target_link_libraries(benchmarks PRIVATE
        Catch2::Catch2WithMain
        duckdb)
endif()
```

## Success Criteria

1. **Coverage**: 80%+ line coverage on all components
2. **All extension functions tested**: Every `dbsp_*` function has tests
3. **Edge cases covered**: NULLs, empty tables, errors, large data
4. **Performance validated**: Benchmarks show O(delta) < O(n) recompute
5. **Easy to run**: `make test` or `ctest` runs all tests
6. **Fast feedback**: Unit tests complete in <1 second, integration tests <10 seconds

## Non-Goals (Out of Scope)

- CI/CD automation (future work)
- Multi-platform testing (future work)
- Fuzzing or property-based testing (future work)
- Performance optimization (benchmarks only measure, not optimize)

## Risks & Mitigation

**Risk**: DuckDB extension testing requires loading .so file
**Mitigation**: Build extension first, load via `LOAD 'path/to/dbsp.duckdb_extension'`

**Risk**: Tests might be slow with large datasets
**Mitigation**: Use small datasets for unit/integration, reserve large data for benchmarks

**Risk**: Catch2 might conflict with DuckDB
**Mitigation**: Header-only mode, careful namespace management

## Implementation Phases

**Phase 1: Infrastructure** (1-2 days)
- Add Catch2 to CMake
- Create DuckDBTestHarness
- Port existing test_zset.cpp to Catch2
- Verify build and test execution

**Phase 2: Integration Tests** (2-3 days)
- Test all extension functions
- CDC and sync tests
- Cascading views tests
- Persistence tests

**Phase 3: Unit Tests** (2-3 days)
- SQL parser tests
- CDC manager tests
- Native view tests

**Phase 4: Benchmarks** (1-2 days)
- Incremental vs recompute comparison
- Aggregate benchmarks
- Join benchmarks

**Total Estimate**: 6-10 days

## Future Enhancements

- GitHub Actions CI (run tests automatically)
- Code coverage reporting (codecov.io)
- Sanitizer builds (ASAN, UBSAN, TSAN)
- Fuzzing with libFuzzer
- Property-based testing with RapidCheck

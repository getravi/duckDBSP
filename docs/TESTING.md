# Testing Guide

Comprehensive guide to testing the DBSP for DuckDB project.

## Overview

The project includes three types of tests:

1. **Unit Tests** (`test/unit/`): Core DBSP library, SQL parser, CDC manager
2. **Integration Tests** (`test/integration/`): Extension functions with real DuckDB
3. **Benchmarks** (`benchmark/`): Performance validation of O(delta) updates

All tests use a simple, dependency-free assertion framework built into the test files.

## Quick Start

```bash
# Build and run all tests
cd build
cmake -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON ..
make

./unit_tests
./integration_tests
./benchmarks
```

Expected output:
```
[PASS] All 45 unit tests passed
[PASS] All 30 integration tests passed
Benchmark: Single insert O(1): 0.05ms ✓
```

## Test Organization

```
test/
├── unit/                    # Core library tests (no DuckDB)
├── integration/             # Extension tests (requires DuckDB)
└── benchmarks/              # Performance tests
```

### Test Categories

| Category | Location | Dependencies | Purpose |
|----------|----------|--------------|---------|
| Unit | `test/unit/` | None | Test DBSP algorithms |
| Integration | `test/integration/` | DuckDB | Test extension API |
| Benchmarks | `benchmark/` | DuckDB | Validate O(delta) |

## Unit Tests

Tests core DBSP library without DuckDB dependency.

### Running

```bash
cd build
cmake -DBUILD_TESTS=ON ..
make
./unit_tests
```

### Test Coverage

**Z-Set Operations** (10 tests)
- Insert/delete elements
- Weight tracking
- Negation
- Addition

**Stream Operators** (15 tests)
- Filter (map/filter)
- Aggregate (sum/count/avg)
- Join (bilinear formula)
- Distinct

**SQL Parser** (12 tests)
- SELECT parsing
- WHERE conditions
- GROUP BY
- JOIN syntax
- Cascading view dependencies

**CDC Manager** (8 tests)
- Track/untrack tables
- Insert/delete notifications
- Sync detection
- Cascading updates

### Example Test

```cpp
void test_zset_insert() {
    dbsp::ZSet<std::string, int> z;
    z.insert("apple", 1);
    z.insert("banana", 1);
    assert(z.get("apple") == 1);
    assert(z.get("banana") == 1);
}
```

## Integration Tests

Tests extension functions with real DuckDB connection.

### Running

```bash
# Build extension first
cd duckdb_extension
./build.sh

# Build and run integration tests
cd ../build
cmake -DBUILD_TESTS=ON ..
make
./integration_tests
```

### Test Coverage

**Table Tracking** (5 tests)
- `dbsp_track()` - Track table
- `dbsp_sync()` - Sync changes
- `dbsp_tables()` - List tracked tables
- Auto-sync detection
- Multi-table tracking

**View Management** (8 tests)
- `dbsp_create_view()` - SQL syntax
- `dbsp_query()` - Query results
- `dbsp_views()` - List views
- `dbsp_drop()` - Drop view
- `dbsp_drop_cascade()` - Cascade drop
- `dbsp_deps()` - Dependencies

**Incremental Updates** (7 tests)
- Filter incrementality
- Aggregate incrementality
- Join incrementality
- Cascading updates
- O(delta) validation

**Persistence** (5 tests)
- `dbsp_save()` / `dbsp_load()` - DuckDB table
- `dbsp_save(file)` / `dbsp_load(file)` - JSON file
- View restoration
- Schema validation

**CDC Operations** (5 tests)
- Auto-detect inserts
- Auto-detect deletes
- Manual `dbsp_notify_insert()`
- Manual `dbsp_notify_delete()`
- Sync batching

### Example Test

```cpp
void test_incremental_aggregate() {
    db->Query("CREATE TABLE orders (id INT, amount INT)");
    db->Query("SELECT * FROM dbsp_track('orders')");
    db->Query("SELECT * FROM dbsp_create_view('totals',
              'SELECT SUM(amount) FROM orders')");

    db->Query("INSERT INTO orders VALUES (1, 100)");
    db->Query("SELECT * FROM dbsp_sync('orders')");

    auto result = db->Query("SELECT * FROM dbsp_query('totals')");
    assert(result->GetValue(0, 0).GetValue<int>() == 100);
}
```

## Benchmarks

Validates O(delta) performance characteristics.

### Running

```bash
cd build
cmake -DBUILD_BENCHMARKS=ON ..
make benchmarks
./benchmarks
```

### Benchmark Scenarios

**1. Single Insert (O(1))**
```
Setup: 1M rows, aggregate view
Test: Insert 1 row
Expected: < 1ms (independent of table size)
```

**2. Batch Insert (O(k))**
```
Setup: 1M rows, aggregate view
Test: Insert 1000 rows
Expected: Linear in batch size, not table size
```

**3. Cascading Views**
```
Setup: 3-level view hierarchy
Test: Single insert propagation
Expected: < 5ms for all levels
```

**4. Join Updates**
```
Setup: Join of two 100K tables
Test: Insert into one table
Expected: O(matching rows), not O(n²)
```

### Output Format

```
Benchmark: Single insert O(1)
  Baseline (1M rows): 5432ms
  Incremental (1 row): 0.05ms
  Speedup: 108640x ✓

Benchmark: Batch insert O(k)
  Incremental (1000 rows): 52ms
  Expected: O(k) = 50ms
  Actual: 1.04x ✓
```

## Writing Tests

### Unit Test Template

```cpp
// test/unit_tests.cpp

void test_my_feature() {
    // Setup
    dbsp::ZSet<int, int> z;

    // Execute
    z.insert(1, 1);
    z.insert(2, 1);

    // Assert
    assert(z.get(1) == 1);
    assert(z.get(2) == 1);
}

// Add to main():
int main() {
    RUN_TEST(test_my_feature);
    // ...
}
```

### Integration Test Template

```cpp
// test/integration_tests.cpp

void test_my_extension_function() {
    auto db = make_unique<DuckDB>(nullptr);
    auto conn = make_unique<Connection>(*db);

    // Load extension
    conn->Query("LOAD '/path/to/dbsp.duckdb_extension'");

    // Setup
    conn->Query("CREATE TABLE test (id INT)");
    conn->Query("SELECT * FROM dbsp_track('test')");

    // Execute
    conn->Query("INSERT INTO test VALUES (1)");
    conn->Query("SELECT * FROM dbsp_sync('test')");

    // Assert
    auto result = conn->Query("SELECT * FROM dbsp_tables()");
    assert(result->RowCount() == 1);
}

// Add to main():
int main() {
    RUN_TEST(test_my_extension_function);
    // ...
}
```

### Benchmark Template

```cpp
// test/benchmarks.cpp

void benchmark_my_operation() {
    auto db = make_unique<DuckDB>(nullptr);
    auto conn = make_unique<Connection>(*db);
    conn->Query("LOAD '/path/to/dbsp.duckdb_extension'");

    // Setup large dataset
    conn->Query("CREATE TABLE big (id INT, val INT)");
    for (int i = 0; i < 1000000; i++) {
        // Insert data
    }

    // Measure incremental operation
    auto start = high_resolution_clock::now();
    conn->Query("INSERT INTO big VALUES (1000001, 100)");
    conn->Query("SELECT * FROM dbsp_sync('big')");
    auto end = high_resolution_clock::now();

    auto duration = duration_cast<microseconds>(end - start).count();
    cout << "Incremental insert: " << duration << "μs" << endl;

    // Validate O(1) or O(delta)
    assert(duration < 1000); // Should be < 1ms
}
```

## Continuous Testing

### Development Workflow

```bash
# Watch mode (requires entr or similar)
ls test/*.cpp include/*.hpp | entr -c bash -c 'cd build && make && ./unit_tests'

# Or manual loop
while true; do
    sleep 2
    cd build && make && ./unit_tests
done
```

### Pre-commit Hook

```bash
#!/bin/bash
# .git/hooks/pre-commit

echo "Running tests..."
cd build
make
./unit_tests || exit 1
./integration_tests || exit 1
echo "All tests passed!"
```

## Test Coverage Analysis

### Manual Coverage

Count test assertions:
```bash
grep -r "assert(" test/ | wc -l
# Should be 100+ assertions
```

### Feature Coverage Checklist

- [ ] Z-Set: insert, delete, negate, add
- [ ] Operators: filter, aggregate, join, distinct
- [ ] SQL Parser: SELECT, WHERE, GROUP BY, JOIN
- [ ] CDC: track, sync, notify
- [ ] Extension: all 15+ SQL functions
- [ ] Persistence: save/load to file and table
- [ ] Cascading: multi-level views
- [ ] Performance: O(delta) benchmarks

## Common Issues

### Issue: "Extension not found"

**Solution**: Build extension first
```bash
cd duckdb_extension
./build.sh
cd ../build
# Update path in test file to absolute path
```

### Issue: Tests compile but segfault

**Solution**: Check CMake configuration
```bash
cd build
cmake -DBUILD_TESTS=ON ..
make clean
make
```

### Issue: Benchmarks show O(n) not O(delta)

**Solution**: Verify incremental operators
```bash
# Check that views are using incremental updates
grep "Incremental" src/dbsp_materialized_view.hpp
```

### Issue: Integration tests fail to load extension

**Solution**: Use absolute path
```cpp
// Instead of:
conn->Query("LOAD 'dbsp.duckdb_extension'");

// Use:
conn->Query("LOAD '/absolute/path/to/dbsp.duckdb_extension'");
```

## Adding New Tests

### 1. Add to Appropriate File

- Core DBSP logic → `test/unit_tests.cpp`
- Extension function → `test/integration_tests.cpp`
- Performance → `test/benchmarks.cpp`

### 2. Follow Naming Convention

```cpp
void test_<component>_<feature>() {
    // Example: test_zset_insert()
    // Example: test_sql_parser_join()
    // Example: test_cdc_sync_detection()
}
```

### 3. Use Clear Assertions

```cpp
// Good
assert(result == expected);

// Better
assert(result->RowCount() == 1);
assert(result->GetValue(0, 0).GetValue<int>() == 100);
```

### 4. Add to Test Runner

```cpp
int main() {
    // ...
    RUN_TEST(test_my_new_feature);
    // ...
}
```

### 5. Document in This File

Add to appropriate test coverage section above.

## CI/CD (Future)

Planned GitHub Actions workflow:

```yaml
name: Tests
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Build
        run: |
          mkdir build && cd build
          cmake -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=ON ..
          make
      - name: Unit Tests
        run: cd build && ./unit_tests
      - name: Integration Tests
        run: cd build && ./integration_tests
      - name: Benchmarks
        run: cd build && ./benchmarks
```

## Summary

- **Unit tests**: Test core DBSP algorithms (no dependencies)
- **Integration tests**: Test extension with DuckDB
- **Benchmarks**: Validate O(delta) performance
- **Simple framework**: No external test dependencies
- **Quick feedback**: All tests run in < 10 seconds

For questions or issues, see [CONTRIBUTING.md](../CONTRIBUTING.md).

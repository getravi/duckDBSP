# DBSP for DuckDB

Real-time incrementally maintained materialized views for DuckDB, based on [Database Stream Processing (DBSP)](https://www.feldera.com/blog/what-is-dbsp) theory.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![DuckDB](https://img.shields.io/badge/DuckDB-v1.5.4-blue.svg)](https://duckdb.org/)

## Overview

Traditional materialized views recompute entirely when underlying data changes. DBSP-powered views update **incrementally** in O(delta) time - only processing the changes, not the entire dataset.

```
Traditional:  INSERT 1 row → Recompute 1M rows → O(n)
DBSP:         INSERT 1 row → Update affected aggregates → O(delta)
```

### Key Features

- **Incremental Updates**: Views update in O(delta) time, not O(n)
- **SQL Syntax**: Define views using familiar SQL
- **Cascading Views**: Views can reference other views
- **Automatic CDC**: Change Data Capture with sync detection
- **Persistence**: Save/restore views across sessions
- **Zero Dependencies**: Pure C++ header-only core library
- **Bounded Memory**: optional disk-backed state (`dbsp_spill`)
- **Parallel Updates**: optional multi-core sync, propagation, and join
  probing (`dbsp_parallel`)

## Quick Start

### Basic Example

```sql
-- Load the extension
LOAD 'dbsp';

-- Create a table
CREATE TABLE orders (id INT, customer VARCHAR, amount DECIMAL);

-- Create an incrementally maintained view. That's it — the source
-- table is tracked automatically and the view keeps itself current.
CREATE MATERIALIZED VIEW customer_totals AS
SELECT customer, SUM(amount) as total
FROM orders
GROUP BY customer;

-- Insert data — the view updates on commit, no sync call needed
INSERT INTO orders VALUES (1, 'Alice', 100), (2, 'Bob', 200), (3, 'Alice', 150);

-- Query the view (instant — no recomputation)
SELECT * FROM dbsp_query('customer_totals');
-- Returns: Alice: 250, Bob: 200

INSERT INTO orders VALUES (4, 'Alice', 50);
SELECT * FROM dbsp_query('customer_totals');
-- Returns: Alice: 300, Bob: 200
```

**Bulk loading?** Turn the automatic refresh off while you load, then
sync once:

```sql
SELECT * FROM dbsp_auto_sync(false);
-- ... millions of inserts ...
SELECT * FROM dbsp_sync();          -- one scan-and-diff
SELECT * FROM dbsp_auto_sync(true);
```

**Alternative Syntax** (table functions):
```sql
-- Create view using table function API
SELECT * FROM dbsp_create_view('customer_totals',
    'SELECT customer, SUM(amount) FROM orders GROUP BY customer');
    
-- Query using table function
SELECT * FROM dbsp_query('customer_totals');
```

### Advanced Examples

**Filtering aggregates with HAVING:**
```sql
CREATE MATERIALIZED VIEW high_value_customers AS
SELECT customer, SUM(amount) as total, COUNT(*) as order_count
FROM orders
GROUP BY customer
HAVING SUM(amount) > 200;
```

**Recursive queries for graph traversal:**
```sql
CREATE TABLE edges (src INT, dst INT);
SELECT * FROM dbsp_track('edges');

CREATE MATERIALIZED VIEW reachable AS
WITH RECURSIVE reach AS (
    SELECT src, dst FROM edges
    UNION
    SELECT e.src, r.dst FROM edges e JOIN reach r ON e.dst = r.src
)
SELECT * FROM reach;
```

See [examples/](examples/) for more comprehensive demos.

## Installation

### Building from Source

```bash
./build.sh
```

This will:
1. Download DuckDB source (if not present)
2. Build the DBSP extension
3. Output `dbsp.duckdb_extension`

### Loading the Extension

```sql
LOAD '/path/to/dbsp.duckdb_extension';
```

## Testing

### Running Tests

```bash
# Build and run the full suite (unit + integration)
cd test/build_test
cmake .. && make -j8
ctest

# Benchmarks (built but not part of ctest)
make bench_planner_eval soak_differential
./bench_planner_eval
SOAK_ROUNDS=60 ./soak_differential "[soak]"
```

### Test Coverage

- **Unit tests**: Core DBSP library, native views, CDC manager
- **Integration tests**: All extension functions, CDC, cascading views, persistence
- **Benchmarks**: O(delta) performance validation

See [docs/TESTING.md](docs/TESTING.md) for details.

## Documentation

- [API Reference](docs/API.md) - Complete function reference
- [Theory](docs/THEORY.md) - DBSP mathematical foundations
- [Examples](examples/) - Usage examples
- [Architecture](docs/ARCHITECTURE.md) - Internal design

## SQL Functions

### Table Tracking

| Function | Description |
|----------|-------------|
| `dbsp_track(table)` | Pre-track a table (optional — view creation auto-tracks its sources) |
| `dbsp_sync(table)` | Manually sync one table (needed only with auto-sync off) |
| `dbsp_sync()` | Manually sync all tracked tables |
| `dbsp_tables()` | List all tracked tables |

### View Management

| Function | Description |
|----------|-------------|
| `dbsp_create_view(name, sql)` | Create view with SQL syntax |
| `dbsp_query(view)` | Query a materialized view |
| `dbsp_views()` | List all views with stats |
| `dbsp_drop(view)` | Drop a view |
| `dbsp_drop_cascade(view)` | Drop view and dependents |
| `dbsp_deps(view)` | Show view dependencies |

### Persistence

| Function | Description |
|----------|-------------|
| `dbsp_save('views.json')` | Save view definitions to a JSON file |
| `dbsp_load('views.json', 'json')` | Load view definitions from a JSON file |

Persistence is JSON-file only (definitions, not materialized state); the
zero-argument DuckDB-table forms are not supported. File paths must be
relative to the working directory — absolute paths are rejected.

### Manual CDC

| Function | Description |
|----------|-------------|
| `dbsp_notify_insert(table, ...)` | Notify of row insertion |
| `dbsp_notify_delete(table, ...)` | Notify of row deletion |

### Automatic CDC & Diagnostics

| Function | Description |
|----------|-------------|
| `dbsp_auto_sync(bool)` | Toggle automatic sync on commit (default ON; turn off for bulk loads) |
| `dbsp_parallel(bool)` | Toggle parallel multi-table sync + same-level view propagation |
| `dbsp_spill(bool)` | Toggle disk-backed state: baselines, join indexes, top-K windows, big aggregate groups |
| `dbsp_use_planner([bool])` | No-op since Phase C (planner is the only frontend); kept for script compatibility |

## Error Handling

duckDBSP uses a structured error code system (DBSP-Exxx) with helpful error messages:

- **Clear descriptions** of what went wrong
- **SQL highlighting** showing exactly where the error occurred
- **Workarounds** for unsupported features
- **Documentation links** for detailed guidance

See [Error Handling Guide](docs/ERROR_HANDLING.md) for details.

## Supported SQL Features

### ✅ Currently Supported

**DDL Syntax:**
- `CREATE MATERIALIZED VIEW name AS SELECT ...`
- `DROP MATERIALIZED VIEW name [CASCADE]`
- `REFRESH MATERIALIZED VIEW name` (no-op with auto-refresh)

**Query Operations:**
- `SELECT * FROM table` / `SELECT columns FROM table`
- `SELECT ... WHERE condition` with complex predicates
- `SELECT ... GROUP BY column`
- `SELECT ... HAVING condition` - filter aggregated results
- `SELECT DISTINCT ...` - incremental deduplication
- `SELECT ... FROM t1 JOIN t2 ON ...` - bilinear incremental joins
  - Multi-column equality joins
  - Complex JOIN predicates (non-equi conditions)
- `WITH RECURSIVE ...` - transitive closures and recursive queries

**Aggregate Functions:**
- `SUM`, `COUNT`, `AVG`, `MIN`, `MAX` - all with O(log n) incremental updates
- `DISTINCT` and `FILTER (WHERE ...)` modifiers, incrementally maintained
- `ROLLUP` / `CUBE` / `GROUPING SETS` with `GROUPING()` - one incremental
  aggregate branch per grouping set
- `STRING_AGG` / `ARRAY_AGG` with in-aggregate `ORDER BY` (sorted
  per-group state, re-rendered on change)
- `MEDIAN`, `QUANTILE_CONT`, `QUANTILE_DISC`, `MODE`, `MAD` (holistic,
  over the sorted per-group multiset; mode ties break by smallest value)
- Window functions over expressions (auto-projected below the window)

**Circuit Optimization:**
- Automatic filter pushdown through JOINs
- Projection pruning to minimize data movement
- Operator fusion for reduced overhead
- Shared join arrangements: N views joining the same table share one
  index (one update per delta instead of N)

**Advanced Features:**
- Cascading views (views on views with dependency tracking)
- NULL-aware operations (SQL semantics for GROUP BY, JOINs, aggregates)
- Incremental recursive query evaluation

**Planner Frontend (the only frontend since Phase C — the bespoke SQL
parser was deleted):**
- View SQL planned by DuckDB's own binder/planner; scan/filter/projection,
  GROUP BY aggregation (incl. exact SUM over DECIMAL), inner and outer
  joins (LEFT/RIGHT/FULL; equi + residual predicates), cross joins,
  IN/NOT IN and scalar subqueries (correlated included), EXISTS,
  DISTINCT, DISTINCT ON, set
  operations, window functions, non-recursive CTEs, WITH RECURSIVE
  (multi-table recursive steps), and ORDER BY/LIMIT/OFFSET translate
  directly to circuit nodes with full DuckDB expression coverage (function
  calls, mixed AND/OR predicates, multi-aggregate GROUP BY, expression
  group/join keys, HAVING, global aggregates). A circuit-IR optimizer
  combines filters, pushes them below joins, and fuses filter+project into
  one node. Unsupported plans (unordered string_agg, USING KEY
  recursion, ...) fail with a DBSP-E110 error naming the operator.

### 📋 Not yet supported

- WITH RECURSIVE ... USING KEY
- Non-constant (expression) LIMIT — percentage LIMIT works
- Window ORDER BY / PARTITION BY over expressions (project first)
- string_agg / array_agg without ORDER BY inside the aggregate (ordered
  forms are supported)


## How It Works

DBSP (Database Stream Processing) treats database operations as streams of changes:

1. **Z-Sets**: Data represented as `element → weight` mappings
   - Weight +1 = insertion
   - Weight -1 = deletion
   - Weight 0 = no change

2. **Incremental Operators**: Each SQL operator has an incremental version
   - Filter^Δ: Only process changed rows matching predicate
   - Join^Δ: `Δa × b + a × Δb` (bilinear formula)
   - Aggregate^Δ: Update running totals with deltas

3. **Change Propagation**: Changes flow through the view graph
   ```
   orders (Δ) → filter_view (Δ) → aggregate_view (Δ)
   ```

For the mathematical foundations, see [Theory](docs/THEORY.md).

## Performance Benchmarks

| Metric | Result |
|--------|--------|
| **Incremental filter/projection** | ~970,000 rows/s |
| **Incremental aggregation** | ~2,200,000 rows/s |
| **Incremental join (100k delta vs 100k index)** | ~460,000 rows/s |
| **Delta propagation, 3-level view chain** | ~13 µs/row |
| **Captured-delta commit (explicit INSERT txn)** | ~0.3 ms |
| **Full scan-and-diff sync (50k rows, 3 views)** | ~41 ms |

*Apple M-series, release build (`test/build_test`), 100k-row deltas unless
noted; reproduce with `bench_planner_eval`. Explicit INSERT-only
transactions commit O(Δ) via captured deltas; other writes pay the
scan-and-diff sync.*

## Project Structure

duckDBSP/
├── include/                     # Header-only implementation
│   ├── dbsp_zset.hpp            # Z-set data structure
│   ├── dbsp_stream.hpp          # Stream operators
│   ├── dbsp_circuit.hpp         # Dataflow graph
│   ├── dbsp_plan_translator.hpp # Planner frontend (circuit translation)
│   ├── dbsp_cdc.hpp             # CDC manager + shared arrangements
│   ├── dbsp_duckdb_types.hpp    # Native DuckDB type integration
│   └── dbsp_context_state.hpp   # Transaction hooks (auto-CDC)
├── src/                         # Extension source
│   ├── dbsp_extension.cpp       # Extension entry point
│   └── dbsp_recovery.cpp        # Replay-based crash recovery
├── build.sh                     # Build script
├── test/                        # Unit/integration tests, benchmarks
├── docs/                        # Documentation
└── examples/                    # Usage examples


## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

### Development Setup

```bash
# Clone the repository
git clone https://github.com/yourusername/duckDBSP.git
cd duckDBSP

# Build the core library tests
mkdir build && cd build
cmake ..
make

# Run tests
./dbsp_tests
```

## References

- [DBSP: Automatic Incremental View Maintenance](https://www.vldb.org/pvldb/vol16/p1601-budiu.pdf) - VLDB 2023
- [Feldera: Continuous Analytics](https://www.feldera.com/)
- [Database Stream Processing Theory (Lean Formalization)](https://github.com/tchajed/database-stream-processing-theory)

## License

MIT License - see [LICENSE](LICENSE) for details.

## Acknowledgments

- The DBSP theory was developed by Mihai Budiu, Tej Chajed, Frank McSherry, Leonid Ryzhyk, and Val Tannen
- DuckDB team for the excellent embeddable database

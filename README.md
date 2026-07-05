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

## Quick Start

### Basic Example

```sql
-- Load the extension
LOAD 'dbsp';

-- Create a table and track it for CDC
CREATE TABLE orders (id INT, customer VARCHAR, amount DECIMAL);
SELECT * FROM dbsp_track('orders');

-- Create an incrementally maintained view (modern DDL syntax)
CREATE MATERIALIZED VIEW customer_totals AS
SELECT customer, SUM(amount) as total
FROM orders
GROUP BY customer;

-- Insert data (views update automatically)
INSERT INTO orders VALUES (1, 'Alice', 100), (2, 'Bob', 200), (3, 'Alice', 150);
SELECT * FROM dbsp_sync('orders');

-- Query the view (instant - no recomputation)
SELECT * FROM customer_totals;
-- Returns: Alice: 250, Bob: 200

-- Insert more data
INSERT INTO orders VALUES (4, 'Alice', 50);
SELECT * FROM dbsp_sync('orders');

-- View is already updated!
SELECT * FROM customer_totals;
-- Returns: Alice: 300, Bob: 200
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
# Build tests
cd build
cmake -DBUILD_TESTS=ON ..
make

# Run unit tests
./unit_tests

# Run integration tests (requires extension to be built)
./build.sh && cd build
./integration_tests

# Run benchmarks
cmake -DBUILD_BENCHMARKS=ON ..
make benchmarks
./benchmarks
```

### Test Coverage

- **Unit tests**: Core DBSP library, SQL parser, CDC manager
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
| `dbsp_track(table)` | Track a table for change detection |
| `dbsp_sync(table)` | Sync tracked table with DuckDB |
| `dbsp_sync()` | Sync all tracked tables |
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
| `dbsp_save()` | Save views to DuckDB table |
| `dbsp_save(file)` | Save views to JSON file |
| `dbsp_load()` | Load views from DuckDB table |
| `dbsp_load(file)` | Load views from JSON file |

### Manual CDC

| Function | Description |
|----------|-------------|
| `dbsp_notify_insert(table, ...)` | Notify of row insertion |
| `dbsp_notify_delete(table, ...)` | Notify of row deletion |

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

**Circuit Optimization:**
- Automatic filter pushdown through JOINs
- Projection pruning to minimize data movement
- Operator fusion for reduced overhead

**Advanced Features:**
- Cascading views (views on views with dependency tracking)
- NULL-aware operations (SQL semantics for GROUP BY, JOINs, aggregates)
- Incremental recursive query evaluation

**Planner Frontend (Phase B — default ON, `dbsp_use_planner(false)` to
disable):**
- View SQL planned by DuckDB's own binder/planner instead of the bespoke
  parser; scan/filter/projection, GROUP BY aggregation, inner joins
  (equi + residual predicates), cross joins, DISTINCT, set operations,
  window functions, and non-recursive CTEs translate directly to circuit
  nodes with full DuckDB expression coverage (function calls, mixed AND/OR
  predicates, multi-aggregate GROUP BY, expression group/join keys, HAVING,
  global aggregates). Unsupported plans (ORDER BY/LIMIT, recursive CTEs,
  outer joins) fall back to the parser transparently.

### 📋 Planned (Phase 4+)

- `ORDER BY` / `LIMIT` (anti-pattern - degrades to O(n), deferred)
- Window functions (`ROW_NUMBER`, `RANK`, `LAG`, `LEAD`)
- Set operations: `UNION`, `INTERSECT`, `EXCEPT`
- Automatic CDC via transaction hooks
- Subqueries and CTEs (non-recursive)

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

| Operation | Throughput (approx) | Latency (10k batch) |
|-----------|---------------------|---------------------|
| **Raw Ingestion** | ~10,000 rows/s | 1.0s |
| **Incremental Projection** | **~200,000 rows/s** | **0.05s** |
| **Incremental Aggregation** | **~210,000 rows/s** | **0.05s** |

*Benchmarks run on Apple M1, simple schema, batch size 10,000 rows.*
*Incremental maintenance is ~20x faster than raw ingestion for these scenarios.*

## Project Structure

duckDBSP/
├── include/                   # Core DBSP library (header-only)
│   ├── dbsp_zset.hpp          # Z-set data structure
│   ├── dbsp_stream.hpp        # Stream operators
│   ├── dbsp_circuit.hpp       # Dataflow graph
│   └── dbsp_materialized_view.hpp
├── src/                       # Extension source
│   └── dbsp_extension.cpp     # Extension entry point
│   ├── dbsp_duckdb_types.hpp  # Native DuckDB type integration
│   ├── dbsp_sql_parser.hpp    # SQL parsing
│   ├── dbsp_plan_translator.hpp # Planner frontend (Phase B)
│   ├── dbsp_cdc.hpp           # CDC manager
├── build.sh                   # Build script
├── test/                      # Unit tests
├── docs/                      # Documentation
└── examples/                  # Usage examples


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

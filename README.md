# DBSP for DuckDB

Real-time incrementally maintained materialized views for DuckDB, based on [Database Stream Processing (DBSP)](https://www.feldera.com/blog/what-is-dbsp) theory.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![DuckDB](https://img.shields.io/badge/DuckDB-v2.0.0_LTS-blue.svg)](https://duckdb.org/)

Built for **DuckDB 2.0**. It is pinned to the
[v2.0.0 LTS release](https://duckdb.org/2026/10/21/announcing-duckdb-200),
runs on a stock, unpatched engine, and loads into the matching
`duckdb==2.0.0` Python wheel.

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
- **Exact change capture**: generated statement triggers hand each
  commit's row changes to the views, so a commit is applied as a delta with
  no table scan. Scan-and-diff is the safety net.
- **Delta read-back**: `dbsp_changes('view')` returns the last sync's
  output delta with signed weights
- **Attached catalogs**: views can source tables in `ATTACH`ed databases
  (`... FROM m.orders`); tables are keyed by canonical
  `catalog.schema.table`
- **Persistence**: view definitions plus circuit-state checkpoints.
  Reopening a database restores views without replaying their sources.
- **Auto-persist**: views survive a clean connection reopen with no
  explicit save/load calls (`dbsp_autopersist`, on by default)
- **Native DDL**: `CREATE [OR REPLACE]`, `REFRESH` and
  `DROP MATERIALIZED VIEW`, with the SQL stored exactly as typed
- **Zero Dependencies**: Pure C++ header-only core library
- **Bounded Memory**: optional disk-backed state (`dbsp_spill`)
- **Parallel Updates**: optional multi-core sync, propagation, and join
  probing (`dbsp_parallel`)

## Quick Start

### Basic Example

```sql
-- Load the extension (unsigned: see "Loading the Extension")
LOAD '/path/to/dbsp.duckdb_extension';

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

**Requires DuckDB 2.0.** The extension is built against the DuckDB
[v2.0.0 LTS release](https://duckdb.org/2026/10/21/announcing-duckdb-200)
(commit `d54cbbe3`), and loads into the matching `duckdb==2.0.0` Python wheel.
Databases need storage version `v2.0.0` or higher, because change capture
uses triggers (see "Storage version" below).

### Building from Source

```bash
./build.sh
```

This will:
1. Download DuckDB source (if not present), pinned by COMMIT to v2.0.0
2. Build the DBSP extension (uses `ccache` and Ninja automatically when
   installed; parallelism capped at `-j 8`)
3. Output `dbsp.duckdb_extension`

Test binaries are not built by default; `DBSP_BUILD_TESTS=ON ./build.sh` adds
them under `build/test` (see `docs/TESTING.md`).

**No engine patch.** The engine tree in `duckdb/` is stock, and `build.sh`
fails loudly if it is not: change capture comes from generated statement
triggers, which are ordinary SQL objects a stock DuckDB already supports. The
extension therefore loads into the public PyPI wheel of the same engine commit,
and CI can build against one.

### SQL DDL

`CREATE [OR REPLACE] MATERIALIZED VIEW`, `DROP MATERIALIZED VIEW [IF EXISTS]
name [CASCADE]` and `REFRESH MATERIALIZED VIEW` are recognised by
`ParserExtension::parser_override`, which sees the RAW query text before
DuckDB's PEG grammar. The SQL a view stores is therefore byte-exact with what
was typed (comments included), and `DROP MATERIALIZED VIEW` works — the 2.0
grammar claims that statement and throws `Cannot drop MATERIALIZED VIEW yet`,
so a hook running only on parse failures could never reach it.

**Loading the extension raises `allow_parser_override_extension` to
`FALLBACK`, but only from `DEFAULT`** — DuckDB's default, which skips every
override callback. An explicit `FALLBACK` or `STRICT` is left alone. The
setting is global, so other parser-override extensions in that database become
active too. Setting it back to `DEFAULT` keeps the DDL working through the
older token-reconstruction hook — same view, normalised stored SQL, no DROP.

### The delta source

The extension learns what a committing transaction wrote from statement-level
`AFTER` triggers it generates on every tracked table. `dbsp_track(t)` creates
three of them plus a small `dbsp_trigger_sink` table in `t`'s own catalog; the
bodies hand the exact old/new row images to the extension, which buffers them
per transaction and applies them at commit. There is no mode switch and no
second mechanism. Full design: `docs/DESIGN_TRIGGER_SOURCE.md`.

The scan-and-diff reconcile (`dbsp_sync`) remains the safety net: any commit
whose picture is or might be incomplete is reconciled by scanning, so
correctness never depends on a trigger having fired.

#### What tracking a table costs it

These are engine behaviours, measured on `v2.0.0-alpha39998`, re-verified on
the `v2.0.0` LTS release, and pinned by `test/unit/test_trigger_source.cpp`.
They are permanent constraints on every tracked table.

| On a tracked (triggered) table | Engine response |
|---|---|
| `MERGE INTO <t> ...` | `Not implemented Error: MERGE INTO is not supported on tables with triggers` |
| `INSERT ... ON CONFLICT DO UPDATE` | `Not implemented Error: ON CONFLICT DO UPDATE is not yet supported with REFERENCING NEW TABLE AS triggers` |
| `INSERT OR REPLACE` (same path) | same error |
| `ALTER TABLE ... DROP COLUMN` / `RENAME COLUMN` / `ALTER COLUMN ... TYPE` / `RENAME TO` | `Dependency Error: Cannot alter entry "t" because there are entries that depend on it.` |
| `ALTER TABLE ... ADD COLUMN` | allowed — the sweep notices and regenerates the bodies |
| `INSERT ... ON CONFLICT DO NOTHING` | allowed |
| `DROP TABLE` | allowed; takes the triggers with it, and a recreate + re-track reinstalls them |

**Storage version.** `CREATE TRIGGER` requires a database at storage version
`v2.0.0` or higher. A file written by DuckDB 1.5.4 is `v1.0.0+`, and tracking a
table in it throws
`Binder Error: CREATE TRIGGER is only supported for storage versions v2.0.0 and higher`.
Files created by the 2.0 wheel are `v2.0.0+` and need nothing. To migrate an
older one:

```sql
ATTACH 'old.duckdb' AS src (READ_ONLY);
ATTACH 'new.duckdb' AS dst (STORAGE_VERSION 'v2.0.0');
COPY FROM DATABASE src TO dst;
```

then move the `old.duckdb.dbsp_spill/` sidecar directory alongside the new file
(the paths are derived from the database path). Verified: the restored views
match plain SQL, the triggers install on the next statement, and edits are
served by exact deltas.

The triggers and the sink are user-visible catalog objects: they appear in
`duckdb_triggers()` / `duckdb_tables()`, are WAL-logged, and travel in
`EXPORT DATABASE`.

Verify the source is live — a database whose triggers never fire is otherwise
indistinguishable from one with nothing to report:

```sql
SELECT * FROM dbsp_stats();
-- trigger_syncs          5   trigger-body ingests served
-- trigger_rows          12   row images buffered
-- exact_delta_syncs      4   table deltas applied exactly (no scan)
-- scan_syncs             2   scan-and-diff reconciles
-- provisional_tables     0   baselines awaiting a concurrency watermark
-- reconcile_failures     0   reconcile scans that did NOT run
-- last_reconcile_error  NULL text of the last one, in the `detail` column
```

`dbsp_stats()` has three columns — `metric`, `value` (BIGINT) and `detail`
(VARCHAR, NULL on every numeric row).

With `DBSP_TIMING=1` the trigger path prints `[dbsp-timing] trigger_ingest`.

`provisional_tables` is 0 in a single-writer session. It counts tables seeded
while ANOTHER connection had a transaction open: that transaction may already
have written the table before it was tracked — invisible to the seeding scan
and reported by no trigger — so the table takes no exact deltas and is
reconciled by scan until every transaction alive at seed time has ended.

### Python probe scripts

`test/python/*.py` run the loadable extension on a real Python client and are
**not** part of `ctest`. Each takes the extension path and prints `PASS`:

```bash
uv run --isolated --with 'duckdb==2.0.0' --with pyarrow \
  python test/python/test_ddl_syntax.py build/dbsp.duckdb_extension
```

See `docs/TESTING.md`.

### Loading the Extension

The extension is not signed, so DuckDB has to be told to accept unsigned
extensions.

DuckDB CLI:

```bash
duckdb -unsigned my.duckdb -cmd "LOAD '/path/to/dbsp.duckdb_extension'"
```

Python (`duckdb==2.0.0`):

```python
import duckdb
con = duckdb.connect("my.duckdb", config={"allow_unsigned_extensions": "true"})
con.execute("LOAD '/path/to/dbsp.duckdb_extension'")
```

Close the connection before the process exits, so the auto-save and teardown
run cleanly.

## Testing

### Running Tests

```bash
# Build and run the full suite (unit + integration, 48 targets)
cd test/build_test
cmake .. && cmake --build . -j 8
ctest

# Same suite under DuckDB's vector verification — catches chunks handed to
# the engine with stale child-vector sizes.
DBSP_TEST_VERIFY_VECTORS=1 ctest

# Same suite under the internal-connection law: a helper that opens its own
# connection for a data read or DDL while the user's transaction is open
# throws, naming the site. Off by default (see docs/TESTING.md).
DBSP_STRICT_INTERNAL_QUERY=1 ctest

# Benchmarks (built but not part of ctest)
make bench_planner_eval soak_differential
./bench_planner_eval
SOAK_ROUNDS=60 ./soak_differential "[soak]"
```

### Test Coverage

- **Unit tests**: Core DBSP library, native views, CDC manager, the
  trigger delta source
- **Integration tests**: All extension functions, CDC, cascading views,
  persistence and checkpoints. Most check every view against the same query
  run as plain SQL (differential testing).
- **Engine canaries**: DuckDB behaviours the extension relies on, pinned by
  name so an engine upgrade fails them first
- **Python probes**: the loadable extension on the real `duckdb==2.0.0` wheel
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
| `CREATE [OR REPLACE] MATERIALIZED VIEW` | DDL form of create/replace |
| `DROP MATERIALIZED VIEW [IF EXISTS] v [CASCADE]` | DDL form of drop |
| `dbsp_create_view(name, sql)` | Create view with SQL syntax |
| `dbsp_replace_view(name, sql)` | Redefine a view, rebuilding only it and its dependents |
| `dbsp_query(view)` | Query a materialized view |
| `dbsp_changes(view)` | The view's last delta: its columns plus a signed `weight` |
| `dbsp_delta_generations()` | Commit generation of each view's last delta (for change feeds) |
| `dbsp_views()` | List all views with stats |
| `dbsp_drop(view)` | Drop a view (alias `dbsp_drop_view`) |
| `dbsp_drop_cascade(view)` | Drop view and dependents (alias `dbsp_drop_view_cascade`) |
| `dbsp_deps(view)` | Show view dependencies |
| `dbsp_set_view_key(view, 'c1,c2')` | Declare a unique, non-null row key so deltas apply by key |

### Persistence

| Function | Description |
|----------|-------------|
| `dbsp_save()` | Save view definitions to `_dbsp_views` plus a circuit-state checkpoint (in the database file) |
| `dbsp_load()` | Load views, restoring their state from the checkpoint where it is still valid |
| `dbsp_save(catalog := 'm')` / `dbsp_load(catalog := 'm')` | The same, in an `ATTACH`ed database |
| `dbsp_save('views.json')` | Save view definitions only to a JSON file |
| `dbsp_load('views.json', 'json')` | Load view definitions from a JSON file |

Table-form persistence lives in the database file, so file copies and
backups carry the views. Beside the definitions, `dbsp_save()` writes each
supported view's operator state and result into the `_dbsp_ckpt` tables,
together with a watermark of every source table:
- A view whose sources are unchanged since the save restores from the
  checkpoint without replay. By default it is decoded lazily, on first use.
- A view whose source closure changed is rebuilt from the current table
  data.
- So is a view whose definition changed, or one with no checkpoint support.

The JSON form carries definitions only. JSON file paths must be relative to
the working directory; absolute paths are rejected.

`dbsp_save()`/`dbsp_load()` are no longer something a caller has to
remember: with auto-persist on (the default), a clean connection close
saves automatically and the next session's first DBSP call loads
automatically. See `dbsp_autopersist` below.

### Manual CDC

| Function | Description |
|----------|-------------|
| `dbsp_notify_insert(table, ...)` | Notify of row insertion |
| `dbsp_notify_delete(table, ...)` | Notify of row deletion |

### Automatic CDC & Diagnostics

| Function | Description |
|----------|-------------|
| `dbsp_auto_sync(bool)` | Toggle automatic sync on commit (default ON; turn off for bulk loads) |
| `dbsp_autopersist(bool)` | Toggle auto-save-on-close + auto-load-on-reopen (default ON; turn off for bulk loads) |
| `dbsp_autopersist_interval(n)` | Piggyback a circuit-state checkpoint every `n` commits (default 0 = off) |
| `dbsp_lazy_restore(bool)` | Toggle lazy per-view checkpoint restore: each view decodes on first need instead of eagerly at load (default ON) |
| `dbsp_parallel(bool)` | Toggle parallel multi-table sync + same-level view propagation |
| `dbsp_spill(bool)` | Toggle disk-backed state: baselines, join indexes, top-K windows, big aggregate groups |
| `dbsp_mv_tables(bool)` | Mirror each view's result into an ordinary `__mv_<view>` table (disk-backed results) |
| `dbsp_realize(view)` | Decode a lazily restored view now instead of on first use |
| `dbsp_stats()` | Sync-path counters: trigger ingests, exact deltas, scan reconciles |
| `dbsp_view_state()` / `dbsp_table_state()` | Resident state per view / per tracked table |
| `dbsp_wait_teardown()` | Wait for close-time teardown threads before process exit |
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
- `CREATE OR REPLACE MATERIALIZED VIEW name AS SELECT ...` - redefine a
  view, rebuilding only it and its transitive dependents
- `REFRESH MATERIALIZED VIEW name` (no-op with auto-refresh)
- `DROP MATERIALIZED VIEW [IF EXISTS] name [CASCADE]`. DuckDB 2.0's own
  grammar claims this statement and refuses it, so the extension recognises
  it first, in `parser_override` (see "SQL DDL" above). The function forms
  `dbsp_drop_view('name')` and `dbsp_drop_view_cascade('name')` still work.
  They return a status string (`'Dropped'`, or the reason) rather than
  raising, so callers must read it.

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

These fail at view creation with a DBSP-E110 error:
- WITH RECURSIVE ... USING KEY
- Non-constant (expression) LIMIT — constant and percentage LIMIT work
- string_agg / array_agg without ORDER BY inside the aggregate (ordered
  forms are supported)
- `approx_quantile` / `reservoir_quantile` / `approx_top_k`
- DISTINCT on holistic aggregates (median / quantile / mode)
- `NTH_VALUE` (its N must index within the window frame first)

On a tracked table, the engine itself refuses `MERGE INTO`,
`ON CONFLICT DO UPDATE` / `INSERT OR REPLACE`, and every `ALTER TABLE` except
`ADD COLUMN`. See "What tracking a table costs it". The full list of
limitations is in [TODO.md](TODO.md).


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
| **Full scan-and-diff sync (50k rows, 3 views)** | ~41 ms |

*Apple M-series, release build, 100k-row deltas unless noted; reproduce with
`bench_planner_eval`. The per-commit figures that used to sit here were
measured under the predictive capture stack, which no longer exists — they are
not reproducible and have been removed rather than re-labelled. A commit whose
trigger bodies fired is served by an exact delta and pays no table scan; every
other commit pays the scan-and-diff above.*

## Project Structure

```
duckDBSP/
├── include/                           # Header-only implementation
│   ├── dbsp_zset.hpp                  # Z-set data structure
│   ├── dbsp_stream.hpp                # Stream operators
│   ├── dbsp_circuit.hpp               # Dataflow graph
│   ├── dbsp_plan_translator.hpp       # Planner frontend (circuit translation)
│   ├── dbsp_cdc.hpp                   # CDC manager + shared arrangements
│   ├── dbsp_trigger_source.hpp        # Trigger-fed exact delta source
│   ├── dbsp_checkpoint.hpp            # Checkpoint blob codecs
│   ├── dbsp_checkpoint_store.hpp      # Checkpoint table I/O (_dbsp_ckpt*)
│   ├── dbsp_spill_store.hpp           # Disk-backed state (dbsp_spill)
│   ├── dbsp_internal_connection.hpp   # Scoped/retained internal connections
│   ├── dbsp_duckdb_types.hpp          # Native DuckDB type integration
│   └── dbsp_context_state.hpp         # Transaction hooks (auto-sync)
├── src/                               # Extension source
│   ├── dbsp_extension.cpp             # Extension entry point
│   └── dbsp_recovery.cpp              # Replay-based crash recovery
├── duckdb/                            # DuckDB v2.0.0 source (pinned by build.sh)
├── build.sh                           # Build script
├── test/                              # Unit/integration tests, benchmarks
├── docs/                              # Documentation
└── examples/                          # Usage examples
```


## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

### Development Setup

```bash
# Clone the repository
git clone https://github.com/getravi/duckDBSP.git
cd duckDBSP

# Build the extension (fetches DuckDB v2.0.0 on first run)
./build.sh

# Build and run the test suite
mkdir -p test/build_test && cd test/build_test
cmake .. && cmake --build . -j 8
ctest
```

Run `ctest` only from `test/build_test`. Every change to code needs a green
full suite before it is committed.

## References

- [DBSP: Automatic Incremental View Maintenance](https://www.vldb.org/pvldb/vol16/p1601-budiu.pdf) - VLDB 2023
- [Feldera: Continuous Analytics](https://www.feldera.com/)
- [Database Stream Processing Theory (Lean Formalization)](https://github.com/tchajed/database-stream-processing-theory)

## License

MIT License - see [LICENSE](LICENSE) for details.

## Acknowledgments

- The DBSP theory was developed by Mihai Budiu, Tej Chajed, Frank McSherry, Leonid Ryzhyk, and Val Tannen
- DuckDB team for the excellent embeddable database

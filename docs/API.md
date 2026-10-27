# DBSP for DuckDB - API Reference

Complete reference for all DBSP extension functions.

## Table of Contents

- [SQL DDL Syntax](#sql-ddl-syntax)
- [Table Tracking](#table-tracking)
- [View Management](#view-management)
- [Querying](#querying)
- [Persistence](#persistence)
- [Manual CDC](#manual-cdc)
- [Runtime Modes](#runtime-modes)

---

## SQL DDL Syntax

The primary interface. Sources are tracked automatically and views keep
themselves current (auto-sync is on by default).

### CREATE MATERIALIZED VIEW

```sql
CREATE MATERIALIZED VIEW name AS SELECT ...;
```

Creates an incrementally maintained view. Equivalent to
`dbsp_create_view('name', 'SELECT ...')`. Internally routed through the
`dbsp_create_materialized_view` table function (registered for the
parser extension; not intended for direct use).

### CREATE OR REPLACE MATERIALIZED VIEW

```sql
CREATE OR REPLACE MATERIALIZED VIEW name AS SELECT ...;
```

Plain `CREATE` when `name` doesn't exist yet; when it does, changes its
definition and rebuilds **only** `name` and its transitive dependents —
equivalent to `dbsp_replace_view('name', 'SELECT ...')`. See
`dbsp_replace_view` below for the rebuild semantics.

### REFRESH MATERIALIZED VIEW

```sql
REFRESH MATERIALIZED VIEW name;
```

Accepted for compatibility — a no-op, since views refresh automatically.

### DROP MATERIALIZED VIEW

```sql
DROP MATERIALIZED VIEW [IF EXISTS] name [CASCADE | RESTRICT];
```

Drops the view. `IF EXISTS` turns a missing view from an error into a message;
`CASCADE` drops the view together with every view that depends on it (without
it, a view with dependents is refused and the error names them).

This works because the extension recognises the statement in
`ParserExtension::parser_override`, which runs BEFORE the core PEG grammar. The
grammar CLAIMS `DROP MATERIALIZED VIEW` and its transformer then throws
`NotImplementedException: Cannot drop MATERIALIZED VIEW yet`, so a hook that
only sees statements the grammar failed on could never have it. See "How the
DDL is parsed" below.

The function forms still work and are unchanged:

```sql
SELECT dbsp_drop_view('name');      -- or dbsp_drop_view_cascade('name')
SELECT dbsp_drop('name');           -- or dbsp_drop_cascade('name')
```

### How the DDL is parsed

`CREATE`/`DROP`/`REFRESH MATERIALIZED VIEW` are recognised by
`ParserExtension::parser_override` (`include/dbsp_parser_extension.hpp`), which
receives the RAW query text before the PEG grammar sees it and rewrites the
statement into a call on the extension's own functions. Two consequences:

- **The stored SQL is byte-exact.** `dbsp_views()` returns the substring you
  typed after `AS`, comments and spacing included.
- **`DROP MATERIALIZED VIEW` is reachable**, as above.

`parser_override` callbacks are SKIPPED unless
`allow_parser_override_extension` is `FALLBACK` or `STRICT`, and DuckDB's
default is `DEFAULT`. **Loading the extension raises that setting to
`FALLBACK`, and only from `DEFAULT`** — an explicit `FALLBACK` or `STRICT` is
read and left alone. The change is for the whole database, so any other
parser-override extension becomes active too. Setting it back does not break the DDL: the older
token-reconstruction hook (which runs on PEG failures) still parses
`CREATE`/`REFRESH`, and builds the same view. What is lost there is the exact
text — `dbsp_views()` reports a normalised `t . "col" AS m , ...` with comments
stripped — and `DROP MATERIALIZED VIEW` goes back to the core parser's refusal.

---

## Table Tracking

> **Usually unnecessary**: creating a view auto-tracks its source
> tables and loads their current state, and auto-sync (on by default)
> keeps views current on every commit. These functions serve manual
> workflows: pre-tracking, bulk loads with auto-sync off, and the
> notify API.

### dbsp_track(table_name)

Track a DuckDB table for automatic change detection.

```sql
SELECT * FROM dbsp_track('orders');
```

**Parameters:**
- `table_name` (VARCHAR): Name of the table to track

**Returns:**
- `result` (VARCHAR): Confirmation message with column count

**Example:**
```sql
CREATE TABLE orders (id INT, customer VARCHAR, amount DECIMAL);
SELECT * FROM dbsp_track('orders');
-- Returns: "Tracking table: orders (3 columns)"
```

**Notes:**
- Automatically detects table schema
- Loads current table contents into tracking state
- Required before creating views on the table

---

### dbsp_sync(table_name)

Synchronize a tracked table with its current DuckDB state. Detects and propagates any changes made outside of DBSP notifications.

```sql
SELECT * FROM dbsp_sync('orders');
```

**Parameters:**
- `table_name` (VARCHAR, optional): Table to sync. If omitted, syncs all tracked tables.

**Returns:**
- `result` (VARCHAR): Confirmation message

**Examples:**
```sql
-- Sync specific table
SELECT * FROM dbsp_sync('orders');

-- Sync all tracked tables
SELECT * FROM dbsp_sync();
```

**Notes:**
- Use after INSERT/UPDATE/DELETE statements
- Computes delta between tracked state and actual table
- Propagates changes to all dependent views

---

### dbsp_tables()

List all tables currently being tracked for CDC.

```sql
SELECT * FROM dbsp_tables();
```

**Returns:**
| Column | Type | Description |
|--------|------|-------------|
| `table_name` | VARCHAR | Name of tracked table |
| `columns` | BIGINT | Number of columns |

**Example:**
```sql
SELECT * FROM dbsp_tables();
-- table_name | columns
-- orders     | 3
-- products   | 5
```

---

## View Management

### dbsp_create_view(name, sql)

Create an incrementally maintained materialized view using SQL syntax.

```sql
SELECT * FROM dbsp_create_view('view_name', 'SELECT ... FROM ...');
```

**Parameters:**
- `name` (VARCHAR): Unique name for the view
- `sql` (VARCHAR): SQL SELECT statement defining the view

**Returns:**
- `result` (VARCHAR): Confirmation with source tables

**Supported SQL:**
```sql
-- Filter
SELECT * FROM dbsp_create_view('high_value',
    'SELECT * FROM orders WHERE amount > 100');

-- Projection
SELECT * FROM dbsp_create_view('order_amounts',
    'SELECT id, amount FROM orders');

-- Aggregation
SELECT * FROM dbsp_create_view('totals',
    'SELECT customer, SUM(amount) FROM orders GROUP BY customer');

-- Distinct
SELECT * FROM dbsp_create_view('unique_customers',
    'SELECT DISTINCT customer FROM orders');

-- Join
SELECT * FROM dbsp_create_view('order_details',
    'SELECT * FROM orders JOIN products ON orders.product_id = products.id');

-- Cascading (view on view)
SELECT * FROM dbsp_create_view('vip_totals',
    'SELECT * FROM totals WHERE SUM > 1000');
```

**Notes:**
- Source tables are automatically tracked if not already
- Views can reference other views (cascading)
- Circular dependencies are detected and rejected

---

### dbsp_replace_view(name, sql)

Change an existing materialized view's definition in place, rebuilding
**only** that view and its transitive dependents — everything upstream
(its sources, and any unrelated view) is left untouched, and every
dependent keeps its own original DDL.

```sql
SELECT * FROM dbsp_replace_view('view_name', 'SELECT ... FROM ...');
```

Equivalent to `CREATE OR REPLACE MATERIALIZED VIEW view_name AS SELECT ...`
(the DDL form routes here automatically when `view_name` already exists).

**Parameters:**
- `name` (VARCHAR): Name of an existing view
- `sql` (VARCHAR): New SQL SELECT statement for the view

**Returns:**
- `result` (VARCHAR): Confirmation with source tables

**Example:**
```sql
SELECT * FROM dbsp_create_view('lvl1', 'SELECT k, v * 2 AS d FROM base');
SELECT * FROM dbsp_create_view('lvl2', 'SELECT k, d + 1 AS e FROM lvl1');

-- Redefine lvl1; lvl2's own DDL is untouched, only its values change
SELECT * FROM dbsp_replace_view('lvl1', 'SELECT k, v * 3 AS d FROM base');
```

**Notes:**
- Fails with `DBSP-E206` if `name` doesn't exist — use `dbsp_create_view`
  for a new view instead.
- No transactional rollback (v1): if a dependent fails to recreate mid
  rebuild, the remaining dependents are still attempted with their saved
  DDL, and the `DBSP-E304` error reports every failure plus which views
  are still queryable afterward.
- Cost is proportional to the changed view's subtree — sources and
  sibling views outside it never repopulate.

---

### dbsp_use_planner(enable)

**Deprecated no-op since Phase C5.** The planner frontend is the only
frontend — the bespoke SQL parser it used to toggle against was deleted.
The function stays callable so existing scripts don't break; it always
reports ENABLED. `dbsp_create_view` translates view SQL through DuckDB's
own binder/planner, covering scan/filter/projection (arbitrary
expressions), GROUP BY aggregation (multiple aggregates, expression keys,
HAVING, global aggregates, exact DECIMAL SUM, DISTINCT and FILTER
modifiers, ROLLUP/CUBE/GROUPING SETS with GROUPING(), ordered
STRING_AGG/ARRAY_AGG, holistic MEDIAN/QUANTILE_CONT/QUANTILE_DISC/
MODE/MAD), inner and outer joins (LEFT/RIGHT/FULL, equi + residual
predicates), cross joins, IN/NOT IN/EXISTS/scalar subqueries
(correlated included), DISTINCT, DISTINCT ON, UNION/INTERSECT/EXCEPT
(ALL and DISTINCT), window functions (expressions auto-projected;
`ROWS`/`RANGE`/`GROUPS` frames including constant bounded frames like
`ROWS BETWEEN 11 PRECEDING AND CURRENT ROW`, `LAG`/`LEAD` with a constant
non-default offset like `LAG(v, 12)`, and `NTILE` with a constant bucket
count like `NTILE(4)`), non-recursive CTEs, WITH RECURSIVE (deletions
included), and ORDER BY/LIMIT/OFFSET (constant or percentage). The few
remaining gaps (USING KEY recursion, expression LIMIT, approximate
statistics like approx_quantile, unordered string_agg, NTH_VALUE's N —
even as a literal constant, unlike NTILE's bucket count — see TODO.md)
fail with a DBSP-E110 error naming the construct.

```sql
SELECT * FROM dbsp_use_planner();       -- Always: ENABLED
```

**Parameters:**
- `enable` (BOOLEAN, optional): Enable or disable. Omit to query status.

**Returns:**
- `result` (VARCHAR): Confirmation or current status

---

### dbsp_create_view(name, table, type, spec)

Alternative syntax for simple views without SQL parsing.

```sql
SELECT * FROM dbsp_create_view('name', 'table', 'type', 'spec');
```

**Parameters:**
- `name` (VARCHAR): View name
- `table` (VARCHAR): Source table
- `type` (VARCHAR): View type (`filter`, `aggregate`, `distinct`)
- `spec` (VARCHAR): Type-specific specification

**Type Specifications:**

| Type | Spec Format | Example |
|------|-------------|---------|
| `filter` | `column op value` | `amount > 100` |
| `aggregate` | `group_col AGG value_col` | `customer SUM amount` |
| `distinct` | (empty string) | `` |

**Examples:**
```sql
-- Filter view
SELECT * FROM dbsp_create_view('expensive', 'orders', 'filter', 'amount > 100');

-- Aggregate view
SELECT * FROM dbsp_create_view('totals', 'orders', 'aggregate', 'customer SUM amount');

-- Distinct view
SELECT * FROM dbsp_create_view('unique', 'orders', 'distinct', '');
```

---

### dbsp_views()

List all materialized views with statistics.

```sql
SELECT * FROM dbsp_views();
```

**Returns:**
| Column | Type | Description |
|--------|------|-------------|
| `view_name` | VARCHAR | Name of the view |
| `sql` | VARCHAR | SQL definition |
| `rows` | BIGINT | Current row count |
| `version` | BIGINT | Update version number |

For a view still pending lazy restore (see `dbsp_lazy_restore` below),
`rows` is read straight from the checkpoint's stashed sink blob (its
length-prefix, no decode) rather than the view's live state — exact,
since "pending" means no delta has touched it, but this call never
triggers the view's actual restore. `dbsp_views()` enumerates every view,
so realizing here to compute a row count would eagerly decode everything
on the first call, defeating the point of lazy restore.

---

### dbsp_drop(view_name)

Alias: `dbsp_drop_view(view_name)` (identical scalar function).

Drop a materialized view.

```sql
SELECT dbsp_drop('view_name');
```

**Parameters:**
- `view_name` (VARCHAR): Name of view to drop

**Returns:**
- VARCHAR: "Dropped" or error message

**Notes:**
- Fails if other views depend on this view
- Use `dbsp_drop_cascade` to force drop with dependents

---

### dbsp_drop_cascade(view_name)

Alias: `dbsp_drop_view_cascade(view_name)`.

Drop a view and all views that depend on it.

```sql
SELECT dbsp_drop_cascade('view_name');
```

**Parameters:**
- `view_name` (VARCHAR): Name of view to drop

**Returns:**
- VARCHAR: Confirmation with count of dropped views

**Example:**
```sql
SELECT dbsp_drop_cascade('totals');
-- Returns: "Dropped totals (and 2 dependent views)"
```

---

### dbsp_deps(view_name)

Show dependencies for a view.

```sql
SELECT * FROM dbsp_deps('view_name');
```

**Returns:**
| Column | Type | Description |
|--------|------|-------------|
| `name` | VARCHAR | Related view/table name |
| `relationship` | VARCHAR | `depends_on` or `depended_by` |

**Example:**
```sql
SELECT * FROM dbsp_deps('vip_totals');
-- name    | relationship
-- totals  | depends_on
-- orders  | depends_on
```

---

## Querying

### dbsp_query(view_name)

Query a materialized view. Returns current contents instantly without recomputation.

```sql
SELECT * FROM dbsp_query('view_name');
```

**Parameters:**
- `view_name` (VARCHAR): Name of view to query

**Returns:**
- Table with columns matching the view's schema

**Example:**
```sql
SELECT * FROM dbsp_query('customer_totals');
-- customer | SUM
-- Alice    | 300
-- Bob      | 200
```

**Notes:**
- Results reflect all synced changes
- Query is O(result_size), not O(source_size)
- Column names derived from SQL or auto-generated (col0, col1, ...)

**Refuses on an unseeded baseline.** Creating a view inside an open
transaction defers the seeding scan of its sources (that scan runs on an
internal connection and cannot see the transaction's own rows), so between
`BEGIN; dbsp_create_view(...)` and the end of that transaction the view stands
on a baseline nothing has scanned. `dbsp_query` and `dbsp_changes` throw there
rather than serve the empty answer:

```
Invalid Input Error: dbsp_query('mv'): source table 'db.main.t' has an
UNSEEDED baseline — its seeding scan was deferred because a transaction was
open when the view was created, and nothing has scanned the table since.
Reading now would return the unseeded (empty) answer, not the table's content.
End that transaction (COMMIT or ROLLBACK), or run dbsp_sync(), and read again.
```

The refusal fires on every connection of the instance, not only the one
holding the transaction: the debt is per-connection but the baseline is
per-instance. It clears when the deferring transaction ends (its COMMIT widens
itself to a scan-and-diff; its ROLLBACK asks for a rebuild from committed
storage) or when someone runs `dbsp_sync()`. `dbsp_view_state()` is not
gated: it takes no view argument, so there is no view for the gate to ask
about. Its numbers are diagnostics anyway, and a diagnostic that refuses while
the state is broken is useless exactly when it is needed.

---

### dbsp_changes(view_name)

Rows added or removed by the view's most recent sync, as the view's columns
plus a signed BIGINT `weight` (+n insert, -n delete).

```sql
SELECT * FROM dbsp_changes('customer_totals');
```

**Parameters:**
- `view_name` (VARCHAR): Name of the view

**Returns:**
- The view's columns, plus `weight` (BIGINT)

**Semantics:**
- Single-generation buffer: holds the delta of the most recent sync step
  that touched the view, and is overwritten by the next one — consume
  between syncs.
- While a group survives an aggregate update, the step contains an
  `(old row, -1)`, `(new row, +1)` pair, so one read yields both old and
  new values.
- One `dbsp_notify_insert`/`dbsp_notify_delete` call is one step: a
  delete+insert notify pair leaves only the insert step's delta. Read
  between the two notifies if both halves are needed.
- Empty result when the view exists but no sync has touched it; error when
  the view does not exist.
- Empty right after CREATE MATERIALIZED VIEW, even over a populated table:
  the initial-population delta is dropped at create (bounded-RAM Phase 1a)
  — only post-create commits populate the buffer.
- Reading does NOT drain the buffer: an untouched view keeps serving the
  same delta (its create-time initial replay, at worst) until the next sync
  that touches it. Use `dbsp_delta_generations()` to tell fresh buffers
  from stale ones.

---

### dbsp_view_state()

Per-view resident circuit-state bytes, approximated by class.

```sql
SELECT * FROM dbsp_view_state();
```

**Returns:**
- `view_name` (VARCHAR) — plus one synthetic row `__shared_arrangements`
  for manager-owned shared join arrangements
- `result_bytes`, `arrangement_bytes`, `window_bytes`, `recursion_bytes`,
  `other_bytes`, `total_bytes` (BIGINT)

**Semantics:**
- Estimates calibrated for trend, not exact heap bytes; H6 payload-shared
  rows are counted once per scan (attribution goes to the first view
  scanned; totals are what reconcile with the process footprint).
- On macOS compare against `footprint(1)`/phys_footprint, not `ps rss` —
  the compressor hides most cold circuit state from RSS (measured: a DAG
  reporting 2.5GB RSS had a 23GB physical footprint).
- `other_bytes` includes per-node output buffers and delta buffers.
- Tracked-table baselines are NOT included — see `dbsp_table_state()`;
  a RAM budget must sum both functions.

---

### dbsp_table_state()

Per-tracked-table baseline residency. Boxed baselines (~300 B/row) are the
dominant big-model RAM class and are invisible to `dbsp_view_state()`.

```sql
SELECT * FROM dbsp_table_state();
```

**Returns:**
- `table_name` (VARCHAR)
- `mode` (VARCHAR) — `boxed` (full rows in a RAM Z-set), `spilled`
  (disk record log + ~72 B/row digest index), or `deferred` (lazy-restore
  fast path: nothing materialized until first use)

Baselines auto-spill above `DBSP_SPILL_THRESHOLD_ROWS` (default 2,000,000
rows; `0` disables) — including mid-scan during the initial baseline
build, so big tables never pay the boxed peak. Small tables stay boxed
for speed; `dbsp_spill(true)` still force-spills everything.
- `distinct_rows` (BIGINT) — deferred tables report the restore-time
  COUNT(*) upper bound
- `resident_bytes` (BIGINT) — baseline plus pending-changes buffer,
  calibrated estimate

---

### dbsp_delta_generations()

Per-view provenance for the `dbsp_changes` buffer: the commit generation at
which each view's delta buffer was last rewritten.

```sql
SELECT * FROM dbsp_delta_generations();
```

**Returns:**
- `view_name` (VARCHAR)
- `generation` (BIGINT): commit sequence at the buffer's last rewrite
  (create-time initial replay or a propagation step); 0 when the buffer was
  never written (e.g. a restored, still-pending view).

**Semantics:**
- Generations only advance when a sync rewrites the view's buffer — reads
  (`dbsp_changes`, `dbsp_query`) never move them.
- Change-feed pattern: snapshot `MAX(generation)` as a baseline after
  setup; after each commit batch, harvest `dbsp_changes` only from views
  whose generation moved past the baseline, then advance the baseline.
  Without the filter, a consumer re-reads stale buffers of untouched views
  on every batch.

---

## Persistence

### dbsp_save()

Save all view definitions to the `_dbsp_views` table in the default
catalog — inside the database file, so copies and backups carry the
views — plus a circuit-state checkpoint: each supported view's operator
state and result go into the `_dbsp_ckpt` tables, with a watermark of
every source table. The report string says how many views made it into
the checkpoint (`(+ circuit checkpoint: N views)`), or why none did.
`dbsp_save(catalog := 'm')` does the same in an `ATTACH`ed database.
`dbsp_save('view_name', 'table_name')` saves one view's definition to a
named table (identifier-validated) and writes no checkpoint.

```sql
SELECT * FROM dbsp_save();
```

---

### dbsp_save(filepath)

Save all view definitions to a JSON file.

```sql
SELECT * FROM dbsp_save('views.json');
```

**Parameters:**
- `filepath` (VARCHAR): Path to output JSON file, relative to the working
  directory (absolute paths and path traversal are rejected)

**Returns:**
- VARCHAR: Confirmation message

**JSON Format:**
```json
{
  "tracked_tables": ["orders", "products"],
  "views": [
    {
      "name": "totals",
      "sql": "SELECT customer, SUM(amount) FROM orders GROUP BY customer",
      "sources": ["orders"],
      "created_at": 1699900000000
    }
  ]
}
```

---

### dbsp_load()

Load view definitions from the `_dbsp_views` table (see `dbsp_save()`).
Missing `_dbsp_views` is not an error — there is simply nothing to load.
`dbsp_load('table_name')` loads from a named table (identifier-validated;
a missing named table IS an error); `dbsp_load(catalog := 'm')` loads
from an `ATTACH`ed database.

A view restores from the checkpoint written by the last `dbsp_save()`,
without replaying its sources, when the checkpoint is still valid for it.
Any other view is rebuilt from the current table data:
- its source closure changed since the save (a stale watermark on any
  table it reads, directly or through the views it is stacked on);
- its definition differs from the one the checkpoint recorded;
- it has no checkpoint support, or no checkpoint exists (including one
  written under an older checkpoint format).

The report string counts both, e.g. `Loaded views from _dbsp_views
(3 views, 2 from checkpoint, ...)`. With `dbsp_lazy_restore` on (the
default) a restored view is decoded on first use.

```sql
SELECT * FROM dbsp_load();
```

---

### dbsp_load(filepath, 'json')

Load view definitions from a JSON file. The explicit `'json'` format
argument is required — a bare one-argument form is interpreted as a
DuckDB table name, not a file.

```sql
SELECT * FROM dbsp_load('views.json', 'json');
```

**Parameters:**
- `filepath` (VARCHAR): Path to JSON file, relative to the working
  directory (absolute paths and path traversal are rejected)
- `format` (VARCHAR): Must be `'json'`

**Notes:**
- Recreates views in creation order (handles dependencies)
- Re-syncs with current table data
- Continues loading if individual views fail

**Returns:**
- VARCHAR: Confirmation with view count

---

## Manual CDC

For cases where `dbsp_sync()` is not suitable, you can manually notify of changes.

### dbsp_notify_insert(table, values...)

Notify DBSP of a row insertion.

```sql
SELECT * FROM dbsp_notify_insert('orders', 1, 'Alice', 100.00);
```

**Parameters:**
- `table` (VARCHAR): Table name
- `values...` (ANY): Column values in order

**Returns:**
- VARCHAR: Confirmation message

---

### dbsp_notify_delete(table, values...)

Notify DBSP of a row deletion.

```sql
SELECT * FROM dbsp_notify_delete('orders', 1, 'Alice', 100.00);
```

**Parameters:**
- `table` (VARCHAR): Table name
- `values...` (ANY): Column values identifying the row

**Returns:**
- VARCHAR: Confirmation message

---

## Runtime Modes

### dbsp_auto_sync(enable)

Toggle automatic change capture — **ON by default**: views update on
every transaction commit without calling `dbsp_sync`.

Every write to a tracked table commits in **O(delta)**: `dbsp_track` puts
statement-level `AFTER` triggers on the table, and their bodies hand the exact
old and new row images to the extension as the statement runs. That covers
INSERT (`VALUES`, `SELECT`, `COPY FROM`, the C++ `Appender`), UPDATE, DELETE
and TRUNCATE, whatever the predicate — including shapes nothing could predict:
`UPDATE ... FROM`, volatile expressions, prepared parameters, subqueries that
read this transaction's own uncommitted writes, indexed-column UPDATEs, and
repeated writes to one table in a transaction.

Scan-and-diff, scoped to the tables the transaction touched, remains the
fallback for anything the triggers could not account for: a write matching zero
rows (the bodies evaluate nothing, so the commit cannot tell that from silence),
a transaction under which the triggers were installed or regenerated, an
unparseable or multi-statement string whose targets could not be resolved, and
a delta that failed to convert. Correctness never depends on a trigger having
fired.

Tracking a table also **costs** it: on a table carrying triggers the engine
refuses `MERGE INTO`, `INSERT ... ON CONFLICT DO UPDATE`, `INSERT OR REPLACE`,
and every `ALTER TABLE` form except `ADD COLUMN`; the database must be at
storage version `v2.0.0` or higher. The full list, with the engine's own error
messages, is in `docs/DESIGN_TRIGGER_SOURCE.md`.

Turn auto-sync off for bulk loads (each autocommit INSERT pays a scoped
scan) and run one `dbsp_sync()` afterwards.

```sql
SELECT * FROM dbsp_auto_sync(true);   -- Enable
SELECT * FROM dbsp_auto_sync(false);  -- Disable
SELECT * FROM dbsp_auto_sync();       -- Query status
```

### dbsp_autopersist(enable)

Toggle auto-persist — **ON by default**: a database that used DBSP views
reopens with them alive, no explicit `dbsp_save()`/`dbsp_load()` calls.

- **Auto-save**: on a clean connection close (the last user connection to
  an instance), if autopersist is on and any view exists, the extension
  saves view definitions (`dbsp_save()`) and a circuit-state checkpoint
  (`save_checkpoint()`) automatically.
- **Auto-load**: the first DBSP entry point that has a `ClientContext`
  (`dbsp_query`, `dbsp_views`, `dbsp_track`, `CREATE MATERIALIZED VIEW`,
  `dbsp_sync`, `dbsp_changes`) loads persisted state — checkpoint fast path
  included — if the view registry in this session is still empty. Fires at
  most once per session and never after a view has already been created
  here; a missing `_dbsp_views` table is not an error (there is simply
  nothing to load).

The two-call `dbsp_save()`/`dbsp_load()` contract still works exactly as
before and stays useful for named/JSON-file forms and manual snapshots;
auto-persist just means the default zero-arg round trip is no longer
something a caller has to remember.

Turn auto-persist off for bulk-load workflows the same way as auto-sync —
`dbsp_autopersist(false)` before, `(true)` after — to skip the save/load
overhead entirely.

```sql
SELECT * FROM dbsp_autopersist(true);   -- Enable (default)
SELECT * FROM dbsp_autopersist(false);  -- Disable
SELECT * FROM dbsp_autopersist();       -- Query status
```

### dbsp_autopersist_interval(n)

Piggyback a circuit-state checkpoint every `n` commits (counted across
`dbsp_sync`-driven propagation), so a crash between clean shutdowns loses
at most `n` commits of operator state. **Default 0 (off)** — the
underlying data is never at risk either way (base tables are
DuckDB-durable, and a stale checkpoint fails its watermark check and
falls back to a full rebuild on load).

```sql
SELECT * FROM dbsp_autopersist_interval(100);  -- checkpoint every 100 commits
SELECT * FROM dbsp_autopersist_interval(0);    -- off
SELECT * FROM dbsp_autopersist_interval();     -- query the current interval
```

### dbsp_lazy_restore(enable)

Toggle lazy per-view checkpoint restore (D-lazy) — **ON by default**: a
`dbsp_load()` circuit-state checkpoint fast path cold-creates every
covered view but decodes each one's node/sink blobs on first need instead
of eagerly, so reopen returns without paying every view's blob-decode
cost. The first `dbsp_query()`, incoming delta, or other read of a
still-pending view's live state decodes it transparently — the query or
delta still returns/applies the exact same result, just with the decode
cost deferred to when it's actually needed. A checkpointed view never
touched between reopen and the next `dbsp_save()` is re-saved verbatim
(no decode at all); on a disk-backed database, where the stash holds
placeholders and the bytes live in `_dbsp_ckpt`, the save keeps that
view's existing rows in place instead of rewriting them.

One deliberate exception to "decodes transparently on first read": `dbsp_views()`
reports a pending view's row count from the checkpoint's stashed metadata
without decoding it (see `dbsp_views()` above) — it enumerates every view,
so realizing there would eagerly decode everything on the first call.

`dbsp_load()`'s report string counts views left pending separately from
"sources deferred" (D3c table baselines) and "from checkpoint":

```
Loaded views from _dbsp_views (43 views, 43 from checkpoint, 0 sources
deferred, 43 pending lazy restore)
```

Turn lazy restore off for bulk benchmarks or when debugging a restore
issue and you want every view's state visibly live immediately after
`dbsp_load()`:

```sql
SELECT * FROM dbsp_lazy_restore(true);   -- Enable (default)
SELECT * FROM dbsp_lazy_restore(false);  -- Disable (eager, pre-D-lazy behavior)
SELECT * FROM dbsp_lazy_restore();       -- Query status
```

### dbsp_stats()

Sync-path observability counters, for monitoring which ingestion path
serves a workload:

Three columns: `metric`, `value` (BIGINT), `detail` (VARCHAR, NULL on every
numeric row).

```sql
SELECT * FROM dbsp_stats();
-- metric                | value | detail
-- exact_delta_syncs    | 1042  |        -- exact table deltas applied (no scan)
-- scan_syncs            | 3     |        -- scan-and-diff fallbacks
-- commit_seq            | 1045  |        -- monotonic baseline mutations
-- tracked_tables        | 4     |
-- trigger_syncs         | 1310  |        -- trigger-body ingest calls served
-- trigger_rows          | 5218  |        -- row images they buffered
-- provisional_tables    | 0     |        -- awaiting a concurrency watermark
-- reconcile_failures    | 0     |        -- reconcile scans that did NOT run
-- last_reconcile_error  | NULL  | NULL   -- text of the last one
```

`trigger_syncs` is the proof of life: it stays 0 until a generated trigger body
has actually delivered, so a database whose triggers never fire is
distinguishable from one with nothing to report. An UPDATE fires ONE trigger
whose body evaluates the ingest scalar TWICE — the two arms of a UNION ALL over
the old and new transition tables — so it adds 2.

`provisional_tables` counts tables seeded while ANOTHER connection had a
transaction open. Such a transaction may already have written the table before
it was tracked — invisible to the seeding scan and reported by no trigger — so
the table takes no exact deltas and is reconciled by scan until every
transaction alive at seed time has ended, at which point one scan retires it.
It is 0 in an ordinary single-writer session. A value that never falls means a
connection is sitting on an open transaction; every commit is paying a scan of
those tables until it ends.

`reconcile_failures` counts reconcile scans that did NOT run, and
`last_reconcile_error` carries the text of the last one in `detail`. This is the
one way a view is left stale with the manager knowing it: the scan reports
failure by RETURNING, not by throwing, so nothing propagates out of the commit
hook and the only other trace is a stderr line an embedding host never sees.
A non-zero `reconcile_failures` means at least one baseline is unchanged when it
should have been rescanned — read `last_reconcile_error`, fix the cause (a
dropped source is the usual one) and run `dbsp_sync()`.

### dbsp_parallel(enable)

Toggle parallel execution (default off): multi-table syncs scan on
threads, views at the same dependency level update concurrently during
propagation, and large residual-free inner-join probe passes split
across cores.

```sql
SELECT * FROM dbsp_parallel(true);
```

### dbsp_mv_tables(enable)

Disk-backed MV results (bounded-RAM Phase 1b, default off): every registered
view mirrors its result into a `__mv_<view>` table in the same database —
backfilled on enable and at view creation, then kept in sync with one
internal transaction per propagation pass. `__dbsp_mv_meta(view_name,
commit_seq)` records the watermark of each table's last write. Backing
tables are ordinary tables: durable across reopen and readable without any
DBSP state. Disabling stops mirroring and leaves the tables stale, and the
disable is STICKY: `load_from_duck_table` runs more than once per manager (once
from the auto-load, again from crash recovery's `load_views`) and its
`__dbsp_mv_meta` block would otherwise re-enable mirroring behind the user's
back. Only an explicit `dbsp_mv_tables(true)` turns it back on.

```sql
SELECT * FROM dbsp_mv_tables(true);
```

Per-commit apply is DELETE (retractions) + INSERT (additions) via a staged
temp table; any delta weight beyond ±1 rebuilds the table from its own
current rows + the delta.

By default that DELETE has to identify the retracted row by comparing EVERY
column of the view (`t.c IS NOT DISTINCT FROM s.c`, one per column), so
applying a one-row delta scans the whole backing table across all its
columns — O(rows × columns) per commit. Declare a key with
`dbsp_set_view_key` to turn it into an equi-join.

### dbsp_set_view_key(view_name, key_columns)

Declares the view's unique, non-null row key as a comma-separated column
list. The delta-apply DELETE then matches those columns by equality instead
of comparing every column, which the planner can hash: measured 1.45x on a
whole authority commit of a 199-column view over a 1M-row grid, and 17x on
the DELETE in isolation.

```sql
SELECT * FROM dbsp_set_view_key('orders_mv', 'customer_id');
SELECT * FROM dbsp_set_view_key('orders_mv', '');  -- clear
```

Requires `dbsp_mv_tables(true)` first — the key is verified against the
`__mv_` backing table. The call FAILS if the columns are not unique over the
view's current rows, if any is NULL, if a column is not in the view, or if
the view is unknown. Uniqueness is checked against the rows that exist at
declaration time, which is all it can do: declare a key the view's SQL
GUARANTEES (a GROUP BY's grouping columns, a dense coordinate grid), never
one that merely happens to be unique right now — a key that stops being
unique later retracts the wrong row.

Plain `=` is the point. `IS NOT DISTINCT FROM` is not an equi-join key, so
narrowing the predicate without switching to equality does not help; it
measured slower than matching every column. That is why the key must be
non-null.

**Table-backed reads (Phase 1c)**: once a view's table is written, the sink
stops integrating — the RAM result is dropped and the __mv_ table IS the
result. `dbsp_query`, create-time replay of view sources, and shared-
arrangement backfill all stream from the table; checkpoints save operator
state only (rows are already durable). Enable is idempotent (table
functions can be bound twice per statement — a second backfill would run
after the results were dropped). Disabling schedules a full view rebuild so
sinks reintegrate in RAM.

### dbsp_realize(view_name)

Realize one pending (lazy-restored) view's circuit state now instead of on
first touch. Returns `realized` (whether it WAS pending) and `state_bytes`
(the view's resident state after realization) — sum the latter to warm
within a RAM budget and leave the tail lazy.

```sql
SELECT * FROM dbsp_realize('customer_totals');
```

### dbsp_wait_teardown()

Wait (bounded, ~30s max) for detached close-time teardown threads. Call from
a throwaway connection after closing your last real connection and before
process exit — exiting during the detached teardown segfaults in static
destructors. Pair with an explicit `dbsp_save()` before the final close so
the teardown has no SQL left to run.

```sql
SELECT * FROM dbsp_wait_teardown();
```

### dbsp_spill(enable)

Toggle disk-backed state (default off): tracked-table baselines, join
indexes (shared and local), top-K sort windows, and oversized holistic
aggregate groups move to disk record logs; RAM keeps compact digest
indexes and hot-bucket caches. Trades sync CPU (row serialization) for
bounded memory. Live state migrates both directions on toggle.

```sql
SELECT * FROM dbsp_spill(true);
```

## Error Handling

duckDBSP uses a structured error code system (DBSP-Exxx) that provides:

- **Error codes** in format `DBSP-E{category}{number}` (e.g., DBSP-E101)
- **Clear descriptions** of what went wrong
- **SQL highlighting** with position markers (^) showing exactly where the error occurred
- **Workarounds** suggesting alternative approaches for unsupported features
- **Documentation links** to detailed error explanations

### Error Categories

| Category | Description | Examples |
|----------|-------------|----------|
| **E1xx** | Unsupported SQL | E110 (plan operator not supported) |
| **E2xx** | Validation errors (invalid input) | E201 (Invalid identifier) |
| **E3xx** | Runtime errors (execution failures) | E301 (View update failed) |
| **E4xx** | Resource errors (limits exceeded) | E401 (Too many views) |
| **E5xx** | Persistence errors (I/O failures) | E501 (Load failed) |

### Example Error

```sql
SELECT * FROM dbsp_create_view('high_orders',
    'SELECT customer, SUM(amt) FROM orders GROUP BY customer HAVING SUM(amt) > 1000');

-- Error:
-- DBSP-E101: HAVING clause in GROUP BY
--
-- SQL:
-- SELECT customer, SUM(amt) FROM orders GROUP BY customer HAVING SUM(amt) > 1000
--                                                          ^
--
-- Workaround:
-- Use a nested view: create a view with GROUP BY, then create another view
-- with WHERE clause to filter the aggregated results. Tracked in TODO #3.
--
-- Documentation: docs/errors/E1xx/DBSP-E101.md
```

### Legacy Error Messages

Some operations still return simple error strings:

```sql
SELECT * FROM dbsp_query('nonexistent');
-- Throws: InvalidInputException("View not found: nonexistent")
```

### Getting Help

For complete error documentation and troubleshooting:
- See [Error Handling Guide](ERROR_HANDLING.md)
- Browse [Error Catalog](errors/README.md)
- Check specific error docs in `docs/errors/E{category}xx/`

---

## Type Mapping

| DuckDB Type | DBSP Internal Type |
|-------------|-------------------|
| INTEGER, BIGINT, SMALLINT, TINYINT | int64_t |
| DOUBLE, FLOAT, REAL | double |
| VARCHAR, TEXT | string |
| BOOLEAN | bool |
| DATE, TIMESTAMP | (converted to string) |

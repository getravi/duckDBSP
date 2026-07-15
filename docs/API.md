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

### REFRESH MATERIALIZED VIEW

```sql
REFRESH MATERIALIZED VIEW name;
```

Accepted for compatibility — a no-op, since views refresh automatically.

### DROP MATERIALIZED VIEW

DuckDB parses `DROP MATERIALIZED VIEW` natively, which bypasses the
extension's parser hook — use the function form instead:

```sql
SELECT dbsp_drop('name');           -- or dbsp_drop_cascade('name')
```

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
(ALL and DISTINCT), window functions (expressions auto-projected),
non-recursive CTEs, WITH RECURSIVE (deletions included), and ORDER
BY/LIMIT/OFFSET (constant or percentage). The few remaining gaps
(USING KEY recursion, expression LIMIT, approximate statistics like
approx_quantile, unordered string_agg) fail with a DBSP-E110 error
naming the construct.

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

---

## Persistence

### dbsp_save()

Save all view definitions to the `_dbsp_views` table in the default
catalog — inside the database file, so copies and backups carry the
views. `dbsp_save('view_name', 'table_name')` saves one view to a named
table (identifier-validated).

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
a missing named table IS an error). Loading rebuilds view state from
current table data.

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

Most plain SQL writes commit in **O(delta)** via captured deltas:

- **INSERT** — explicit transactions containing only INSERTs (G2,
  captured from transaction-local storage), and autocommit
  `INSERT ... VALUES` with a full-cover column list (the VALUES list is
  evaluated with the INSERT's own casts; ~1.0 ms at 1M rows).
- **UPDATE / DELETE** — explicit-transaction *and* autocommit statements
  (write capture: one internal SELECT reads the old images and computes
  the new ones before the statement runs; a commit guard — interleaved-
  commit check, signed COUNT(*), rowid re-verification — validates the
  captured delta against committed storage). A single-row UPDATE on a
  1M-row table syncs in ~1.5 ms vs ~2.4 s for scan-and-diff.

Everything else uses scan-and-diff scoped to the tables the transaction
touched: `INSERT ... SELECT` autocommits, partial-column-list INSERTs
(defaults), upserts, `UPDATE ... FROM`, `DELETE ... USING`,
CTEs/`RETURNING`, subqueries or prepared parameters in expressions,
non-deterministic expressions (`random()`, `now()`), UPDATEs of indexed
or LIST-typed columns, multi-statement strings, Appender writes, and any
transaction that writes the same table twice. If any
statement in a transaction is un-capturable, the whole transaction falls
back — captured and scanned deltas never mix for one commit, and guard
failures fall back loudly (`capture_guard_fallbacks` counter).
Correctness never depends on capture; the design is in
`docs/DESIGN_WRITE_CAPTURE.md`.

Turn auto-sync off for bulk loads (each autocommit INSERT pays a scoped
scan) and run one `dbsp_sync()` afterwards.

```sql
SELECT * FROM dbsp_auto_sync(true);   -- Enable
SELECT * FROM dbsp_auto_sync(false);  -- Disable
SELECT * FROM dbsp_auto_sync();       -- Query status
```

### dbsp_parallel(enable)

Toggle parallel execution (default off): multi-table syncs scan on
threads, views at the same dependency level update concurrently during
propagation, and large residual-free inner-join probe passes split
across cores.

```sql
SELECT * FROM dbsp_parallel(true);
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

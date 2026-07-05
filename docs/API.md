# DBSP for DuckDB - API Reference

Complete reference for all DBSP extension functions.

## Table of Contents

- [Table Tracking](#table-tracking)
- [View Management](#view-management)
- [Querying](#querying)
- [Persistence](#persistence)
- [Manual CDC](#manual-cdc)

---

## Table Tracking

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

Toggle the planner frontend (Phase B). When enabled, `dbsp_create_view`
first translates view SQL through DuckDB's own binder/planner instead of the
bespoke parser. Currently covers single-table scan/filter/projection plans
(arbitrary expressions, function calls, mixed AND/OR predicates); anything
else falls back to the bespoke parser transparently.

```sql
SELECT * FROM dbsp_use_planner(true);   -- Enable
SELECT * FROM dbsp_use_planner(false);  -- Disable (default)
SELECT * FROM dbsp_use_planner();       -- Query status
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

## Persistence

### dbsp_save()

Save all view definitions to DuckDB table `_dbsp_views`.

```sql
SELECT * FROM dbsp_save();
```

**Returns:**
- VARCHAR: Confirmation message

**Notes:**
- Creates `_dbsp_views` and `_dbsp_views_tables` if not exist
- Overwrites existing saved state
- Saves view SQL and creation timestamps

---

### dbsp_save(filepath)

Save all view definitions to a JSON file.

```sql
SELECT * FROM dbsp_save('/path/to/views.json');
```

**Parameters:**
- `filepath` (VARCHAR): Path to output JSON file

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

Load view definitions from DuckDB table `_dbsp_views`.

```sql
SELECT * FROM dbsp_load();
```

**Returns:**
- VARCHAR: Confirmation with view count

**Notes:**
- Recreates views in creation order (handles dependencies)
- Re-syncs with current table data
- Continues loading if individual views fail

---

### dbsp_load(filepath)

Load view definitions from a JSON file.

```sql
SELECT * FROM dbsp_load('/path/to/views.json');
```

**Parameters:**
- `filepath` (VARCHAR): Path to JSON file

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
| **E1xx** | Parser errors (unsupported SQL) | E101 (HAVING), E102 (ORDER BY) |
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

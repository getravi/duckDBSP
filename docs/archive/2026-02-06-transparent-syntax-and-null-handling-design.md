# Transparent Materialized View Syntax & NULL Handling - Design Document

**Date:** 2026-02-06
**Tasks:** #0 (Transparent Syntax) and #11 (NULL Edge Cases)
**Type:** Breaking Change - Major Feature

---

## Executive Summary

Transform duckDBSP from a function-based extension into a **native DuckDB feature** with:
- **Transparent SQL syntax**: `CREATE MATERIALIZED VIEW` DDL commands
- **Comprehensive NULL handling**: Full SQL standard compliance
- **Automatic incremental refresh**: Real-time updates on every commit
- **Full catalog integration**: Views queryable as regular tables

**Breaking Change:** Removes all `dbsp_*` function APIs in favor of native SQL.

---

## Design Decisions

### Core Principles
1. **Native SQL First**: Use standard DDL, not functions
2. **Automatic Everything**: No manual sync or refresh needed
3. **SQL Standard Compliance**: Comprehensive NULL handling per spec
4. **Full Integration**: Views in catalog, queryable directly
5. **Safe Defaults**: DROP requires CASCADE for dependents

### Key Choices
- **Implementation**: Parser Extension Hook (cleanest integration)
- **NULL Handling**: Comprehensive SQL standard compliance (not minimal)
- **API Strategy**: Complete replacement (breaking change, no legacy API)
- **Refresh Strategy**: Automatic incremental (showcases DBSP strength)
- **Catalog**: Full integration (views in information_schema)
- **Dependencies**: SQL-standard CASCADE/RESTRICT behavior

---

## Architecture Overview

### Integration Flow
```
User SQL: CREATE MATERIALIZED VIEW v AS SELECT...
    ↓
Parser Extension (intercept DDL)
    ↓
Parse & Validate (view name, SQL syntax)
    ↓
DBSP Manager (create incremental view)
    ↓
Catalog Registration (register as virtual table)
    ↓
Success / Error Response
```

### Query Flow
```
User SQL: SELECT * FROM v WHERE condition
    ↓
DuckDB Parser (sees table reference 'v')
    ↓
Catalog Lookup (finds MaterializedViewTableFunction)
    ↓
Bind Phase (get schema from DBSP)
    ↓
Execute Phase (scan Z-set data)
    ↓
Return Results
```

### Core Components

**1. Parser Extension Hook**
- Registers CREATE/DROP MATERIALIZED VIEW keywords
- Intercepts DDL before standard DuckDB processing
- Parses view definition and validates
- Routes to DBSP manager

**2. Catalog Integration Layer**
- Registers materialized views as virtual tables
- Implements TableFunction for view data access
- Adds entries to information_schema.tables
- Enables direct queries: `SELECT * FROM view_name`

**3. NULL-Aware Operators**
- Three-valued logic (TRUE/FALSE/UNKNOWN) in filters
- NULL-aware hashing and equality
- SQL-standard NULL propagation
- Comprehensive aggregate NULL handling

**4. Automatic Refresh Engine**
- Hooks into transaction commits
- Propagates changes through dependency graph
- Uses existing incremental DBSP operators
- Views always reflect committed state

---

## DDL Statement Support

### CREATE MATERIALIZED VIEW

**Syntax:**
```sql
CREATE MATERIALIZED VIEW view_name AS
  SELECT ... FROM ... WHERE ...;

CREATE MATERIALIZED VIEW IF NOT EXISTS view_name AS
  SELECT ...;
```

**Implementation:**
```cpp
class DBSPParserExtension : public ParserExtension {
public:
  void ParseStatement(ParserExtensionInfo &info,
                     const string &query,
                     vector<unique_ptr<SQLStatement>> &statements) {
    if (starts_with_ci(query, "CREATE MATERIALIZED VIEW")) {
      auto stmt = ParseCreateMaterializedView(query);
      statements.push_back(std::move(stmt));
      return;
    }
  }
};
```

**Execution Steps:**
1. Extract view name and SELECT query
2. Validate name (no duplicates, valid identifier)
3. Parse SELECT using existing DBSPSqlParser
4. Register with CDCManager (creates incremental view)
5. Create catalog entry as virtual table
6. Return confirmation or formatted error

### DROP MATERIALIZED VIEW

**Syntax:**
```sql
DROP MATERIALIZED VIEW view_name;           -- RESTRICT (default)
DROP MATERIALIZED VIEW view_name RESTRICT;  -- Explicit
DROP MATERIALIZED VIEW view_name CASCADE;   -- Drop with dependents
DROP MATERIALIZED VIEW IF EXISTS view_name;
```

**Dependency Handling:**
- **RESTRICT** (default): Fails if any dependent views exist
- **CASCADE**: Collects all transitive dependents, drops in reverse topological order
- **IF EXISTS**: Succeeds silently if view doesn't exist

**Implementation:**
```cpp
void ExecuteDropMaterializedView(const string &view_name, bool cascade) {
  auto dependents = cdc_manager.get_dependent_views(view_name);

  if (!dependents.empty() && !cascade) {
    throw ViewHasDependentsError(view_name, dependents);
  }

  if (cascade) {
    // Drop in reverse topological order
    auto drop_order = ComputeDropOrder(view_name);
    for (const auto &v : drop_order) {
      cdc_manager.drop_view(v);
      catalog.DropTable(v);
    }
  } else {
    cdc_manager.drop_view(view_name);
    catalog.DropTable(view_name);
  }
}
```

### REFRESH MATERIALIZED VIEW

**Syntax:**
```sql
REFRESH MATERIALIZED VIEW view_name;
```

**Behavior:**
- Parse statement for SQL compatibility
- Return success immediately (views are always auto-refreshed)
- Optionally: force full recomputation from base tables (debugging mode)

---

## Catalog Integration

### Virtual Table Registration

**When CREATE MATERIALIZED VIEW executes:**

```cpp
// 1. Create view in DBSP
bool success = cdc_manager.create_view(context, view_name, select_sql);

// 2. Register as virtual table in DuckDB catalog
auto table_function = CreateMaterializedViewTableFunction(view_name);
catalog.CreateTableFunction(context, table_function);

// 3. Add metadata to information_schema
catalog.CreateTable(context, CreateMaterializedViewMetadata(view_name));
```

### MaterializedViewTableFunction

```cpp
class MaterializedViewTableFunction : public TableFunction {
  string view_name;

  unique_ptr<FunctionData> Bind(ClientContext &context,
                                TableFunctionBindInput &input,
                                vector<LogicalType> &return_types,
                                vector<string> &names) {
    // Get view schema from DBSP
    auto view = cdc_manager.get_view(view_name);
    return_types = view->GetColumnTypes();
    names = view->GetColumnNames();

    return make_uniq<MaterializedViewBindData>(view_name);
  }

  void Execute(ClientContext &context,
               TableFunctionInput &input,
               DataChunk &output) {
    auto &data = input.bind_data->Cast<MaterializedViewBindData>();
    auto view = cdc_manager.get_view(data.view_name);

    // Scan DBSP Z-set and populate output chunk
    view->ScanToDataChunk(output);
  }
};
```

### Information Schema Integration

**Materialized views appear in standard tables:**

```sql
SELECT table_name, table_type
FROM information_schema.tables
WHERE table_type = 'MATERIALIZED VIEW';

-- Output:
-- table_name       | table_type
-- high_orders      | MATERIALIZED VIEW
-- customer_totals  | MATERIALIZED VIEW
```

**DBSP-specific metadata (supplemental):**

```sql
-- Custom function for DBSP stats
SELECT * FROM dbsp_view_stats();

-- Output:
-- view_name    | rows | version | last_update      | source_tables
-- high_orders  | 42   | 17      | 2026-02-06 10:15 | [orders]
```

### Direct Query Support

**Standard table syntax works:**

```sql
-- Query like a normal table
SELECT * FROM high_orders WHERE customer_id = 123;

-- Joins work
SELECT h.*, c.name
FROM high_orders h
JOIN customers c ON h.customer_id = c.id;

-- Subqueries work
SELECT * FROM orders
WHERE id IN (SELECT order_id FROM high_orders);
```

**Behind the scenes:**
1. Parser sees table reference
2. Catalog lookup finds MaterializedViewTableFunction
3. Bind gets schema from DBSP
4. Execute scans Z-set
5. Results returned

---

## NULL Handling - SQL Standard Compliance

### Three-Valued Logic Foundation

SQL uses three truth values: TRUE (1), FALSE (0), UNKNOWN (NULL)

**Truth Tables:**

```
AND      | TRUE    | FALSE   | UNKNOWN
---------|---------|---------|--------
TRUE     | TRUE    | FALSE   | UNKNOWN
FALSE    | FALSE   | FALSE   | FALSE
UNKNOWN  | UNKNOWN | FALSE   | UNKNOWN

OR       | TRUE    | FALSE   | UNKNOWN
---------|---------|---------|--------
TRUE     | TRUE    | TRUE    | TRUE
FALSE    | TRUE    | FALSE   | UNKNOWN
UNKNOWN  | TRUE    | UNKNOWN | UNKNOWN

NOT      | Result
---------|--------
TRUE     | FALSE
FALSE    | TRUE
UNKNOWN  | UNKNOWN
```

### NULL in WHERE Clauses

**Rule:** UNKNOWN treated as FALSE in WHERE clauses

```cpp
bool EvaluateFilter(const DuckDBRow &row, const Expression &expr) {
  auto result = EvaluateExpression(row, expr);

  // NULL handling: UNKNOWN -> FALSE (filtered out)
  if (result.IsNull()) {
    return false;
  }

  return result.GetValue<bool>();
}
```

**Examples:**
```sql
-- NULL values filtered out
SELECT * FROM orders WHERE amount > 100;
-- Rows with NULL amount excluded

-- IS NULL / IS NOT NULL
SELECT * FROM orders WHERE notes IS NULL;      -- Includes NULLs
SELECT * FROM orders WHERE notes IS NOT NULL;  -- Excludes NULLs

-- NULL in comparisons
SELECT * FROM orders WHERE amount = NULL;
-- Returns 0 rows (always UNKNOWN)
```

### NULL in GROUP BY

**Rule:** NULL values form their own group

```sql
-- Data:
-- customer_id | amount
-- 1           | 100
-- NULL        | 50
-- 1           | 200
-- NULL        | 75

SELECT customer_id, SUM(amount)
FROM orders
GROUP BY customer_id;

-- Result:
-- customer_id | SUM
-- 1           | 300
-- NULL        | 125    <- NULL forms a group
```

**Implementation:**

```cpp
// NULL-aware hash function
struct DuckDBRowHash {
  size_t operator()(const DuckDBRow &row) const {
    size_t hash = 0;
    for (const auto &val : row.columns) {
      if (val.IsNull()) {
        hash ^= 0x9e3779b97f4a7c15ULL;  // Special NULL hash
      } else {
        hash ^= val.Hash();
      }
    }
    return hash;
  }
};

// NULL-aware equality for GROUP BY
struct GroupByRowEqual {
  bool operator()(const DuckDBRow &a, const DuckDBRow &b) const {
    for (size_t i = 0; i < a.columns.size(); i++) {
      bool a_null = a.columns[i].IsNull();
      bool b_null = b.columns[i].IsNull();

      // For GROUP BY: NULL == NULL
      if (a_null && b_null) continue;
      if (a_null || b_null) return false;

      if (a.columns[i] != b.columns[i]) return false;
    }
    return true;
  }
};
```

### NULL in Aggregates

**SQL Standard Rules:**
- `COUNT(*)` counts all rows (including NULLs)
- `COUNT(column)` ignores NULLs
- `SUM/AVG/MIN/MAX` ignore NULLs
- All-NULL inputs return NULL (except COUNT returns 0)

```cpp
class NullAwareAggregateView {
  int64_t count = 0;      // Non-NULL values
  int64_t null_count = 0; // NULL values
  double sum = 0.0;

  void ProcessUpdate(const Value &value, int64_t weight) {
    if (value.IsNull()) {
      null_count += weight;  // Track NULLs separately
      return;  // Don't include in sum/avg
    }

    count += weight;
    sum += value.GetValue<double>() * weight;
  }

  Value GetSum() {
    if (count == 0) {
      return Value::NULL_VALUE;  // All NULLs -> NULL
    }
    return Value(sum);
  }

  Value GetCount() {
    return Value(count);  // COUNT(column) excludes NULLs
  }

  Value GetCountStar() {
    return Value(count + null_count);  // COUNT(*) includes NULLs
  }

  Value GetAvg() {
    if (count == 0) {
      return Value::NULL_VALUE;
    }
    return Value(sum / count);
  }
};
```

**Examples:**
```sql
-- Data: amounts = [100, NULL, 200, NULL]

SELECT COUNT(*) FROM orders;        -- 4
SELECT COUNT(amount) FROM orders;   -- 2 (excludes NULLs)
SELECT SUM(amount) FROM orders;     -- 300 (ignores NULLs)
SELECT AVG(amount) FROM orders;     -- 150 (sum/non-null count)

-- All NULLs
SELECT SUM(null_column) FROM t;     -- NULL
SELECT COUNT(null_column) FROM t;   -- 0
```

### NULL in JOIN Operations

**Rule:** NULL never equals NULL in join conditions

```sql
-- Data:
-- orders: id=1, customer_id=NULL
-- customers: id=NULL, name='Ghost'

SELECT * FROM orders o
JOIN customers c ON o.customer_id = c.id;

-- Result: 0 rows (NULL != NULL in joins)
```

**Implementation:**

```cpp
bool JoinKeysMatch(const DuckDBRow &left, const DuckDBRow &right,
                   const vector<size_t> &left_keys,
                   const vector<size_t> &right_keys) {
  for (size_t i = 0; i < left_keys.size(); i++) {
    auto &left_val = left.columns[left_keys[i]];
    auto &right_val = right.columns[right_keys[i]];

    // NULL handling: NULL never matches NULL in JOINs
    if (left_val.IsNull() || right_val.IsNull()) {
      return false;
    }

    if (left_val != right_val) {
      return false;
    }
  }
  return true;
}
```

**LEFT JOIN NULL handling:**

```sql
-- LEFT JOIN preserves left rows even with NULL keys
SELECT * FROM orders o
LEFT JOIN customers c ON o.customer_id = c.id;

-- Rows with NULL customer_id appear with NULL padding:
-- id | customer_id | customer.id | customer.name
-- 1  | NULL        | NULL        | NULL
```

```cpp
void ProcessLeftJoin(const DuckDBRow &left_row, int64_t weight) {
  bool matched = false;

  for (const auto &[right_row, right_weight] : right_table) {
    if (JoinKeysMatch(left_row, right_row, left_keys, right_keys)) {
      EmitJoinResult(left_row, right_row, weight * right_weight);
      matched = true;
    }
  }

  // No match: emit with NULL padding
  if (!matched) {
    auto padded = PadWithNulls(left_row, right_schema);
    output.insert(padded, weight);
  }
}
```

### NULL in DISTINCT

**Rule:** NULL values are considered equal for DISTINCT

```sql
-- Data: [1, NULL, 2, NULL, 1]

SELECT DISTINCT value FROM data;

-- Result: [1, 2, NULL]  <- Only one NULL
```

**Implementation:**

```cpp
// DISTINCT uses NULL-aware equality
struct DistinctRowEqual {
  bool operator()(const DuckDBRow &a, const DuckDBRow &b) const {
    for (size_t i = 0; i < a.columns.size(); i++) {
      bool a_null = a.columns[i].IsNull();
      bool b_null = b.columns[i].IsNull();

      // For DISTINCT: NULL == NULL
      if (a_null && b_null) continue;
      if (a_null || b_null) return false;

      if (a.columns[i] != b.columns[i]) return false;
    }
    return true;
  }
};

// Use in DistinctView
std::unordered_map<DuckDBRow, int64_t,
                   DuckDBRowHash,
                   DistinctRowEqual> distinct_rows;
```

**Key Differences:**
- **JOIN**: NULL ≠ NULL (no match)
- **DISTINCT**: NULL = NULL (dedup)
- **GROUP BY**: NULL = NULL (same group)

### NULL Propagation in Expressions

**Rule:** Most operations with NULL produce NULL

```cpp
Value EvaluateExpression(const DuckDBRow &row, const Expression &expr) {
  switch (expr.type) {
    case ExpressionType::BINARY_OP: {
      auto left = EvaluateExpression(row, expr.left);
      auto right = EvaluateExpression(row, expr.right);

      // NULL propagation
      if (left.IsNull() || right.IsNull()) {
        return Value::NULL_VALUE;
      }

      return ApplyOperator(expr.op, left, right);
    }

    case ExpressionType::COLUMN_REF: {
      return row.columns[expr.column_index];  // May be NULL
    }

    // ... other types
  }
}
```

**Examples:**
```sql
SELECT amount + 10 FROM orders;          -- NULL + 10 = NULL
SELECT CONCAT(first, ' ', last) FROM u;  -- NULL || ' ' || 'Smith' = NULL
SELECT amount > 100 FROM orders;         -- NULL > 100 = UNKNOWN
```

### NULL Functions

**COALESCE - Returns first non-NULL:**

```cpp
Value EvaluateCoalesce(const DuckDBRow &row,
                       const vector<Expression> &args) {
  for (const auto &arg : args) {
    auto value = EvaluateExpression(row, arg);
    if (!value.IsNull()) {
      return value;
    }
  }
  return Value::NULL_VALUE;  // All NULL -> NULL
}
```

```sql
SELECT COALESCE(nickname, first_name, 'Anonymous') as name FROM users;
-- Uses first non-NULL value
```

**NULLIF - Returns NULL if values equal:**

```cpp
Value EvaluateNullIf(const DuckDBRow &row,
                     const Expression &expr1,
                     const Expression &expr2) {
  auto val1 = EvaluateExpression(row, expr1);
  auto val2 = EvaluateExpression(row, expr2);

  if (val1.IsNull() || val2.IsNull()) {
    return val1;
  }

  return (val1 == val2) ? Value::NULL_VALUE : val1;
}
```

```sql
SELECT NULLIF(status, 'deleted') FROM records;
-- Returns NULL if status = 'deleted'
```

**IS NULL / IS NOT NULL:**

```cpp
Value EvaluateIsNull(const DuckDBRow &row, const Expression &expr) {
  auto value = EvaluateExpression(row, expr);
  return Value::BOOLEAN(value.IsNull());
}

Value EvaluateIsNotNull(const DuckDBRow &row, const Expression &expr) {
  auto value = EvaluateExpression(row, expr);
  return Value::BOOLEAN(!value.IsNull());
}
```

**Critical:** These are the ONLY way to test for NULL:
```sql
WHERE amount IS NULL      -- Correct
WHERE amount = NULL       -- Wrong! Always 0 rows
```

---

## Testing Strategy

### NULL Handling Test Suite

**Comprehensive coverage:**

```cpp
// test/unit/test_null_handling.cpp

TEST_CASE("NULL in WHERE clauses", "[null][filter]") {
  // NULL > 100 -> UNKNOWN -> filtered out
  // NULL IS NULL -> TRUE -> included
  // NULL IS NOT NULL -> FALSE -> filtered out
  // NULL = 100 -> UNKNOWN -> filtered out
}

TEST_CASE("NULL in GROUP BY", "[null][aggregate]") {
  // NULL values form their own group
  // Multiple NULL keys group together
  // NULL groups alongside non-NULL groups
}

TEST_CASE("NULL in aggregates", "[null][aggregate]") {
  SECTION("COUNT(*) includes NULLs") { ... }
  SECTION("COUNT(column) excludes NULLs") { ... }
  SECTION("SUM ignores NULLs") { ... }
  SECTION("AVG = SUM / non-null count") { ... }
  SECTION("All-NULL -> NULL result") { ... }
  SECTION("Mixed NULL and values") { ... }
}

TEST_CASE("NULL in JOINs", "[null][join]") {
  SECTION("INNER JOIN: NULL keys don't match") { ... }
  SECTION("LEFT JOIN: unmatched rows get NULL padding") { ... }
  SECTION("RIGHT JOIN: NULL handling") { ... }
  SECTION("Self-join with NULLs") { ... }
}

TEST_CASE("NULL in DISTINCT", "[null][distinct]") {
  // Multiple NULLs -> single NULL in output
  // Mixed NULL and non-NULL values
}

TEST_CASE("NULL propagation", "[null][expressions]") {
  // NULL + 5 -> NULL
  // NULL || 'text' -> NULL
  // NULL * 0 -> NULL (not 0!)
}

TEST_CASE("NULL functions", "[null][functions]") {
  SECTION("COALESCE") { ... }
  SECTION("NULLIF") { ... }
  SECTION("IS NULL / IS NOT NULL") { ... }
}

TEST_CASE("Incremental NULL updates", "[null][incremental]") {
  // Insert NULL values -> correct update
  // Update NULL to value -> correct delta
  // Update value to NULL -> correct delta
  // Delete NULL values -> correct update
}
```

### Integration Tests

```cpp
// test/integration/test_null_materialized_views.cpp

TEST_CASE("End-to-end NULL handling", "[null][e2e]") {
  db.exec("CREATE TABLE orders (id INT, customer_id INT, amount DECIMAL)");
  db.exec("INSERT INTO orders VALUES (1, NULL, 100), (2, 5, NULL), (3, NULL, NULL)");

  db.exec("CREATE MATERIALIZED VIEW summary AS "
          "SELECT customer_id, COUNT(*) as total_orders, SUM(amount) as total_amount "
          "FROM orders GROUP BY customer_id");

  auto result = db.query("SELECT * FROM summary ORDER BY customer_id NULLS FIRST");

  // Verify:
  // Row 1: customer_id=NULL, total_orders=2, total_amount=100
  // Row 2: customer_id=5, total_orders=1, total_amount=NULL

  // Test incremental update with NULL
  db.exec("INSERT INTO orders VALUES (4, NULL, 50)");

  result = db.query("SELECT * FROM summary WHERE customer_id IS NULL");
  // Verify: total_orders=3, total_amount=150
}

TEST_CASE("NULL in cascading views", "[null][cascading]") {
  // Base view with NULLs
  // Dependent view filters NULLs
  // Verify incremental propagation
}
```

### Parser Extension Tests

```cpp
// test/integration/test_transparent_syntax.cpp

TEST_CASE("CREATE MATERIALIZED VIEW", "[parser][ddl]") {
  SECTION("Basic syntax") {
    db.exec("CREATE MATERIALIZED VIEW v AS SELECT * FROM t");
    // Verify view exists in catalog
  }

  SECTION("IF NOT EXISTS") {
    db.exec("CREATE MATERIALIZED VIEW IF NOT EXISTS v AS SELECT * FROM t");
    db.exec("CREATE MATERIALIZED VIEW IF NOT EXISTS v AS SELECT * FROM t");
    // Second one succeeds silently
  }

  SECTION("Duplicate without IF NOT EXISTS") {
    db.exec("CREATE MATERIALIZED VIEW v AS SELECT * FROM t");
    REQUIRE_THROWS(db.exec("CREATE MATERIALIZED VIEW v AS SELECT * FROM t"));
  }
}

TEST_CASE("DROP MATERIALIZED VIEW", "[parser][ddl]") {
  SECTION("RESTRICT with dependents fails") { ... }
  SECTION("CASCADE drops dependents") { ... }
  SECTION("IF EXISTS") { ... }
}

TEST_CASE("Query materialized views", "[catalog][query]") {
  db.exec("CREATE MATERIALIZED VIEW v AS SELECT * FROM t WHERE x > 10");

  // Direct query
  auto result = db.query("SELECT * FROM v");
  // Verify results

  // Joins work
  result = db.query("SELECT * FROM v JOIN other ON v.id = other.id");

  // Subqueries work
  result = db.query("SELECT * FROM t WHERE id IN (SELECT id FROM v)");
}

TEST_CASE("information_schema integration", "[catalog]") {
  db.exec("CREATE MATERIALIZED VIEW v1 AS SELECT * FROM t");
  db.exec("CREATE MATERIALIZED VIEW v2 AS SELECT * FROM v1");

  auto result = db.query(
    "SELECT table_name, table_type FROM information_schema.tables "
    "WHERE table_type = 'MATERIALIZED VIEW' ORDER BY table_name");

  // Verify v1 and v2 appear
}
```

---

## Error Handling

### New Error Codes

```cpp
// Add to dbsp_errors.hpp
enum class ErrorCode {
  // ... existing codes ...

  // E2xx: Validation errors
  INVALID_MV_SYNTAX = 206,
  VIEW_ALREADY_EXISTS = 207,
  VIEW_NOT_FOUND = 208,
  VIEW_HAS_DEPENDENTS = 209,

  // E3xx: Runtime errors
  CATALOG_REGISTRATION_FAILED = 305,
  NULL_VIOLATION = 306,  // For future NOT NULL constraints
};
```

### Error Messages

**Invalid syntax:**
```cpp
if (!ValidateMaterializedViewSyntax(query)) {
  ErrorInfo error;
  error.code = ErrorCode::INVALID_MV_SYNTAX;
  error.message = "Invalid CREATE MATERIALIZED VIEW syntax";
  error.sql = query;
  error.workaround = "Use: CREATE MATERIALIZED VIEW name AS SELECT...";
  error.documentation = "docs/API.md#create-materialized-view";
  throw InvalidInputException(format_error(error));
}
```

**View already exists:**
```cpp
if (ViewExists(view_name)) {
  ErrorInfo error;
  error.code = ErrorCode::VIEW_ALREADY_EXISTS;
  error.message = "Materialized view already exists: " + view_name;
  error.workaround = "Use: CREATE MATERIALIZED VIEW IF NOT EXISTS " + view_name + " AS ...";
  throw InvalidInputException(format_error(error));
}
```

**View has dependents:**
```cpp
if (!dependents.empty() && !cascade) {
  ErrorInfo error;
  error.code = ErrorCode::VIEW_HAS_DEPENDENTS;
  error.message = "Cannot drop materialized view: other views depend on it";
  error.context = "Dependent views: " + JoinNames(dependents);
  error.workaround = "Use: DROP MATERIALIZED VIEW " + view_name + " CASCADE";
  throw InvalidInputException(format_error(error));
}
```

---

## Migration Guide

### Breaking Changes

**All function-based APIs removed:**

```sql
-- ❌ OLD (NO LONGER SUPPORTED)
SELECT * FROM dbsp_track('orders');
SELECT * FROM dbsp_create_view('high_value', 'SELECT * FROM orders WHERE amount > 100');
SELECT * FROM dbsp_query('high_value');
SELECT * FROM dbsp_sync('orders');
SELECT * FROM dbsp_drop('high_value');
SELECT * FROM dbsp_views();
SELECT * FROM dbsp_tables();
```

**New native syntax:**

```sql
-- ✅ NEW
-- Tracking is automatic
CREATE MATERIALIZED VIEW high_value AS
  SELECT * FROM orders WHERE amount > 100;

-- Query directly
SELECT * FROM high_value WHERE customer_id = 123;

-- Updates are automatic (no sync needed)
INSERT INTO orders VALUES (1, 'Alice', 500);

-- Standard DDL
DROP MATERIALIZED VIEW high_value;
DROP MATERIALIZED VIEW high_value CASCADE;

-- Standard catalog introspection
SELECT * FROM information_schema.tables WHERE table_type = 'MATERIALIZED VIEW';
```

### Migration Table

| Old Function API | New SQL Syntax | Notes |
|-----------------|----------------|-------|
| `dbsp_create_view('v', 'SELECT...')` | `CREATE MATERIALIZED VIEW v AS SELECT...` | Breaking change |
| `SELECT * FROM dbsp_query('v')` | `SELECT * FROM v` | Direct table access |
| `dbsp_drop('v')` | `DROP MATERIALIZED VIEW v` | Standard DDL |
| `dbsp_sync('t')` | Automatic | No action needed |
| `dbsp_track('t')` | Automatic | Tracked when referenced |
| `dbsp_views()` | `SELECT * FROM information_schema.tables WHERE table_type='MATERIALIZED VIEW'` | Standard catalog |
| `dbsp_save()` / `dbsp_load()` | TBD | Persistence API to be redesigned |

### User Communication

**Create `docs/MIGRATION_V4.md`:**
```markdown
# Migrating to duckDBSP 4.0

## What Changed

duckDBSP 4.0 transforms the extension from function-based to native SQL:
- ✅ Use `CREATE MATERIALIZED VIEW` instead of functions
- ✅ Query views directly: `SELECT * FROM view_name`
- ✅ Automatic incremental refresh (no manual sync)
- ✅ Comprehensive NULL handling per SQL standard

## Benefits

- **Native SQL**: Feels like first-class DuckDB feature
- **Automatic Updates**: No manual `dbsp_sync()` calls
- **Better Tooling**: Views appear in all introspection tools
- **Correctness**: SQL-standard NULL semantics

## Migration Steps

1. Replace `dbsp_create_view()` calls with `CREATE MATERIALIZED VIEW`
2. Replace `dbsp_query('view')` with `SELECT * FROM view`
3. Remove all `dbsp_sync()` calls (automatic now)
4. Replace `dbsp_drop()` with `DROP MATERIALIZED VIEW`

## Example

**Before (v3.x):**
```sql
SELECT * FROM dbsp_track('orders');
SELECT * FROM dbsp_create_view('high_value',
  'SELECT * FROM orders WHERE amount > 100');
SELECT * FROM dbsp_sync('orders');
SELECT * FROM dbsp_query('high_value');
```

**After (v4.0):**
```sql
CREATE MATERIALIZED VIEW high_value AS
  SELECT * FROM orders WHERE amount > 100;

SELECT * FROM high_value;
-- That's it! Updates are automatic.
```
```

---

## Implementation Roadmap

### Phase 1: NULL Handling Foundation (Week 1)

**Tasks:**
- [ ] Add NULL support to `DuckDBRow` hash and equality
- [ ] Update `FilterView` for three-valued logic
- [ ] Update `AggregateView` for NULL-aware aggregation
  - [ ] Separate null_count tracking
  - [ ] COUNT(*) vs COUNT(column)
  - [ ] NULL handling in SUM/AVG/MIN/MAX
- [ ] Update `JoinView` for NULL key handling
  - [ ] NULL never matches in join conditions
  - [ ] LEFT JOIN NULL padding
- [ ] Update `DistinctView` for NULL-aware deduplication
- [ ] Add COALESCE and NULLIF functions
- [ ] Add IS NULL / IS NOT NULL operators

**Tests:**
- [ ] 50+ NULL test cases covering all operators
- [ ] Incremental NULL update tests
- [ ] NULL edge case tests

**Deliverables:**
- All view types handle NULL per SQL standard
- Comprehensive test suite passing

### Phase 2: Parser Extension (Week 2)

**Tasks:**
- [ ] Research DuckDB 1.4.0 parser extension API
- [ ] Implement `DBSPParserExtension` class
- [ ] Parse `CREATE MATERIALIZED VIEW` statements
  - [ ] Extract view name
  - [ ] Extract SELECT query
  - [ ] Handle IF NOT EXISTS
- [ ] Parse `DROP MATERIALIZED VIEW` statements
  - [ ] Handle CASCADE / RESTRICT
  - [ ] Handle IF EXISTS
- [ ] Parse `REFRESH MATERIALIZED VIEW` (no-op)
- [ ] Integrate with DuckDB parser registration
- [ ] Add parser error handling with error codes

**Tests:**
- [ ] Parser accepts valid syntax
- [ ] Parser rejects invalid syntax with clear errors
- [ ] IF NOT EXISTS / IF EXISTS handling

**Deliverables:**
- Parser extension registered and working
- All DDL statements parsed correctly

### Phase 3: Catalog Integration (Week 2-3)

**Tasks:**
- [ ] Implement `MaterializedViewTableFunction`
  - [ ] Bind phase (get schema from DBSP)
  - [ ] Execute phase (scan Z-set to DataChunk)
- [ ] Register views as virtual tables in catalog
- [ ] Add `information_schema.tables` integration
  - [ ] Views show with `table_type='MATERIALIZED VIEW'`
- [ ] Test direct queries: `SELECT * FROM view_name`
- [ ] Test joins and subqueries with materialized views
- [ ] Implement `dbsp_view_stats()` for DBSP metadata

**Tests:**
- [ ] Views queryable as regular tables
- [ ] information_schema shows views
- [ ] Complex queries (joins, subqueries) work
- [ ] Schema changes handled correctly

**Deliverables:**
- Full catalog integration working
- Views feel completely native

### Phase 4: DDL Execution & API Removal (Week 3)

**Tasks:**
- [ ] Wire `CREATE MATERIALIZED VIEW` to `CDCManager.create_view()`
- [ ] Wire `DROP MATERIALIZED VIEW` to `CDCManager.drop_view()`
- [ ] Implement CASCADE dependency resolution
- [ ] Implement RESTRICT dependency checking
- [ ] Remove all old function-based APIs
  - [ ] Remove `dbsp_create_view()` function
  - [ ] Remove `dbsp_query()` function
  - [ ] Remove `dbsp_drop()` function
  - [ ] Remove `dbsp_track()` function (make automatic)
  - [ ] Remove `dbsp_sync()` function (make automatic)
- [ ] Update all examples to new syntax
- [ ] Update all documentation

**Tests:**
- [ ] CREATE MATERIALIZED VIEW creates views
- [ ] DROP CASCADE works correctly
- [ ] DROP RESTRICT fails on dependents
- [ ] Old function APIs no longer exist

**Deliverables:**
- Complete DDL support
- Function API completely removed
- Clean, native SQL-only interface

### Phase 5: Testing & Documentation (Week 4)

**Tasks:**
- [ ] Run full NULL test suite (50+ tests)
- [ ] Run full integration test suite
- [ ] Update all documentation
  - [ ] Update README.md
  - [ ] Update docs/API.md
  - [ ] Create docs/MIGRATION_V4.md
  - [ ] Update all examples
- [ ] Performance benchmarking
  - [ ] NULL handling overhead
  - [ ] Catalog lookup overhead
- [ ] Create release notes

**Tests:**
- [ ] All unit tests passing
- [ ] All integration tests passing
- [ ] No regressions in existing functionality
- [ ] Performance acceptable

**Deliverables:**
- Complete, documented, tested system
- Migration guide for users
- Release ready

---

## Success Criteria

### Functionality
- ✅ `CREATE MATERIALIZED VIEW name AS SELECT...` works
- ✅ `DROP MATERIALIZED VIEW name CASCADE/RESTRICT` works
- ✅ Views queryable with `SELECT * FROM view_name`
- ✅ Views appear in `information_schema.tables`
- ✅ NULL handling passes all SQL standard tests
- ✅ Incremental updates work with NULL values
- ✅ Automatic refresh on every transaction commit

### Testing
- ✅ 50+ NULL handling test cases pass
- ✅ All integration tests updated and passing
- ✅ No function-based API remains
- ✅ Parser extension tests pass
- ✅ Catalog integration tests pass

### Documentation
- ✅ Migration guide complete
- ✅ All examples use new syntax
- ✅ Error catalog updated
- ✅ API documentation complete

### Performance
- ✅ NULL handling adds <10% overhead
- ✅ Catalog lookup negligible overhead
- ✅ No regressions in incremental update speed

---

## Future Enhancements

**Not in scope for this design:**

- **Persistence API redesign** - `dbsp_save()` / `dbsp_load()` need rethinking for new catalog model
- **NOT NULL constraints** - Could add later if needed
- **Materialized view refresh policies** - Always automatic for now
- **Partial refresh** - Full incremental only
- **Query rewriting** - Automatic use of views in query optimization

---

## References

- SQL:2016 Standard (NULL semantics)
- DuckDB Parser Extension API Documentation
- DuckDB Catalog Integration Guide
- DuckDB Virtual Table Implementation
- Existing DBSP Theory (docs/THEORY.md)

---

**Status:** Design Complete - Ready for Implementation
**Next Step:** Create implementation plan and begin Phase 1

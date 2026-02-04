# Architecture

Internal design of the DBSP DuckDB extension.

## System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        DuckDB Extension                          │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                    Table Functions                           ││
│  │  dbsp_track, dbsp_create_view, dbsp_query, dbsp_sync, ...   ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                      CDC Manager                             ││
│  │  - Tracked Tables     - View Registry                       ││
│  │  - Change Detection   - Dependency Graph                    ││
│  │  - Change Propagation - Persistence                         ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                     SQL Parser                               ││
│  │  - Uses DuckDB's parser                                     ││
│  │  - Extracts: tables, columns, predicates, aggregates        ││
│  │  - Creates view definitions                                  ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                  Materialized Views                          ││
│  │  FilterView, ProjectView, AggregateView, JoinView, ...      ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                    Core DBSP Library                         ││
│  │  ZSet, DuckDBRow, Stream Operators                          ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
```

## Components

### 1. Core DBSP Library (`include/`)

Header-only C++ library implementing DBSP primitives.

#### dbsp_zset.hpp

```cpp
template <typename T, typename Hash = std::hash<T>>
class ZSet {
    std::unordered_map<T, Weight, Hash> data_;

public:
    void insert(const T& elem, Weight weight);
    Weight get(const T& elem) const;
    ZSet operator+(const ZSet& other) const;
    ZSet operator-() const;
    // Iterators for range-based for loops
};
```

#### dbsp_stream.hpp

Stream operators: Integration, Differentiation, Delay.

```cpp
template <typename T>
class Integration {
    ZSet<T> integrated_;
public:
    ZSet<T> process(const ZSet<T>& delta);
    void reset();
};

template <typename T>
class IncrementalDistinct {
    std::unordered_map<T, int64_t> counts_;
public:
    ZSet<T> process(const ZSet<T>& delta);
};
```

### 2. DuckDB Type Integration (`duckdb_extension/dbsp_duckdb_types.hpp`)

Native DuckDB Value-based data structures.

```cpp
struct DuckDBRow {
    std::vector<duckdb::Value> columns;
    bool operator==(const DuckDBRow& other) const;
};

struct DuckDBRowHash {
    size_t operator()(const DuckDBRow& row) const noexcept;
};

using DuckDBZSet = ZSet<DuckDBRow, DuckDBRowHash>;
```

### 3. SQL Parser (`duckdb_extension/dbsp_sql_parser.hpp`)

Uses DuckDB's parser to extract query structure.

```cpp
struct ParsedViewDef {
    enum ViewType { FILTER, PROJECT, AGGREGATE, JOIN, DISTINCT };
    ViewType type;
    std::string view_name;
    std::vector<std::string> source_tables;
    std::vector<FilterInfo> filters;
    std::vector<AggInfo> aggregates;
    // ...
};

class DBSPSqlParser {
public:
    ParseResult parse(const std::string& sql, const std::string& view_name);
};

class ViewFactory {
public:
    static std::unique_ptr<NativeMaterializedView> create_view(
        const ParsedViewDef& def,
        const std::unordered_map<std::string, TableSchema>& schemas);
};
```

### 4. CDC Manager (`duckdb_extension/dbsp_cdc.hpp`)

Central coordinator for change tracking and propagation.

```cpp
class CDCManager {
    // State
    std::unordered_map<std::string, TrackedTable> tracked_tables_;
    std::unordered_map<std::string, NativeMaterializedView> views_;
    std::unordered_map<std::string, ViewDefinition> view_definitions_;
    DependencyGraph dep_graph_;

public:
    // Table tracking
    bool track_table(ClientContext& ctx, const std::string& name);
    bool sync_table(ClientContext& ctx, const std::string& name);

    // View management
    bool create_view(ClientContext& ctx, const std::string& name, const std::string& sql);
    bool drop_view(const std::string& name);
    bool drop_view_cascade(const std::string& name);

    // CDC notifications
    void on_insert(const std::string& table, const DuckDBRow& row);
    void on_delete(const std::string& table, const DuckDBRow& row);

    // Persistence
    bool save_to_table(ClientContext& ctx);
    bool load_from_table(ClientContext& ctx);

private:
    void propagate_changes(const std::string& source);
};
```

### 5. Dependency Graph

Manages view dependencies for cascading updates.

```cpp
class DependencyGraph {
    std::unordered_map<std::string, std::set<std::string>> dependencies_;
    std::unordered_map<std::string, std::set<std::string>> dependents_;

public:
    void add_dependency(const std::string& view, const std::string& source);
    void remove_node(const std::string& node);
    bool would_create_cycle(const std::string& from, const std::string& to);
    std::vector<std::string> topological_order(const std::string& changed);
};
```

### 6. Extension Entry Point (`duckdb_extension/dbsp_extension.cpp`)

Registers all DuckDB functions.

```cpp
static void LoadInternal(DatabaseInstance &instance) {
    // Table functions
    ExtensionUtil::RegisterFunction(instance, "dbsp_track", ...);
    ExtensionUtil::RegisterFunction(instance, "dbsp_create_view", ...);
    ExtensionUtil::RegisterFunction(instance, "dbsp_query", ...);

    // Scalar functions
    ExtensionUtil::RegisterFunction(instance, "dbsp_drop", ...);
}
```

## Data Flow

### View Creation

```
1. User: dbsp_create_view('totals', 'SELECT customer, SUM(amount) FROM orders GROUP BY customer')
         │
2. SQL Parser: Parse SQL, extract structure
         │
         ▼
   ParsedViewDef {
     type: AGGREGATE,
     sources: ["orders"],
     aggregates: [{function: "SUM", column: "amount"}],
     group_by: ["customer"]
   }
         │
3. ViewFactory: Create appropriate view class
         │
         ▼
   NativeAggregateView {
     key_fn: extract customer,
     value_fn: extract amount,
     agg_type: SUM
   }
         │
4. CDC Manager: Register view, add dependencies
         │
5. Initialize: Apply current table state to view
```

### Change Propagation

```
1. User: INSERT INTO orders VALUES (1, 'Alice', 100)
2. User: dbsp_sync('orders')
         │
3. CDC Manager: Detect changes
         │
   Δorders = {(1, 'Alice', 100): +1}
         │
4. Get topological order: [filter_view, totals_view, vip_totals]
         │
5. For each view in order:
         │
   ┌─────┴─────┐
   │           │
   ▼           ▼
   filter_view.apply_changes('orders', Δorders)
   totals_view.apply_changes('orders', Δorders)
         │
         ▼
   vip_totals.apply_changes('totals', Δtotals)
```

### Incremental Aggregation Example

```
State before:
  agg_states = { 'Alice': {sum: 200, count: 2} }
  result = { ('Alice', 200): +1 }

Incoming delta:
  Δ = { ('Alice', 100): +1 }  -- Alice bought $100 more

Processing:
  1. For row ('Alice', 100) with weight +1:
  2. key = 'Alice'
  3. old_state = {sum: 200, count: 2}
  4. Output -('Alice', 200)  -- Remove old aggregate row
  5. new_state = {sum: 300, count: 3}
  6. Output +('Alice', 300)  -- Add new aggregate row

State after:
  agg_states = { 'Alice': {sum: 300, count: 3} }
  result = { ('Alice', 300): +1 }
```

## Threading Model

- Single global CDCManager instance (singleton)
- Mutex protects all state access
- Lock held during:
  - Table tracking
  - View creation/deletion
  - Change propagation
  - Queries

Future improvement: Reader-writer locks for concurrent queries.

## Memory Model

- All data stored in-memory
- No automatic eviction or bounds
- Persistence saves definitions only, not materialized state
- On load, views are rebuilt from current table data

## Extension Points

### Adding New View Types

1. Create class inheriting `NativeMaterializedView`
2. Implement `apply_changes()` with incremental logic
3. Add case to `ViewFactory::create_view()`
4. Update SQL parser to detect the pattern

### Adding New Aggregate Functions

1. Add to `AggType` enum
2. Implement in `NativeAggregateView::compute_agg()`
3. Update parser to recognize function name

## File Layout

```
duckdb_extension/
├── dbsp_extension.cpp      # Entry point, function registration
├── dbsp_extension.hpp      # Extension class declaration
├── dbsp_cdc.hpp           # CDC manager, dependency graph
├── dbsp_duckdb_types.hpp  # DuckDB-native Z-sets and views
├── dbsp_sql_parser.hpp    # SQL parsing and view factory
├── CMakeLists.txt         # Build configuration
└── build.sh               # Build script
```

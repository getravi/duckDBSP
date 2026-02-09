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

### 2. DuckDB Type Integration (`src/dbsp_duckdb_types.hpp`)

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

### 3. SQL Parser (`src/dbsp_sql_parser.hpp`)

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

### 4. DBSPOptimizer (`include/dbsp_optimizer.hpp`)

Circuit optimization passes for performance improvements.

```cpp
class DBSPOptimizer {
    OptimizationStats stats_;

public:
    ParsedViewDef optimize(const ParsedViewDef& def);
    
    // Optimization passes
    ParsedViewDef combine_filters(const ParsedViewDef& def);
    ParsedViewDef pushdown_filters(const ParsedViewDef& def);
    ParsedViewDef prune_projections(const ParsedViewDef& def);
    
    const OptimizationStats& stats() const { return stats_; }
};
```

**Filter Pushdown**: Moves filters closer to data sources through JOIN operations.

```
Before:
  JOIN(orders, customers) → FILTER(amount > 100)

After:
  JOIN(FILTER(orders, amount > 100), customers)
  
Benefit: Reduces rows entering the join, improving performance
```

**Projection Pruning**: Eliminates unused columns early in the pipeline.

```
Before:
  SELECT a, b FROM (SELECT a, b, c, d FROM table)

After:
  SELECT a, b FROM (SELECT a, b FROM table)
  
Benefit: Reduces memory usage and data movement
```

**Implementation**: The optimizer runs before view creation, transforming `ParsedViewDef` to include:
- `left_pushed_filters`: Filters applied to left JOIN input
- `right_pushed_filters`: Filters applied to right JOIN input
- `required_columns`: Minimal column set needed

### 5. Recursive Query Engine (`src/dbsp_sql_parser.hpp` + `NativeRecursiveView`)

Implements `WITH RECURSIVE` for transitive closures and fixed-point iteration.

```cpp
class NativeRecursiveView : public NativeMaterializedView {
    std::unique_ptr<NativeMaterializedView> anchor_view_;
    std::unique_ptr<NativeMaterializedView> recursive_view_;
    DuckDBZSet fixed_point_;
    
public:
    void apply_changes(const std::string& table_name, 
                      const DuckDBZSet& changes) override;
                      
private:
    void compute_fixed_point();
    bool has_converged(const DuckDBZSet& delta);
};
```

**Recursive Evaluation**:

```
1. Initialize: Evaluate anchor query (non-recursive part)
   T₀ = SELECT src, dst FROM edges

2. Iterate: Apply recursive query until convergence
   T₁ = T₀ ∪ (SELECT e.src, r.dst FROM edges e JOIN T₀ r ON e.dst = r.src)
   T₂ = T₁ ∪ (SELECT e.src, r.dst FROM edges e JOIN T₁ r ON e.dst = r.src)
   ...
   
3. Fixed point: When Tₙ₊₁ = Tₙ (no new rows), return result
```

**Incremental Updates**: When base table changes:
1. Compute Δ for anchor view
2. Re-run fixed-point iteration with new delta
3. Output changes to recursive view result

**Safety**: Maximum iteration limit (default 1000) prevents infinite loops.

### 6. CDC Manager (`src/dbsp_cdc.hpp`)

Central coordinator for change tracking and propagation.

```cpp
class CDCManager {
    // State
    std::unordered_map<std::string, TrackedTable> tracked_tables_;
    std::unordered_map<std::string, NativeMaterializedView> views_;
    std::unordered_map<std::string, ViewDefinition> view_definitions_;
    DependencyGraph dep_graph_;
    DBSPOptimizer optimizer_;  // NEW: Circuit optimizer

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

### 6. Extension Entry Point (`src/dbsp_extension.cpp`)

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
1. User: CREATE MATERIALIZED VIEW totals AS 
         SELECT customer, SUM(amount) FROM orders GROUP BY customer
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
3. DBSPOptimizer: Apply optimization passes
         │
         ▼
   Optimized ParsedViewDef {
     ...optimized structure...
     required_columns: ["customer", "amount"]
   }
         │
4. ViewFactory: Create appropriate view class
         │
         ▼
   NativeAggregateView {
     key_fn: extract customer,
     value_fn: extract amount,
     agg_type: SUM
   }
         │
5. CDC Manager: Register view, add dependencies
         │
6. Initialize: Apply current table state to view
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
- **Reader-writer locks** (`std::shared_mutex`) for concurrent queries
- Readers (queries) can run in parallel
- Writers (sync, create_view, drop) acquire exclusive locks
- Lock held during:
  - Table tracking (write)
  - View creation/deletion (write)
  - Change propagation (write)
  - Queries (read - shared lock)

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
src/
├── dbsp_extension.cpp      # Entry point, function registration
├── dbsp_cdc.hpp           # CDC manager, dependency graph
├── dbsp_duckdb_types.hpp  # DuckDB-native Z-sets and views
└── dbsp_sql_parser.hpp    # SQL parsing and view factory
```

Build layout:
```
duckDBSP/
├── CMakeLists.txt         # Build configuration
└── build.sh               # Build script
```

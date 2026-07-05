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
│  │  - Incremental cascade: one topological pass, deltas only   ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │            Planner Frontend (the only frontend)              ││
│  │  - Connection::ExtractPlan (DuckDB optimizer disabled)       ││
│  │  - Logical ops → PlanOpSpec tree → circuit nodes;            ││
│  │    bound expressions via ExpressionExecutor                  ││
│  │  - Circuit-IR optimizer: filter combine, join pushdown,      ││
│  │    filter+project fusion (plan_ir::optimize)                 ││
│  │  - Unsupported plan → DBSP-E110 error to the user            ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │        Materialized Views (PlannedCircuitView circuits)      ││
│  │  Fine-grained nodes: Filter, Map, FilterMap (fused),         ││
│  │    Aggregate, Join, Distinct, SetOp, Recursive               ││
│  │  Embedded views:     Window, Sort/Limit, DistinctOn          ││
│  │    (proven Native* views behind EmbeddedViewNode)            ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │              Circuit IR (dbsp::Circuit)                      ││
│  │  SourceNode → operator nodes → SinkNode; one step per delta ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                    Core DBSP Library                         ││
│  │  ZSet (DuckDBZSet = ZSet<DuckDBRow>), Stream Operators      ││
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

### 3. Planner Frontend (`include/dbsp_plan_translator.hpp`)

The only frontend since Phase C5 (the bespoke SQL parser was deleted once
the planner covered everything it did; `dbsp_use_planner()` remains callable
as a no-op). View SQL is parsed, bound, and planned by DuckDB itself via
`Connection::ExtractPlan` on an internal connection with the optimizer
disabled (keeps plan shapes canonical — no filter pushdown into scans). The
bound `LogicalOperator` tree is walked into a `PlanOpSpec` tree, rewritten
by the circuit-IR optimizer, and built into circuit nodes; bound expressions
are evaluated row-at-a-time through `ExpressionExecutor`.

Because materialized views are not in DuckDB's catalog, views-on-views are
bound by shadowing every existing MV as an empty TEMP table on the internal
connection during plan extraction (the temp schema shadows main, matching
CDC's own views-before-tables resolution order).

`PlannedCircuitView` builds a multi-source operator tree (one shared
SourceNode per base table) covering:

- GET / FILTER / PROJECTION — arbitrary expressions, function calls, mixed
  AND/OR predicates; virtual columns (rowid) scan as NULL (B1, C5)
- AGGREGATE — COUNT/SUM/AVG/MIN/MAX/FIRST, multi-aggregate GROUP BY,
  expression keys, HAVING (a FILTER above the aggregate); global
  aggregates keep exactly one row, even on empty input; SUM(DECIMAL)
  accumulates exactly in 128-bit unscaled form (`PlanAggregateNode`,
  B2, C5, D3). FIRST unlocks uncorrelated scalar-subquery comparisons
  (CROSS_PRODUCT + first()/count_star() guard plans).
- COMPARISON_JOIN — INNER (equi keys + residual comparisons, bilinear
  delta rule incl. the Δl⋈Δr self-join term); LEFT/RIGHT/FULL with
  incrementally reconciled NULL padding (per-row weighted match counts);
  MARK for IN/NOT IN with three-valued null-aware marks — plus
  CROSS_PRODUCT, DISTINCT, and UNION [ALL] / INTERSECT [ALL] /
  EXCEPT [ALL] (`PlanJoinNode`, `PlanDistinctNode`, `PlanSetOpNode`,
  B3, D2, D3)
- WINDOW — BoundWindowExpressions mapped onto `NativeWindowView`, embedded
  mid-circuit via `EmbeddedViewNode`; non-recursive CTEs
  (MATERIALIZED_CTE + CTE_REF) with the definition subtree built once and
  shared by all references (B4)
- ORDER BY / LIMIT / OFFSET — folded (with a trailing pure-column-ref
  projection) into one `NativeSortView`/`NativeLimitView` behind an
  `EmbeddedViewNode`; a root sort/limit drives `dbsp_query`'s scan order (C1)
- WITH RECURSIVE — `PlanRecursiveNode`: anchor inline, the recursive step as
  a nested `PlannedCircuitView` iterated to a fixed point; multi-table
  recursive steps supported. Insert-only deltas maintain incrementally;
  deltas with deletions trigger a full fixed-point recompute from
  integrated inputs (correct, non-incremental) (C2, hardening)
- DISTINCT ON — `NativeDistinctOnView` behind an `EmbeddedViewNode`;
  winner-pick order comes from the DISTINCT node's own order modifier (C3)

Filter/map/fused nodes evaluate expressions in vectorized DataChunk
batches (`BatchEvaluator`, one shared chunk per node, typed column
fill/read fast paths, `Slice` for filter survivors); sources and sinks
borrow the caller's deltas instead of copying (D1: 2-2.5x on the
maintenance and recovery-replay paths).

Anything else (correlated subqueries / DELIM_JOIN, USING KEY recursion,
non-constant LIMIT, ...) fails view creation with a DBSP-E110 error naming
the unsupported operator.

### 4. Circuit-IR Optimizer (`plan_ir::optimize`, in `dbsp_plan_translator.hpp`)

Rewrites the `PlanOpSpec` tree between translation and circuit construction
(Phase C4; successor of the deleted `ParsedViewDef`-based DBSPOptimizer).
Gated by `dbsp_native::g_plan_ir_optimize` (default on).

```
1. combine_filters:  FILTER(FILTER(x))  →  one FILTER with an AND list
2. pushdown_filters: FILTER above JOIN  →  per-side FILTERs below the join
   (right-side predicates get column indices shifted; copies live in
   PlanKeepAlive::rewritten_exprs so node lambdas never dangle)
3. fuse_filter_map:  MAP(FILTER(x))     →  one PlanFilterMapNode
   (no intermediate Z-set between WHERE and SELECT)
```

Projection pruning was deliberately not ported: DuckDB's binder already
prunes via GET `column_ids`, so canonical plans have nothing left to prune.

### 5. Recursive Queries (`PlanRecursiveNode`, in `dbsp_plan_translator.hpp`)

Implements `WITH RECURSIVE` for transitive closures and fixed-point
iteration. The anchor subtree builds inline in the outer circuit; the
recursive step becomes a nested `PlannedCircuitView` whose self-reference is
a sentinel source. Each circuit step seeds the frontier with the anchor
delta plus the step's reaction to base-table deltas, then iterates:

```
1. Seed:    frontier = Δanchor ∪ step(Δbase-tables)
2. Iterate: feed frontier into the sentinel source, collect the step's
            output delta, admit new rows (UNION dedups against persistent
            accumulated state; UNION ALL admits everything)
3. Stop:    frontier empty, or max_iterations (1000) reached
```

UNION dedup state persists across deltas, so a row that becomes reachable
again later is not double-counted. Deltas containing deletions fall back to
a full fixed-point recompute from integrated anchor/base state (a derived
row may be supported by many recursion paths, so retraction cannot be
decided locally); the node emits the diff against its previous result.

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
2. Planner frontend: DuckDB binds and plans the SQL
   (Connection::ExtractPlan, optimizer off; existing MVs shadowed as
   empty TEMP tables so views-on-views bind)
         │
         ▼
   LogicalOperator tree → PlanOpSpec tree {
     AGGREGATE { groups: [customer], aggs: [SUM(amount)] }
       └─ SOURCE { table: orders }
   }
         │
3. Circuit-IR optimizer (plan_ir::optimize): filter combine,
   join pushdown, filter+project fusion
         │
4. PlannedCircuitView: build circuit nodes from the spec tree
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

### Lifetime

- Planner-frontend views hold an internal DuckDB Connection and therefore
  **pin the DatabaseInstance** while they exist. This is required: their
  expression-executor buffers come from the instance's allocator, so a view
  must never outlive its instance. Drop views (or `reset()` the manager) to
  release the instance while the process runs.
- `CDCManager` is a **deliberately leaked** heap singleton: no destructors
  run at process exit, so DuckDB shutdown never happens during static
  teardown (which previously caused intermittent exit segfaults). The OS
  reclaims everything at exit.

## Extension Points

### Adding New Plan Operators

1. Add a `PlanOpSpec::Kind` and a `Walker::visit_*` for the logical operator
2. Add the matching `PlannedCircuitView::build` case (fine-grained node, or
   wrap a proven `Native*` view in an `EmbeddedViewNode`)
3. Cover it in `test/integration/test_planner_frontend.cpp` (differential)

### Adding New Aggregate Functions

1. Add to `PlanAggSpec::Fn` and map the DuckDB function name in
   `Walker::visit_aggregate`
2. Implement accumulate/emit in `PlanAggregateNode`

## File Layout

```
src/
├── dbsp_extension.cpp           # Entry point, function registration
include/
├── dbsp_cdc.hpp                 # CDC manager, dependency graph
├── dbsp_duckdb_types.hpp        # DuckDB-native Z-sets and views
└── dbsp_plan_translator.hpp     # Planner frontend + circuit-IR optimizer
```

Build layout:
```
duckDBSP/
├── CMakeLists.txt         # Build configuration
└── build.sh               # Build script
```

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
│  │    (same-level views step in parallel with dbsp_parallel)   ││
│  │  - Shared join arrangements: one index per (table, keys)    ││
│  │    fingerprint, probed by matching joins across all views   ││
│  │  - Spill mode (dbsp_spill): baselines, join indexes,        ││
│  │    top-K windows, big agg groups on disk; digest indexes    ││
│  │    + LRU bucket caches in RAM                               ││
│  └─────────────────────────────────────────────────────────────┘│
│                              │                                   │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │      Auto-sync hooks (DBSPContextState, per connection)      ││
│  │  - O(Δ) captured deltas: INSERT txns (LocalStorage scan,    ││
│  │    G2) + whitelisted UPDATE/DELETE incl. autocommit          ││
│  │    (pre-image capture SELECT, dbsp_write_capture.hpp)        ││
│  │  - Commit guard: seq conflict + signed COUNT(*) + rowid      ││
│  │    re-verify; any miss → scoped scan-and-diff fallback       ││
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
bound by shadowing referenced MVs as empty TEMP tables on the internal
connection during plan extraction (the temp schema shadows main, matching
CDC's own views-before-tables resolution order). Only views whose names
appear in the SQL text are shadowed — shadowing all N existing views made
DAG creation O(N^2) in catalog work. `CREATE MATERIALIZED VIEW` statements
are classified READ by the commit hook (they only read tracked tables), so
view creation never triggers a tracked-table scan-diff.

`PlannedCircuitView` builds a multi-source operator tree (one shared
SourceNode per base table) covering:

- GET / FILTER / PROJECTION — arbitrary expressions, function calls, mixed
  AND/OR predicates; virtual columns (rowid) scan as NULL (B1, C5)
- AGGREGATE — COUNT/SUM/AVG/MIN/MAX/FIRST, multi-aggregate GROUP BY,
  expression keys, HAVING (a FILTER above the aggregate); global
  aggregates keep exactly one row, even on empty input; SUM(DECIMAL)
  accumulates exactly in 128-bit unscaled form (`PlanAggregateNode`,
  B2, C5, D3). FIRST unlocks uncorrelated scalar-subquery comparisons
  (CROSS_PRODUCT + first()/count_star() guard plans). DISTINCT and
  FILTER (WHERE ...) modifiers maintain incrementally (value-weight
  presence transitions / per-aggregate predicate slots); ordered
  STRING_AGG/ARRAY_AGG re-render from sorted per-group entries; holistic
  MEDIAN/QUANTILE_CONT/QUANTILE_DISC/MAD read the sorted multiset, MODE
  per-value multiplicities (ties break by smallest value).
  ROLLUP/CUBE/GROUPING SETS fan out into one aggregate branch per
  grouping set (typed-NULL pads + constant GROUPING() masks) unioned
  with UNION ALL (J, J2, L1).
- COMPARISON_JOIN — INNER (equi keys + residual comparisons, bilinear
  delta rule incl. the Δl⋈Δr self-join term; a bare-scan side may probe
  a shared CDC-owned arrangement instead of a private index — see
  CDC Manager below); LEFT/RIGHT/FULL with
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
  deltas with deletions on UNION (set-semantics) recursion take the
  incremental DRed (Delete-Rederive) path (overdelete + rederive
  fixpoints); a **linear** UNION ALL step (the recursive relation
  referenced exactly once) rides the same incremental fixpoint with
  signed weights instead — only a **nonlinear** UNION ALL step (referenced
  ≥2 times) still falls back to a full fixed-point recompute (weighted
  deletion through a self-join doesn't distribute) (C2, hardening; DRed,
  2026-07-07; linear signed deltas, 2026-07-31)
- DISTINCT ON — `NativeDistinctOnView` behind an `EmbeddedViewNode`;
  winner-pick order comes from the DISTINCT node's own order modifier (C3)

Filter/map/fused nodes evaluate expressions in vectorized DataChunk
batches (`BatchEvaluator`, one shared chunk per node, typed column
fill/read fast paths, `Slice` for filter survivors); sources and sinks
borrow the caller's deltas instead of copying (D1: 2-2.5x on the
maintenance and recovery-replay paths).

Correlated subqueries translate too (E2): DELIM_JOIN becomes an
incremental DISTINCT of the outer side's correlated columns feeding the
decorrelated subplan (DELIM_GET = shared reference to it), joined back
with null-safe keys — SINGLE onto LEFT-join padding, MARK onto marks.

Anything else (USING KEY recursion, non-constant LIMIT, string_agg, ...)
fails view creation with a DBSP-E110 error naming the unsupported
operator.

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
again later is not double-counted. Deltas containing deletions take one of
three paths depending on recursion semantics:

- **Linear UNION ALL recursion** (the recursive relation referenced exactly
  once in the step subtree — `linear_step_`, detected at build time by
  scanning the step's `PlanOpSpec` for sentinel-table `SOURCE` refs) — the
  step is linear in the recursive relation: `step(R + δ) = step(R) +
  step(δ)`. Retraction-bearing deltas therefore skip the deletion
  special-case entirely and ride the *same* incremental fixpoint as
  insertions, just with signed weights: `admit`'s UNION ALL branch does
  plain multiset arithmetic on `accumulated_` (add the signed weight; a row
  whose running weight nets to zero is erased, exactly like ordinary
  `ZSet::insert`), and `iterate` pushes signed frontiers through
  `step_view_` unchanged (join/filter/project are all weight-linear). A
  `max_iterations_` trip, or any exception during admission/iteration,
  discards the attempt (restoring `accumulated_`/`output_` from a
  pre-admission snapshot) and falls back to `recompute()` — logged under
  `DBSP_DEBUG_SYNC` — rather than emit a partial delta. The same scan also
  vetoes `linear_step_` when the step subtree contains `AGGREGATE`,
  `DISTINCT`, `DISTINCT_ON`, `WINDOW`, `SORT_LIMIT`, or non-`UNION ALL`
  `SET_OP` — even with exactly one sentinel ref, these operators collapse or
  reorder rows and are not weight-linear, so they cannot ride the signed path
  (the planner still accepts these shapes inside a recursive step and they
  remain pre-existing-wrong on every path — this veto only stops the signed
  path from also claiming linearity for them).
- **UNION (set-semantics) recursion** — the incremental DRed
  (Delete-Rederive) path: an overdelete fixpoint over-approximates the
  retraction from the affected subgraph, then a rederive fixpoint re-admits
  rows that still have alternative support (cycles included) by re-running
  the step over the surviving relation. `dred()` mirrors every
  `accumulated_` mutation to the sentinel source so the two stay in sync.
- **Nonlinear UNION ALL recursion** (recursive relation referenced ≥2 times,
  e.g. a self-join of the working relation) — `recompute()`: a full
  fixed-point recompute from integrated anchor/base state. Weighted
  deletion through a self-join doesn't distribute over `+`, so this path
  stays non-incremental by design. `recompute()` also serves as the
  differential-test oracle for both the DRed and linear-signed paths.

All paths emit the diff against the node's previous result.

**Safety**: Maximum iteration limit (default 1000) prevents infinite loops.
If a DRed overdelete or rederive fixpoint hits that cap before converging,
`dred()` restores the pre-delta state and falls back to the self-healing
`recompute()` rather than emit a silently corrupted diff — the linear
signed path's `iterate()` cap trip recovers the same way.

### 6. CDC Manager (`src/dbsp_cdc.hpp`)

Central coordinator for change tracking and propagation.

```cpp
class CDCManager {
    // State
    std::unordered_map<std::string, TrackedTable> tracked_tables_;
    std::unordered_map<std::string, NativeMaterializedView> views_;
    std::unordered_map<std::string, ViewDefinition> view_definitions_;
    DependencyGraph dep_graph_;
    // I1: shared join arrangements — one index per (table, projection,
    // key-exprs) fingerprint, probed by every matching join side across
    // views; weak refs (join nodes own), updated before views step in
    // propagate_changes
    std::unordered_map<std::string, std::weak_ptr<SharedArrangement>> arrangements_;

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
    void propagate_changes(const std::string& source);      // delegates ↓
    void propagate_changes_multi(sources);  // ONE pass for a whole commit
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
   (Connection::ExtractPlan, optimizer off; SQL-referenced MVs shadowed
   as empty TEMP tables so views-on-views bind)
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
   filter_view.apply_changes_batch({'orders': Δorders})
   totals_view.apply_changes_batch({'orders': Δorders})
         │
         ▼
   vip_totals.apply_changes_batch({'totals': Δtotals})
```

A view fed by SEVERAL sources updated in the same pass (e.g. a join of
two sibling views over one base table) receives all of their deltas in
ONE `apply_changes_batch` call = one circuit step — required for the
join's both-shared bilinear correction (−Δl⋈Δr) to fire. Per-source
sequential applies would overcount Δl⋈Δr and strand stale rows.

The same rule holds one level up, at the commit boundary: a transaction
that wrote SEVERAL tracked tables runs as ONE propagation pass
(`propagate_changes_multi` — every commit path collects all table deltas
first: engine-hook and write-capture via `apply_captured_deltas`, the
scan fallback via `sync_tables`). Per-table passes would rewrite each
downstream view's single-generation `dbsp_changes` buffer (a view over
both tables keeps only the last table's effects) and miss the join
both-shared correction above. All views stepped in the pass share one
delta generation.

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
- With `dbsp_parallel(true)`: multi-table syncs scan on threads, and
  views at the same dependency level step concurrently during
  propagation (each level's inputs are frozen; results publish in
  stable order; levels under 256 input rows stay sequential)
- Writers (sync, create_view, drop) acquire exclusive locks
- Lock held during:
  - Table tracking (write)
  - View creation/deletion (write)
  - Change propagation (write)
  - Queries (read - shared lock)

## Memory Model

- All data stored in-memory
- No automatic eviction or bounds
- Persistence saves definitions plus (D3b) a circuit-state checkpoint for
  supported views: aggregate group scalars plus (Task 2, cold/restore
  attack) the per-group values multiset for plain non-DISTINCT MIN/MAX,
  private INNER/LEFT/RIGHT-join indexes plus (LEFT/RIGHT) their pad
  bookkeeping, sink results, (Task 1, restore-tail) `WITH RECURSIVE`
  fixed-point state for UNION ALL recursion whose step is itself wholly
  checkpointable — the integrated `accumulated_`/`anchor_total_`/
  `base_totals_` totals plus the step's own nested circuit-node state
  (embedded sub-blob) — and (Task 2, restore-tail) a `NativeWindowView`
  embedded behind `EmbeddedViewNode` (the WINDOW plan shape): each
  partition's ordered source rows plus its rendered-output cache
  (`partitions_`/`partition_outputs_`), sized however that partition
  currently is — watermarked by source COUNT + bit_xor(hash(row)).
  Window partition rows live PACKED at rest (`dbsp_window_rows.hpp`):
  each store is one byte arena + positional slot directory using the
  join-index codec (`dbsp_packed_row.hpp`), with binary searches
  comparing sort columns straight from the packed bytes and render
  passes memoizing full-row decodes; codec-unsupported types flip that
  store to boxed rows transparently. Packed window checkpoint blobs are
  bulk byte copies (in-blob `kWindowPackedMagic` marker); legacy
  row-by-row blobs remain readable and re-encode on restore, while a
  boxed-fallback store still writes the legacy layout.
  `EmbeddedViewNode` reports whatever its wrapped view reports
  (`NativeMaterializedView::circuit_state_kind()`), so a `NativeSortView`/
  `NativeLimitView`/`NativeDistinctOnView` behind the same wrapper (the
  SORT_LIMIT/DISTINCT_ON plan shapes) still declines. DISTINCT and
  holistic/ordered aggregates (MEDIAN, QUANTILE_*, MODE, MAD, STRING_AGG,
  ARRAY_AGG, FIRST), FULL (OUTER) and MARK joins, spilled join/aggregate
  state, UNION (set-semantics) recursion, a UNION ALL recursive step
  containing any other UNSUPPORTED node, and sort/limit/distinct-on
  embedded views stay UNSUPPORTED (rebuild-by-replay).
- On load, checkpointed views restore without circuit replay when the
  watermarks still match. Watermark validity is **per table** (O(delta)
  recovery, increment A): a changed table lands in
  `CkptData.stale_tables`, and only views whose source closure (walked
  from the persisted `sources` column in `created_at` order) touches a
  stale table rebuild by replay — every other view keeps its
  checkpoint. A crash that lost one edit to one table costs a replay of
  that table's dependents, not the whole DAG
- **Streaming create mirror** (bounded-RAM Phase 5): with mv tables
  enabled, `create_view` marks the sink table-backed *before* the
  initial replay and flushes each replay step's delta straight into the
  `__mv_` backing table — a view's initial result never integrates in
  RAM. Companion bounds on the same path: initial baseline population
  runs through the rebuild flow (never per-row `insert()` into
  `pending_changes_`), baselines auto-spill above
  `DBSP_SPILL_THRESHOLD_ROWS` (default 2M) including mid-scan, and
  `SpilledBaseline::install_rebuild()` swaps a generation in without
  the diff path's payload read-back. Big-table scans stream
  pre-serialized row bytes straight off flattened chunk vectors
  (`serialize_chunk`) — no per-cell `duckdb::Value` on the build path
- **Durable baselines**: file-backed databases keep spill logs in
  `<dbfile>.dbsp_spill/`; `dbsp_save` writes a digest-index sidecar
  under the checkpoint watermark, and a clean reopen's first table
  touch adopts the log+index pair instead of rescanning the table.
  Watermark or log-size mismatch rejects the sidecar and rescans
- **Flat restore layers (tier 2)**: reopen paths never rebuild hash
  maps. The baseline index sidecar (v2) is digest-sorted and mmap'd
  read-only (binary-search lookups, mutation overlay on top); packed
  join checkpoint blobs decode into a contiguous arena + key-sorted
  directory (`dbsp_flat_packed.hpp`); shared packed arrangements get a
  fingerprint sidecar written at save and ADOPTED at register over a
  watermark-verified deferred table (`sharr_*.flat`) — first edit after
  reopen stops paying a whole-baseline backfill. Serialization folds
  flat + overlay back into one stream
- **Delta-append sidecars**: with an adopted base and a small overlay, a
  dirty save writes only the changes — the digest-index overlay as
  `.idx.d` and per-arrangement replacement buckets for touched keys as
  `<sharr>.d` — each chained to its base by watermark (+ entry count /
  live log size); adopt loads base + delta, any chain mismatch rescans.
  Oversized overlays compact into a fresh self-adopted base. Every save
  records the watermark it saved under, so a same-content save writes
  nothing. An empty-diff `end_rebuild` discards instead of swapping — a
  no-change sync must not destroy the adopted flat layer or the delta
  chain. Measured 18M rows: dirty save 7.1s → 0.03s
- **Lazy per-view restore (D-lazy)**, default ON (`dbsp_lazy_restore`):
  a watermark-matched load cold-creates the view exactly as before, but
  instead of decoding its node/sink blobs immediately it stashes the
  already-read bytes in `CDCManager::pending_restore_` (`name -> {nodes,
  sink}`) and marks the view pending
  (`NativeMaterializedView::is_pending_restore()`). `dbsp_load()` returns
  without paying any per-view decode cost; each view's
  `realize_pending_view`/`realize_pending_view_locked` decodes on first
  need — mirrors D3c's `TrackedTable::is_deferred()` +
  `materialize_deferred_locked` shape (and its locking discipline:
  `pending_restore_` is guarded by the same `view_mutex_` tier that
  already owns view content, no new lock level). Realization is wired
  into every surface that reads a view's live state — `dbsp_query`'s
  read path (`scan_view`/`query_view`), an incoming delta reaching the
  view or a pending ancestor of it (`propagate_changes`'s pre-pass, which
  runs sequentially before any per-level parallel `step_view` work so
  `pending_restore_`'s single shared map is never touched from more than
  one thread at a time), a warm `create_view` replay or shared-arrangement
  backfill reading another view's result as a source, and
  `dbsp_replace_view`/drop (which discard rather than decode a dropped
  view's stash). `save_checkpoint` re-saves a still-pending view's stash
  **verbatim** — valid because "pending" is definitionally "no deltas
  applied since the stash," so the undecoded bytes are still exactly
  current. `dbsp_lazy_restore(false)` reproduces the pre-D-lazy eager
  behavior (every checkpointed view fully restored during the load call
  itself).
- A checkpoint blob format version travels alongside the data (a
  dedicated `_dbsp_ckpt_version` table); a checkpoint saved under an
  older layout — or missing the table entirely — is treated as absent
  rather than misparsed, so a version bump costs at most one
  rebuild-by-replay on the next load
- Each checkpointed view also carries a SQL fingerprint (its exact
  definition at save time). A load compares this against the SQL it is
  about to use to cold-create the view and declines the checkpoint fast
  path for that view alone on a mismatch — the guard against a view whose
  definition changed (e.g. `dbsp_replace_view`) after the checkpoint was
  written but before the next save landed
- Blob row values (`BlobWriter`/`BlobReader::row`, `dbsp_checkpoint.hpp`) go
  through the same typed row codec as spill (`serialize_row`/
  `deserialize_row`, `dbsp_spill_store.hpp`) — a tag-byte fast path for
  `INTEGER`/`BIGINT`/`DOUBLE`/`VARCHAR`/`BOOLEAN` and their typed `NULL`s,
  falling back to DuckDB's `BinarySerializer` for everything else; one
  codec, no per-checkpoint-site duplication. `BlobReader::hashed_row()`
  additionally pre-seeds the decoded row's hash cache (`hash_row_fast`,
  calling the same `duckdb::Hash<T>` primitives `VectorOperations::Hash`
  uses per element) instead of leaving it for the first hash-map/Z-set
  insertion to compute lazily via `Value::Hash()` — a restored row becomes
  a hash-map key immediately (aggregate `states_`, join `Index`/
  `RowWeights`, the sink `DuckDBZSet`), and the lazy path's per-value
  temporary-`Vector` cost otherwise dominates restore time (cold/restore
  attack, Task 3: ~51.5x on a 58k-group aggregate, see CHANGELOG).

### Lifetime

- Planner-frontend views hold an internal DuckDB Connection and therefore
  **pin the DatabaseInstance** while they exist. This is required: their
  expression-executor buffers come from the instance's allocator, so a view
  must never outlive its instance.
- **Qualified table keys (Phase D2)**: tracked tables are keyed by
  canonical `catalog.schema.table` derived from bound catalog entries, so
  views can source tables in attached databases and same-name tables in
  two catalogs stay distinct. `dbsp_qualified_name.hpp` holds the
  resolution utilities; temp-catalog entries keep bare names for the
  views-on-views TEMP-shadow mechanism.
- **Per-instance managers (Phase D1)**: `CDCManagerRegistry` keys one
  `CDCManager` per `DatabaseInstance`; `get_cdc_manager(context)` resolves
  through the closing/calling context. Two databases open in one process
  have fully isolated views and tracked tables.
- **Automatic release on last close**: the extension's `OnConnectionClosed`
  callback (with `InstanceRegistry` distinguishing DBSP-internal connections
  from user ones) detects when the last user connection to an instance
  closes and `take()`s that instance's manager out of the registry
  (atomic single-flight), destroying it on a detached thread — inline
  destruction would deadlock, since destroying a view's Connection
  re-enters `ConnectionManager::RemoveConnection`. Without this, a
  same-process close + reopen of the same database file spins forever in
  `DBInstanceCache::GetInstanceInternal`. Views are in-memory state, so
  they are gone after the last close by design; use `dbsp_save`/`dbsp_load`
  to carry definitions across sessions. Set `DBSP_DEBUG_TEARDOWN=1` to
  trace the decision on stderr.
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
├── dbsp_context_state.hpp       # Auto-sync hooks + captured-delta paths
├── dbsp_write_capture.hpp       # UPDATE/DELETE capture vetting + SQL builder
├── dbsp_duckdb_types.hpp        # DuckDB-native Z-sets and views
└── dbsp_plan_translator.hpp     # Planner frontend + circuit-IR optimizer
```

Build layout:
```
duckDBSP/
├── CMakeLists.txt         # Build configuration
└── build.sh               # Build script
```

# Changelog

## Phase K2: Spilled shared join arrangements - Jul 2026

- dbsp_spill(true) now also moves shared join arrangements (the largest
  random-probe state) to disk: buckets live in an append-only log, RAM
  keeps a key-digest -> (offset, length) slot map plus an LRU cache of
  hot deserialized buckets. Probes hit the cache or cost one disk read;
  updates merge + append at the log tail; the log compacts (rewrite +
  atomic rename) when it exceeds 2x the live payload. Join nodes probe
  through per-node scratch buckets — thread-private under I2 parallel
  propagation — while the arrangement serializes its own cache with an
  internal mutex (TSAN-verified with 4 views probing one spilled
  arrangement concurrently). Toggling migrates live arrangements both
  directions (unspill re-derives bucket keys through the arrangement's
  own key evaluators). Weights/counter maps stay in RAM: shared sides
  never self-pad, so they are empty or tiny. Aggregate states and
  embedded sort/window views remain RAM-resident (TODO).

## Phase K1: Disk-backed table baselines - Jul 2026

- dbsp_spill(true) moves tracked-table baselines (the engine's largest
  RAM consumer: a full copy of every tracked table kept for
  scan-and-diff) to on-disk record logs. RAM keeps a 128-bit row-digest
  index (~40 bytes/row). Scan-diff compares digest indexes; deleted-row
  payloads are read back from the old generation, added rows are in
  hand from the scan; files swap atomically (tmp+rename). View init and
  arrangement backfill stream baselines in 64k-row chunks — the whole
  table never materializes in RAM. Captured-delta commits append to the
  live log; net-zero records compact on the next full rebuild. Toggling
  migrates live baselines both directions. Crash-torn files are simply
  discarded — DuckDB storage remains the only durable source and
  recovery resyncs. Measured (200k rows): maxrss 259 -> 132 MB; first
  sync 170 -> 712 ms, incremental resync 210 -> 287 ms (Value
  serialization is the cost — hence opt-in).

## Phase J2: Order-sensitive aggregates - Jul 2026

- string_agg (+ group_concat/listagg aliases) and array_agg (+ list
  alias) with a REQUIRED ORDER BY inside the aggregate. Per group the
  node keeps (order keys, value) entries sorted by the declared keys
  (direction and null placement honored) with the value as a
  deterministic tiebreak; any group change re-renders the aggregate.
  string_agg skips NULL values, array_agg keeps them; custom separators
  read from DuckDB's bind data (the binder erases the separator
  argument). FILTER combines freely. Unordered string_agg/array_agg
  remain DBSP-E110 — DuckDB's scan order is unreproducible after
  incremental deletes/reinserts — with the error suggesting the ORDER BY
  rewrite. Ties on the order keys are broken by value, which can differ
  from DuckDB's input-order tiebreak; use unique order keys for exact
  parity.

## Phase J: Grouping sets & aggregate modifiers - Jul 2026

- ROLLUP / CUBE / GROUPING SETS: the planner translates N grouping sets
  into N incremental aggregate branches over the input (excluded group
  columns padded with typed NULLs, GROUPING() emitted as a per-branch
  constant bitmask) combined with UNION ALL. Each branch maintains
  incrementally, so the construct costs N aggregate updates per delta.
- FILTER (WHERE ...) on aggregates: per-aggregate predicate evaluated in
  the shared batch; failing rows contribute nothing to that aggregate
  (symmetric for inserts and deletes).
- DISTINCT aggregates: COUNT/SUM/AVG(DISTINCT x) maintain a per-group
  value-weight map; contributions fire on presence transitions (value
  appears / last copy vanishes). MIN/MAX(DISTINCT) are the plain
  aggregates (duplicates never move an extreme).
- ORDER BY inside order-insensitive aggregates (SUM/COUNT/AVG/MIN/MAX)
  is accepted and ignored; first()/order-sensitive functions still
  reject it.

## Phase I2: Parallel view propagation - Jul 2026

- Same-level views step concurrently: propagate_changes groups the
  topological order into dependency levels; views within a level share no
  path, so their circuits run on threads (opt-in via dbsp_parallel(true),
  which also controls parallel multi-table sync). Everything a stepping
  view reads is frozen for the level — pending deltas from earlier
  levels, shared arrangements (updated before views step and between
  levels), and its own private state. Results publish sequentially in
  stable order. Levels with under 256 input rows stay sequential (thread
  spawn would cost more than it saves). New dbsp_parallel() table
  function exposes the toggle. TSAN-clean.

## Phase I: Shared join arrangements - Jul 2026

- I1b both-sides sharing: a join may now share BOTH sides. With both
  arrangements post-delta when the node steps, the delta rule becomes
  Δl⋈R_new + L_new⋈Δr − Δl⋈Δr (the cross term flips sign instead of
  dropping). Initialization skips only the RIGHT side's replay: the left
  side's full replay joined against the backfilled right arrangement
  bootstraps the complete join, so no double-count is possible. Self-
  padding exclusions and single-reference rules unchanged.
- I1 shared join arrangements (one-side v1): join sides that are bare
  table scans (SOURCE or MAP_COLS(SOURCE), referenced once in the view)
  read a CDC-owned `SharedArrangement` instead of integrating a private
  index copy. One arrangement per (table, projection, key-exprs, flags)
  fingerprint serves every matching join side across views: N views over
  the same table cost one index update per delta instead of N, and one
  index in memory instead of N. Arrangements are updated before views
  step (the join drops its Δl⋈Δr term to compensate: Δl⋈R_new =
  Δl⋈R_old + Δl⋈Δr), and view initialization skips replaying a shared
  table (Δother ⋈ full arrangement reproduces the full join). Self-padding
  sides (right of RIGHT/FULL, left of LEFT/FULL/MARK) are excluded — init
  replay skip would lose their unmatched-row pads. Registry holds weak
  refs; consuming join nodes own the arrangement, so dropping the last
  view frees it. Bench: 8 views, 2k-row delta into a 20k-row probe side —
  55.0ms/sync shared vs 68.5ms with 8 private arrangements.

## Phase H: Sync scoping, dense Z-sets, batch keys - Jul 2026

- H1 touched-table sync scoping: auto-sync commits sync only the tables
  the transaction wrote (statement classification at QueryBegin covers
  autocommit; Appender/multi-statement/unparseable fall back to full
  sync; read-only commits skip sync entirely). Sync cost now scales with
  touched tables, not tracked tables.
- H2 guard statement cache: captured-commit COUNT(*) via cached prepared
  statements. Finding: the guard was not the cost — a captured commit is
  ~620us against a ~180us bare-transaction floor.
- H3 dense flat-map Z-set storage: contiguous live entries (O(size)
  iteration), generation-stamped index (O(1) clear), swap-remove erase
  with position-based index repair. Found and fixed a latent G1 bug:
  ColumnVec's implicit move left moved-from objects with a stale "valid"
  hash cache. Property tests joined the suite. Filter +27%, aggregate
  +27%, join +19%.
- H4 batch key extraction: joins materialize each delta's keys once per
  step (probes + integration reuse them; integration used to re-evaluate
  every key); aggregates batch keys/args. Join delta +21%.
- H5 streaming sync scans: SendQuery instead of materializing the whole
  table per scan-diff sync.
- H6' copy-on-write rows: ColumnVec holds a shared payload — copying a
  row between Z-sets, indexes and sinks is a refcount bump instead of N
  Value copies; mutation clones shared payloads (immutable-once-shared);
  the hash cache is an atomic in the payload (0 = unknown), shared by all
  copies and safe under concurrent readers; row equality gets a
  payload-identity shortcut. Hot row builders construct plain vectors and
  assign() them in one shot (the first COW attempt regressed build-heavy
  paths through per-push mutate checks). Filter 932k -> 1.05M rows/s,
  join 439k -> 487k, sync 42.6 -> 40.9ms, copy-heavy baseline 3.5x.
  This supersedes the byte-encoding H6 design: typed-Value consumers
  (sort comparators, pads, keys) made bytes-first storage a
  materialize-everywhere trap; sharing beats re-encoding here.
- Cumulative since the 173ms start: scan sync 40.9ms (4.2x), captured
  commit 0.62ms, join 140k->487k rows/s, filter 259k->1.05M, aggregate
  770k->2.27M.


## Phase G: Row-hash caching & transaction capture - Jul 2026

### G2: Captured-delta sync for explicit-transaction appends (Jul 5, 2026)
- Pure-INSERT explicit transactions on tracked tables sync in O(delta):
  QueryEnd (transaction still alive) classifies each statement via the
  parser and scans the transaction's LocalStorage for rows appended since
  the last statement (AddedRows watermark); at commit a COUNT(*) guard
  validates captured vs committed and the deltas feed straight into
  propagation. 636us/commit vs 47ms scan-diff on a 50k-row 3-level chain
  (74x); the bench shows 20/20 commits on the fast path.
- Everything uncapturable falls back to scan-and-diff by construction:
  autocommit (probed: the transaction object is already gutted at every
  extension hook), DELETE/UPDATE/upsert/multi-statement/unparseable
  statements (transaction poisoned), guard mismatches, capture
  exceptions. Correctness never depends on capture; a randomized churn
  test mixes clean commits, poisoned commits, and rollbacks.

### G1: Cached row hashes via ColumnVec (Jul 5, 2026)
- DuckDBRow::columns is now ColumnVec: const API passes through, every
  mutating operation invalidates a lazily cached hash, copies keep it —
  a row hashed once carries its hash through every Z-set and index. The
  entire codebase compiled unchanged. Full sync 119ms -> 52.6ms; join
  delta 265k -> 306k rows/s; fused filter 644k -> 732k rows/s.

## Phase F (started): Sync-path cost - Jul 2026

### F1: Scan-and-diff sync constant factors (Jul 5, 2026)
- Typed chunk extraction in sync_table_scan_and_consume (flatten once,
  direct FlatVector reads for INT/BIGINT/DOUBLE/VARCHAR instead of
  per-cell GetValue boxing).
- The freshly scanned state replaces the tracked-table baseline in one
  move (TrackedTable::replace_state) instead of replaying the computed
  delta row by row — the old path repeated every hash operation the diff
  had already paid for.
- TrackedTable's change_log_ deleted: appended on every change, read by
  nothing — a per-row cost and an unbounded memory leak in long-running
  processes.
- 3-level-chain bench: full sync 173ms → 119ms per row on a 50k-row
  table; propagate-only unchanged at ~25µs. Remaining floor is two O(n)
  hash passes; the designed fix (transaction-local capture at the
  pre-Finalize commit hook) is recorded in TODO.md.

## Phase E: Incremental Cascades & Correlated Subqueries - Jul 2026

### E1: Incremental view-on-view cascades (Jul 5, 2026)
- propagate_changes no longer resets cascaded views and re-applies full
  source state; a single topological pass carries pending deltas from
  each updated source into its dependents (owned unions for multi-source
  rounds, borrowed get_delta() otherwise). Propagating one row through a
  3-level chain over 50k rows: ~29µs (was two full 50k-row view
  recomputes). Scan-and-diff sync (~173ms) is now the dominant end-to-end
  cost — recorded in TODO.md (row-level CDC is the path).

### E2: Correlated subqueries (Jul 5, 2026)
- DELIM_JOIN translates: incremental DISTINCT of the outer side's
  correlated columns feeds every DELIM_GET through a shared output; the
  subplan translates as ordinary operators; the join back uses null-safe
  (IS NOT DISTINCT FROM) keys with SINGLE mapped onto LEFT-join padding
  and MARK onto the mark machinery. Correlated scalar / EXISTS /
  NOT EXISTS all differential-tested. Plain joins accept
  IS NOT DISTINCT FROM too (mixed null-safe + plain keys rejected).

### E3: Row-hash caching — DEFERRED
- Every safe design either loses the win (cache reset on copy) or risks
  stale hashes and silent Z-set corruption. The sound version is a
  compiler-enforced encapsulation of DuckDBRow::columns (~350 sites) —
  deferred to a dedicated change. Recorded in TODO.md.

## Phase D: Vectorized Eval, Outer Joins, Subqueries - Jul 2026

### D1: Vectorized node evaluation + zero-copy circuit deltas (Jul 5, 2026)
- Filter/map/fused nodes evaluate expressions over shared 2048-row
  DataChunks (BatchEvaluator: typed column fill/read fast paths,
  SelectionVector + Slice for filter survivors); sources and sinks borrow
  the caller's deltas instead of copying/rehashing them.
- bench_planner_eval: fused filter 259k→644k rows/s, aggregate
  770k→1.88M, join delta 140k→265k; overhead vs hand lambda 4.6×→2.4×.
  Remaining gap is Z-set insert hashing, not expression evaluation.

### D2: Outer joins (Jul 5, 2026)
- PlanJoinNode supports LEFT/RIGHT/FULL: padded sides track per-row total
  weights (incl. NULL-key rows) and reconcile NULL pads per affected row
  after each delta (desired pad = row weight when its weighted count of
  residual-passing matches is zero). Randomized differentials for all
  three types plus residual-participating matching.

### D3: MARK joins + FIRST aggregate (Jul 5, 2026)
- IN / NOT IN subqueries translate via left-preserving MARK joins with
  three-valued null-aware marks (subquery-side emptiness / NULL-presence
  transitions flip unmatched marks in bulk).
- first()/arbitrary() aggregate implemented, unlocking uncorrelated
  scalar subquery comparisons (val > (SELECT AVG(...))).
- Correlated subqueries (DELIM_JOIN) remain DBSP-E110.

## Pre-Phase-D: Recovery correctness - Jul 2026

### Checkpoint/WAL subsystem deleted (Jul 5, 2026)
- Follow-through on the replay-based recovery fix: once checkpoint contents
  were validated-but-never-applied and WAL replay was skipped, the entire
  subsystem was write-only overhead. Deleted: dbsp_checkpoint_format.*,
  dbsp_wal_manager.*, save/validate/get_latest/cleanup checkpoint APIs, the
  WAL flush in the commit hook, and the phase-3 WAL test binary.
- Recovery is now exactly: crash markers → _dbsp_views definitions →
  create_view replay of committed DuckDB storage → resync safety pass.
- load_views no longer counts failed view recreations as recovered; it
  logs CDCManager::last_error() per failure.
- Phase-2 tests rewritten as replay-restore tests (filter content,
  aggregate-after-delta, ordered-view scan, cross-restart e2e with crash
  markers). Suite: 31/31 green ×3.

### Checkpoint restore rebuilt around replay (Jul 5, 2026)
- Audit found checkpoint restore broken four ways: values deserialized as
  VARCHAR (never equal to live rows, so Z-set retractions could not
  cancel), partial checkpoint loads followed by a full resync doubled
  state, table baselines read from the checkpoint were discarded, and
  set_result() filled only view sinks while every internal circuit-node
  state (aggregate groups, join indexes, sort/limit multisets, recursive
  dedup) stayed empty — first post-restore delta computed garbage.
- Fix: recovery no longer applies checkpoint contents at all. DuckDB's
  committed storage is the single durable source of truth — load_views
  already rebuilds each view by replaying tracked-table scans through its
  circuit, which restores sinks AND internal node state consistently.
  load_checkpoint became validate_checkpoint (parse + checksum,
  diagnostics only); CDCManager::restore_view_state deleted; WAL
  table-delta replay skipped during recovery (it double-applied onto
  baselines already scanned from committed storage — rows absent from
  DuckDB storage after a crash were never committed and must not be
  resurrected). replay_wal() itself remains for callers managing their own
  baselines.
- read_value now casts values back to the written type where the bare type
  id suffices (parameterized types stay strings; contents are validated,
  never applied).
- New regression tests ([restore_audit]): aggregate view stays correct
  through the first post-restore delta; ordered views still scan their
  rows after restore. 32/32 green ×3.

## Phase C: Planner Completion & Parser Retirement - Jul 2026

### C5: Bespoke SQL parser deleted (Jul 5, 2026)
- `dbsp_sql_parser.hpp` (parser + ViewFactory) and `dbsp_optimizer.hpp`
  (ParsedViewDef-based DBSPOptimizer) are gone; the planner frontend is the
  only frontend. Translation failure is now a user-visible DBSP-E110 (or
  binder) error instead of a silent fallback — which also kills the old
  parser bug that silently dropped OR filters.
- Pre-deletion inventory gate (suite run with fallback disabled) found and
  closed three real gaps: views-on-views (MVs now shadowed as empty TEMP
  tables on the internal connection during ExtractPlan so DuckDB's binder
  can bind them), COUNT(*)-only scans (virtual rowid columns scan as NULL),
  and SUM over DECIMAL (exact 128-bit unscaled accumulation; AVG(DECIMAL)
  uses the double path, matching DuckDB's DOUBLE return type).
- `dbsp_use_planner()` stays callable as a backwards-compatible no-op that
  always reports ENABLED.
- Deleted with the parser: parser-only unit tests (sql_parser, optimizer,
  order_limit_parser, set_ops_parser, parser_errors, cte_subquery) and
  dead classes nothing referenced anymore (`NativeFilterProjectView`,
  `NativeRecursiveView`/dbsp_recursive.hpp — replaced by C4's fused
  FILTER_MAP node and C2's PlanRecursiveNode respectively).
- Suite: 32/32 green ×3 (was 37 test binaries; 5 parser-test binaries
  removed, all remaining coverage intact or migrated).

### C4: Optimizer ported to circuit IR (Jul 5, 2026)
- `plan_ir::optimize` rewrites the PlanOpSpec tree before circuit
  construction: adjacent-filter merge, single-side filter pushdown below
  joins (shrinks join index state; right-side predicate copies live in
  PlanKeepAlive::rewritten_exprs), and MAP(FILTER(x)) fusion into one
  PlanFilterMapNode. Projection pruning intentionally not ported — DuckDB's
  binder already prunes via GET column_ids.

### C1–C3: Planner gaps closed (Jul 5, 2026)
- C1 ORDER BY/LIMIT: folded (with trailing column-ref projections) into
  NativeSortView/NativeLimitView behind EmbeddedViewNode; a root sort/limit
  drives dbsp_query scan order, so ordered views return ordered rows.
- C2 WITH RECURSIVE: PlanRecursiveNode fixed-point driver over a nested
  PlannedCircuitView; UNION dedup state persists across deltas and
  multi-table recursive steps (e.g. transitive closure joining an edge
  table) now work — both beyond what the parser supported.
- C3 DISTINCT ON: NativeDistinctOnView behind EmbeddedViewNode; winner-pick
  order from the DISTINCT node's own order modifier. Also fixed a latent
  NULL-comparison crash in NativeDistinctOnView's comparator.

## Phase B5: Planner Frontend Default ON - Jul 2026

### B5: Planner becomes the default frontend (Jul 4, 2026)
- `dbsp_use_planner` now defaults to ON: every view first translates
  through DuckDB's binder/planner; the bespoke parser handles what the
  planner rejects (ORDER BY/LIMIT, recursive CTEs, plus anything DBSP-E110).
  `dbsp_use_planner(false)` restores the old behavior entirely.
- Full suite green both ways: with the default OFF (previous commit) and
  ON (this commit) — 37/37.
- Flipping the default exposed two tests coded to parser quirks, both
  fixed: the pure non-equi JOIN test only ever passed through its error
  path (parser requires an equality condition; planner supports the join,
  and the test was missing a dbsp_sync), and the window RANGE test indexed
  a `val` column the parser leaked into window view output (planner honors
  the SQL projection exactly; test now reads the last column).
- Deviation from the plan: the bespoke parser extraction paths are NOT
  deleted yet. The planner deliberately defers ORDER BY/LIMIT and
  recursion to the parser, so deleting it would remove working features.
  Deletion moves to when those gaps close (Phase C or a dedicated
  follow-up).
- Lifetime issue RESOLVED (follow-up commit): CDCManager is now a
  deliberately leaked heap singleton, so no view destructors run during
  static teardown at process exit. Planner views intentionally pin the
  DatabaseInstance while alive (they must not outlive it — executor
  buffers come from the instance's allocator); the exit segfault came
  purely from the singleton's destructor running DuckDB shutdown at
  static-destruction time. Verified: 0/20 crashes with the test-harness
  workaround disabled (was ~50%). True instance-scoped ownership is
  impossible without a reference cycle, so "views pin the instance" is
  the documented semantic; drop_view/reset release it while running.

## Phase B4: Planner Frontend Windows + CTEs - Jul 2026

### B4: Window functions and CTEs through the planner (Jul 4, 2026)
- LOGICAL_WINDOW translated by mapping BoundWindowExpressions onto the
  proven `NativeWindowView` (ROW_NUMBER, RANK, DENSE_RANK, NTILE, LAG,
  LEAD, FIRST/LAST/NTH_VALUE, SUM/COUNT/AVG/MIN/MAX OVER), run mid-circuit
  via the new `EmbeddedViewNode` adapter. Column-ref partitions/orders/args
  and constant offsets/frames; anything fancier falls back.
- Non-recursive CTEs: with the optimizer disabled DuckDB emits
  LOGICAL_MATERIALIZED_CTE + LOGICAL_CTE_REF (no inlining) — the definition
  subtree is built once and shared by all references.
- Correlated subqueries (DELIM_JOIN) and recursive CTEs rejected with
  explicit DBSP-E110 messages; both fall back transparently.
- `dbsp_create_view` now accepts SQL starting with WITH (non-recursive).
  WITH RECURSIVE stays rejected at that entry point: enabling it exposed a
  latent parser-path bug (rows duplicated because view initialization
  applies the base table once per reference) — tracked separately.
- Differential tests: ROW_NUMBER/RANK/LAG windows, CTE single + double
  reference (self-join through the CTE). 37/37 tests green.

## Phase B3: Planner Frontend Joins, Distinct, Set Ops - Jul 2026

### B3: Multi-source plans through the planner (Jul 4, 2026)
- `PlannedCircuitView` generalized from a linear single-source pipeline to a
  multi-source operator tree: one SourceNode per base table, shared across
  subtrees (self-joins work), translated recursively from the logical plan.
- `PlanJoinNode`: incremental inner join, bilinear delta rule
  (Δl⋈R + L⋈Δr + Δl⋈Δr). Equi-keys from EQUAL join conditions (expression
  keys included), residual comparisons (>, <, >=, <=, <>) checked per
  candidate pair, NULL keys never match, no conditions = cross product.
- `PlanDistinctNode`: multiplicity tracking, emits ±1 on 0↔positive edges.
- `PlanSetOpNode`: UNION ALL / UNION (n-ary) and INTERSECT [ALL] /
  EXCEPT [ALL] (binary) via per-input multiplicity state.
- View schema column names deduplicated (t.val + u.val → val, val_1) so
  join results stay queryable through dbsp_query.
- Not translated (falls back): outer/semi/anti/mark joins, DELIM joins,
  ORDER BY / LIMIT (deliberate — parser path already handles them; wrapping
  presentation nodes adds no user value before B5).
- Differential tests: equi-join, residual join, join+aggregate, cross join,
  DISTINCT, all four set ops, randomized two-table mutation rounds.
  37/37 tests green.

## Phase B2: Planner Frontend Aggregation - Jul 2026

### B2: Aggregation through the planner (Jul 4, 2026)
- `PlanAggregateNode`: incremental GROUP BY with multiple aggregates per
  view (COUNT(*)/COUNT/SUM/AVG/MIN/MAX), expression group keys (e.g.
  `GROUP BY val % 3`), and expression aggregate arguments. Accumulators:
  int64/double sums, `multiset<Value>` for MIN/MAX (any orderable type).
- HAVING comes free: the planner emits it as a FILTER above the aggregate,
  which the existing FILTER translation already handles.
- Global aggregates (no GROUP BY) keep exactly one result row — including
  on an empty table (COUNT=0, SUM/AVG NULL), matching DuckDB semantics the
  bespoke parser never had.
- Not translated (falls back): DISTINCT aggregates, FILTER clauses,
  ORDER BY in aggregates, ROLLUP/CUBE/GROUPING SETS, DECIMAL SUM/AVG.
- Differential tests: multi-aggregate, expression keys, HAVING, global
  aggregate incl. delete-to-empty; randomized rounds now inject NULLs.
  37/37 tests green.

## Phase B1: Planner Frontend Skeleton - Jul 2026

### B1: DuckDB Planner as Frontend — scan/filter/project (Jul 4, 2026)
- New `dbsp_plan_translator.hpp`: view SQL parsed/bound/planned by DuckDB
  itself (`Connection::ExtractPlan` on an internal connection, optimizer
  disabled for canonical plan shapes). `LOGICAL_GET → LOGICAL_FILTER →
  LOGICAL_PROJECTION` chains translate to circuit nodes
  (`PlannedCircuitView`); bound expressions evaluate row-at-a-time via
  `ExpressionExecutor` — arbitrary expressions, function calls, and mixed
  AND/OR predicates now work (the bespoke parser silently dropped OR
  filters).
- Feature flag `dbsp_use_planner(true/false)` (default OFF). Unsupported
  plans yield DBSP-E110 internally and fall back to the bespoke parser
  transparently.
- Differential test harness (`test/integration/test_planner_frontend.cpp`):
  view result == direct DuckDB query result after every delta batch across
  randomized insert/delete sequences. 37/37 tests green.

## DuckDB 1.5.4 Upgrade, Deadlock Fix & Phase A Start - Jul 2026

### Engine Upgrade & v2 Extension API (Jul 4, 2026)
- DuckDB pinned v1.4.0 → v1.5.4; entry point via `DUCKDB_CPP_EXTENSION_ENTRY`,
  registration via `ExtensionLoader` (legacy `dbsp_init`/`dbsp_version` removed).
- Adapted to 1.5 API: n-ary `SetOperationNode`, `ParserExtension::Register`,
  `ExtensionCallback::Register`, split `duckdb_generated_extension_loader` link.

### Integration Test Deadlock Fixed (Jul 4, 2026)
- Root cause: same-thread `struct_mutex_` relock — internal helper connection
  triggered first-time recovery from inside a sync holding the lock.
- `InternalQueryGuard` (thread-local) marks internal queries; hooks and
  recovery skip them. `auto_sync_enabled_` now atomic. Nested `context.Query()`
  replaced with fresh connections.
- Commit-hook sync scans committed state via fresh connection (raw storage
  scan with the just-committed transaction sees no rows in 1.5).
- Result: 36/36 tests green in ~4s (previously 10 tests hung at 120s timeouts
  since February). Also fixed: recursive subquery detection, stale ORDER BY
  error test, checkpoint tests saving while recovery disabled.

### Phase A: Circuit IR Unification - COMPLETE (Jul 4, 2026)
All view execution now flows through the `dbsp::Circuit` substrate
(`dbsp_circuit_views.hpp`); verified 36/36 tests green.
- `DuckDBZSet` unified with generic `dbsp::ZSet<DuckDBRow, DuckDBRowHash>` —
  circuit nodes operate on production rows with no boundary conversion.
- Fine-grained circuit views: Filter (Source→Filter→Sink), Project
  (Source→Map→Sink), FilterProject (Source→Filter→Map→Sink), Aggregate
  (Source→RowAggregateNode→Sink; group state + MIN/MAX multiset live in the
  node, the sink integrates emitted retract/emit deltas).
- Opaque circuit nodes via `WrappedViewNode`/`CircuitWrappedView`: Join,
  Distinct, Sort, Limit, Window, DistinctOn — proven state logic reused
  verbatim, to be decomposed into fine-grained nodes later.
- SetOp / Recursive / CTE remain combinators composing circuit-backed views.
- `SinkNode::set_materialized` added for checkpoint restore.
- Deferred to Phase C: porting DBSPOptimizer's 4 passes from ParsedViewDef
  rewrites to IR graph rewrites (they run pre-construction and still work
  unchanged; rewriting them belongs with naive-circuit construction).

## Phase 5: Advanced SQL & Automation - Completed Feb 2026

### P5.3: Subqueries & Non-recursive CTEs
**Completion Date**: Feb 7, 2026
**Description**: Parser support for non-recursive CTEs and subqueries in FROM.
- Handle DuckDB `CTE_NODE` wrapper for non-recursive CTEs.
- Support multiple CTEs via both CTE_NODE unwrapping and cte_map fallback.
- Parse subqueries in FROM clause as derived tables.
- CDCManager creates intermediate views for CTEs and derived tables.
- Added 5 test cases with 21 assertions.

### P5.1: Advanced Window Functions
**Completion Date**: Feb 7, 2026
**Description**: Support for LAG, LEAD, FIRST_VALUE, LAST_VALUE, NTH_VALUE, NTILE, and custom ROWS BETWEEN frame bounds.
- Implemented `LAG()` and `LEAD()` with custom offsets in `NativeWindowView`.
- Implemented `FIRST_VALUE()`, `LAST_VALUE()`, `NTH_VALUE()`, `NTILE()`.
- Implemented support for `PRECEDING` and `FOLLOWING` offsets in frames.
- Added comprehensive tests (10 test cases, 41 assertions).

## Phase 4: Advanced Features - Completed Feb 2026

### P4.3: Window Functions and Streaming Aggregates
**Completion Date**: Feb 8, 2026
**Description**: Support window functions for time-based and row-based windows.
- Row-based windows work correctly.
- PARTITION BY works.
- ORDER BY within window works.
- Incremental maintenance of windows.
- Standard window functions (RANK, DENSE_RANK, ROW_NUMBER, LAG, LEAD).

### P4.2: ORDER BY and LIMIT Support
**Completion Date**: Feb 8, 2026
**Description**: Implement ORDER BY and LIMIT/OFFSET for sorted, paginated results.
- Implemented incremental sort using O(n log n) logic.
- Implemented LIMIT/OFFSET support.
- Fixed `ORDER BY` default direction (ASC).

## Phase 3: Production Readiness - Completed Feb 2026

### P3.4: Circuit Optimization Pass
**Completion Date**: Feb 7, 2026
**Description**: Implement circuit optimization passes for operator fusion and pushdown.
- Filter Pushdown: Move filters closer to data sources.
- Projection Pushdown: Eliminate unused columns early.
- Operator Fusion: Combine multiple map/filter operations.

### P3.2: Reader-Writer Locks for Concurrency
**Completion Date**: Feb 7, 2026
**Description**: Replace global mutex with reader-writer locks for concurrent queries.
- Replaced `std::mutex` with `std::shared_mutex` in `CDCManager`.
- Updated lock usage: shared locks for reads, exclusive locks for writes.
- Verified no data races with ThreadSanitizer.

### P3.1: Enhanced Error Messages and Diagnostics
**Completion Date**: Feb 7, 2026
**Description**: Improve error messages for unsupported features and edge cases.
- Rewrote error messages to include problem, workaround, and example.
- Added context (line numbers, table names).
- Created documentation pages for common errors.

## Phase 2: Core DBSP Completeness - Completed Feb 2026

### P2.4: Recursive Query Support
**Completion Date**: Feb 7, 2026
**Description**: Implement WITH RECURSIVE for transitive closures and recursive queries.
- Extended SQL parser for `WITH RECURSIVE`.
- Implemented stream introduction (δ₀) and elimination (∫).
- Created `RecursiveView` with fixed-point iteration.
- Validated transitive closure and hierarchy queries.

### P2.3: Support Complex JOIN Predicates
**Completion Date**: Feb 7, 2026
**Description**: Extend JOIN support from equality-only to complex predicates.
- Updated parser to handle compound `ON` clauses (AND-ed predicates).
- Updated `NativeJoinView` to evaluate complex predicates.
- Validated multi-column equality and non-equi joins.

### P2.2: Fix MIN/MAX Incremental Deletions
**Completion Date**: Feb 7, 2026
**Description**: Replace O(n) MIN/MAX deletion handling with O(log n) solution.
- Extended `AggState` to track all values using `std::multiset`.
- Implemented O(log n) insert/delete for MIN/MAX.
- Verified correctness and performance.

### P2.1: DISTINCT SQL Integration
**Completion Date**: Feb 7, 2026
**Description**: Expose existing DISTINCT operator to SQL parser.
- Updated SQL parser to detect `SELECT DISTINCT` syntax.
- Wired `DistinctView` in `CDCManager`.
- Verified duplicate removal and incremental updates.

## Phase 1: Foundation - Completed Feb 2026

### P1.3: Add MIN and MAX Aggregate Functions
**Completion Date**: Feb 6, 2026
**Description**: Implement MIN and MAX aggregate functions alongside SUM/COUNT/AVG.
- Extended `AggregateFunction` enum.
- Updated parser to recognize MIN/MAX.
- Implemented computation logic for initial data and updates.

### P1.2: Implement HAVING Clause for Aggregate Filtering
**Completion Date**: Feb 6, 2026
**Description**: Add HAVING clause support for filtering aggregated results.
- Extended `ParsedViewDef` to store HAVING filter.
- Updated `NativeAggregateView` to filter groups based on HAVING condition.
- Verified correct filtering and incremental updates.

### P1.1: Wire Parser Extension for DDL Syntax
**Completion Date**: Feb 6, 2026
**Description**: Complete parser extension integration to enable native SQL DDL syntax.
- Registered parser extension in `LoadInternal()`.
- Verified `CREATE MATERIALIZED VIEW`, `DROP`, `REFRESH` syntax.
- Added aliases `dbsp_drop` and `dbsp_drop_cascade`.

## Infrastructure & Migrations - Completed Feb 2026

### Extension Loading Infrastructure
**Date Completed**: Feb 5, 2026
- Fixed extension entry point naming.
- Fixed runtime deadlock issues.
- Integrated DuckDB metadata system.

### DependencyGraph Bug Fixes
**Date Completed**: Feb 5, 2026
- Fixed topological sort returning empty vector.
- Fixed transitive dependency tracking.

### Security Hardening
**Date Completed**: Feb 5, 2026
- Fixed null byte injection in `validate_filepath()`.
- Implemented cross-platform path validation.

### Test Infrastructure Setup
**Date Completed**: Feb 5, 2026
- Created CMake test configuration.
- Fixed Catch2 compatibility.
- Enabled CTest execution.

### DuckDB 1.4.0 Migration
**Date Completed**: Feb 5, 2026
- Upgraded to DuckDB v1.4.0 APIs.
- Updated build system and successfully compiled extension.

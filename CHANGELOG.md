# Changelog

## Perf: DP3a — MAP_COLS fusion + operator output-hash preseed - Jul 2026

- Per-node step profiling (DBSP_STEP_PROF) showed the filter-path cost was
  NOT the FILTER_MAP node (~15ms/100k) but a hidden plan_scan_cols
  projection (~72ms/100k): a per-row RowMap materializing GET's column
  permutation with lazily hashed Z-set output. New fuse_map_cols IR pass
  clones the consumer's bound expressions, remaps BOUND_REF leaves through
  the column selection, and deletes the node from FILTER_MAP/FILTER/MAP
  chains. PlanBatchNode and PlanAggregateNode now also pre-seed output-row
  and group-key hashes from their already-flat result vectors
  (fold_vector_hashes — same exact lazy-formula replication and equality
  gate as DP1).
- Filter view sync: 90ms -> 20ms per 100k rows (4.9M rows/s); overhead vs
  a hand-written lambda 13.4x -> 2.06x. DP3b (full chunk-through) demoted:
  at 2x residual it no longer clears its architectural cost.

## Perf: DP1 vectorized row hashing - Jul 2026

- Lazy per-Value row hashing was 68% of chunk->Z-set ingestion time
  (each Value::Hash constructs two Vectors internally).
  chunk_row_hashes() computes whole-chunk row hashes via one
  VectorOperations::Hash per column, replicating the lazy combine
  exactly (equality by construction — Value::Hash IS a 1-element
  VectorOperations::Hash — pinned across types/NULLs/nested/subsets by
  test_row_hash.cpp, 3.8k assertions). stream_table_rows pre-seeds row
  caches: ingestion 684->304 ms per 1M rows (2.25x), scan-path sync
  ~0.27 us/row. bench_dataplane records the cost split and gates the
  win. Roadmap for DP2-DP4 (batched key eval, late materialization,
  columnar state) in docs/DESIGN_DATA_PLANE.md.

## Feature: O(Δ) auto-sync capture for UPDATE and DELETE - Jul 2026

- Whitelisted UPDATE/DELETE statements on tracked tables now commit via
  captured deltas instead of scan-and-diff — for explicit transactions
  AND autocommit (the first captured autocommit path; the capture needs
  no transaction internals). Single-row UPDATE at 1M rows: ~1.5 ms
  end-to-end vs ~2.4 s scan-and-diff.
- Mechanism (design 1 of docs/DESIGN_WRITE_CAPTURE.md): at QueryBegin one
  internal SELECT reads the statement's old images and computes the new
  ones by projecting the SET expressions cast to column types — no
  version-chain access needed, which was the 1.5.4 blocker. Deltas enter
  the existing `apply_captured_delta` path (old −1 / new +1), merged with
  G2 INSERT captures per table at commit.
- Whitelist: plain base-table UPDATE (no FROM/RETURNING/CTE/DEFAULT) and
  DELETE (no USING), no subqueries or parameters in SET/WHERE, all
  functions CONSISTENT (now()/random() excluded), no indexed or LIST SET
  columns (DuckDB executes those as delete+re-append — rowids unstable),
  target not written earlier in the same transaction. Anything else —
  including any un-capturable statement mid-transaction — falls back to
  the scoped scan for the WHOLE transaction; captured and scanned deltas
  never mix.
- Commit guard generalized (the old COUNT(*) guard is UPDATE-blind):
  commit-sequence conflict detection (`CDCManager::commit_seq`), signed
  COUNT(*), and O(Δ) rowid re-verification of written rows (deleted
  rowids gone, updated rowids hold exactly the predicted post-image).
  Guard misses fall back loudly (`capture_guard_fallbacks` counter).
- Checkpoint/D3c interplay covered: deferred baselines still materialize
  at QueryBegin before any write; dbsp_save/dbsp_load round-trips with
  captured UPDATE/DELETE history (python checkpoint-restore test).
- Follow-up in the same release: autocommit INSERTs are captured too —
  VALUES lists AND deterministic SELECT sources are evaluated via one
  internal SELECT with the INSERT's own casts (~1.0 ms at 1M rows);
  partial column lists take their declared DEFAULTs (volatile defaults
  like nextval() fail the stability check and fall back). Guard is
  commit-seq + signed COUNT(*). SELECT sources with LIMIT/SAMPLE (row
  choice depends on scan order), table functions (no stability
  metadata), window functions, or CTEs fall back; explicit-txn INSERTs
  stay on G2 (exact there).
- Upserts captured too: `INSERT ... ON CONFLICT (cols) DO UPDATE SET`
  (excluded.-qualified) and `DO NOTHING` probe committed state with a
  LEFT JOIN from the row source; insert-part rows enter as +1, update-part
  rows as old/new pairs with rowid re-verification. Unqualified target
  columns in SET (ambiguous in the probe), conditional DO ... WHERE,
  OR REPLACE, implicit conflict targets, and SET on indexed columns fall
  back. Duplicate conflict keys inside a DO NOTHING source insert fewer
  rows than the capture predicts — the signed COUNT(*) guard catches that
  case and falls back, loudly.
- Subquery predicates in UPDATE/DELETE and `DELETE ... USING` (rewritten
  to a correlated EXISTS probe) are captured when the statement's view is
  pure committed state — autocommit, or an explicit transaction before
  its first write (a new wrote_any flag gates it; a prior write to ANY
  table would make the probe read stale state). Subquery row sources are
  vetted with the same repeatability rules as INSERT ... SELECT.
  UPDATE ... FROM stays scan-diff: with multiple FROM matches per target
  row the SET result is nondeterministic.
- Design-2 phase 1: a plan tee (OptimizerExtension + injected extension
  operator) captures ANY remaining DELETE shape from the rows the plan
  actually processed — prepared parameters, volatile predicates,
  post-write subqueries, USING over transaction-local state, repeated
  writes to one table. The DELETE child chain is widened to carry full
  old row images (through filters, projections, and join left sides);
  teed rows are exact, so no commit guard applies to them. Hook-ordering
  fix on the way: autocommit statements fold their sync scope at
  QueryBegin (the mid-statement commit hook cannot resolve catalog
  entries — that legacy fold path never actually worked), explicit-txn
  statements fold at QueryEnd so the tee can mark them captured first.
- Design-2 phase 2: the UPDATE tee. Same widening; the new image overlays
  the SET values (already computed in the child projection) onto the old
  image, closing UPDATE ... FROM, prepared parameters, volatile SET
  expressions, and indexed-column UPDATEs (update_is_del_and_insert —
  the tee needs no post-rowids). Multi-match UPDATE ... FROM produces two
  new images for one target row — ambiguous, the tee invalidates itself
  and the scan reconciles. Implementation findings: PhysicalUpdate takes
  the rowid from the LAST child column by position (the tee projects the
  widened columns back out), and a projection-pushdown LogicalGet exposes
  only projection_ids entries (appended columns must join the list).
  With this, every plain-SQL DML statement on a tracked table syncs in
  O(Δ) except multi-statement strings, Appender writes, and multi-match
  UPDATE ... FROM.
- Design-2 phase 3: the INSERT tee + engine-assumption canaries.
  Autocommit INSERTs tee their child rows as exact +1 appends (identity/
  permutation column maps only — DEFAULTs resolve in a physical
  projection above the tee, so a nextval() default declines rather than
  double-advancing; canary-tested). Closes LIMIT/SAMPLE/table-function
  INSERT sources and multi-statement DML strings. A new canary suite
  (test_engine_assumptions.cpp) pins every empirically probed engine
  behavior by name so engine upgrades fail readably instead of as view
  corruption; writing it surfaced that provably-empty DML predicates
  fold to LOGICAL_EMPTY_RESULT (tee declines, correctly) and that
  autocommit QueryEnd has no transaction (the mid-statement commit hook
  is the only post-execution point with one).
- Appender coverage confirmed + dbsp_stats(): probing for a planned
  COMMIT-time Appender sweep revealed the Appender's flush already runs
  through the statement hooks as a plain INSERT — G2's LocalStorage
  capture takes it, in explicit transactions and autocommit, including
  Appender-then-UPDATE ordering (probe declines via touched, tee captures
  with transaction-local rows visible). The long-standing 'Appender
  bypasses query hooks' note was wrong; tests now pin the real behavior
  and the planned sweep was discarded as dead code. New dbsp_stats()
  table function exposes captured/scan/guard-fallback/commit-seq counters
  to SQL. docs/UPSTREAM_PROPOSAL.md drafts the storage-hook RFC for
  duckdb/duckdb (referencing #12408), incorporating the validated
  call-site design from the shelved fork.
- No upstream help available: DuckDB through 1.5.x ships no CDC/changeset
  extension hook (discussion #12408 open); revisit on engine upgrade.

## Feature: D3c lazy baselines — watermark-matched restore reads zero source rows - Jul 2026

- `dbsp_load()`'s checkpoint fast path no longer rebuilds `TrackedTable`
  baselines or shared-arrangement state by re-scanning every source table.
  With the save-time watermark (COUNT + `bit_xor(hash(row))`) verified
  against live storage, the baseline is BY DEFINITION the committed table
  content — so tables auto-tracked during a fast-path load start
  **deferred** (`TrackedTable::mark_deferred`, watermark carried along),
  and arrangements registered over them start `needs_backfill`.
  Materialization happens on first need via one typed streaming scan
  (`stream_table_rows`, factored out of the sync path) + arrangement
  backfill from the fresh baseline.
- Persisting baselines at save was evaluated and REJECTED with evidence:
  the per-`Value` blob codec decodes at ~1.4 µs/row (13.6 ms for the 10k-row
  sink), extrapolating to ~1.4 s for 1M rows — slower than the scan it
  would replace, plus ~2x db-file bloat for content DuckDB already stores
  compressed. When the watermark matches, the table IS the baseline.
- Materialization triggers (all paths covered):
  - `QueryBegin` before any SQL write statement executes (runs even with
    auto-sync off while anything is deferred) — the only moment storage
    still equals the restore-time content exactly, which keeps
    SQL-write-then-notify hosts (NuEPM `UPDATE` → `dbsp_notify_delete` +
    `dbsp_notify_insert`) exact;
  - the notify path (`on_insert`/`on_delete`/`on_update`/
    `on_batch_insert`, now taking an optional `ClientContext *`) and the
    captured-delta path, with pending-delta subtraction (baseline :=
    scan − δ, then the normal apply lands it on storage) — first delta
    also materializes ALL deferred tables, since join propagation reads
    other tables' arrangements;
  - warm `create_view` init replay / arrangement registration;
  - scan-diff sync: a deferred table recheck is a ~ms watermark query —
    match ⇒ empty delta and the baseline STAYS deferred (recovery's
    post-crash resync is now near-free on unchanged tables).
- Safety: stale checkpoints at load are rejected exactly as before (0
  deferred sources). Post-restore out-of-band changes (a write that
  bypassed hooks and notify) are detected by the watermark/count guard;
  since the restore-time baseline is then unrecoverable, views schedule a
  full rebuild that runs at the next statement boundary
  (`rebuild_all_views`: refresh all baselines from committed storage,
  drop dependents-first, recreate from stored definitions) — correct,
  self-healing, slow only on the broken-contract path.
- `dbsp_load()` message now reports "N sources deferred"; the regression
  test asserts 2 on the fast path and 0 on the stale path, plus two new
  sessions: auto-sync SQL DML after a lazy restore (pre-write
  materialization) and out-of-band write + `dbsp_sync()` (rebuild
  self-heal).
- `LoadFunc` now calls `EnsureContextState` — previously a connection
  whose first DBSP call was `dbsp_load()` had no QueryBegin/commit hooks
  at all (recovery, auto-sync and the pre-write materialization silently
  skipped until some other dbsp_* function ran).
- Measured (test_checkpoint_restore, 1M rows, DBSP_TIMING=1): restore
  1.39 s → 0.045 s (~31x; ~90x vs cold build). The old restore window
  (baseline build ~0.95 s + arr_backfill ~0.38 s + blob_decode ~0.014 s)
  collapses to blob_decode ~0.015 s + watermark checks. First post-restore
  edit pays the deferred scan once (~1.2 s at 1M rows;
  `baseline_materialize` timing phase), steady-state edits unchanged.
- Timing attribution fix for the D3b plan numbers: the "source_sync
  ~1.08 s" bucket measured on restore was actually session-1 commit-hook
  syncs; the load-path cost was the UNTIMED `sync_table_internal` baseline
  build. `sync_table_scan_and_consume` now shares the typed scan helper,
  and materialization has its own `baseline_materialize` phase.

## Fix: D3b checkpointing never fired for planner-built views - Jul 2026

- Root cause: `checkpointable()`/`serialize_circuit_state()`/
  `restore_circuit_state()` were only overridden on
  `SingleSourceCircuitView`. `PlannedCircuitView` — every join/aggregate
  view since C5 made the planner the only frontend, i.e. the views D3b was
  built for — inherited the base-class `checkpointable() == false`, so
  `dbsp_save()` wrote watermarks but ZERO circuit rows and every load
  rebuilt by replay. The D3b regression test kept passing because full
  replay is equally correct and its speedup assert was direction-only
  (warm-cache rebuild beat cold build). Found via `DBSP_TIMING=1`
  instrumentation: no `blob_decode` phase ever ran.
- Fix 1: `PlannedCircuitView` now carries the same three overrides
  (walk `circuit_`, serialize/restore SERIALIZABLE nodes, refuse when any
  node is UNSUPPORTED).
- Fix 2: recovery's `load_views` routed through `load_from_duck_table` —
  the hand-rolled loop recreated views unconditionally, so first-query
  recovery in embedded hosts (the NuEPM pattern) ignored the checkpoint
  even where it existed. The recovery-only `create_view(name, sql,
  sources, ctx)` overload (which discarded `sources`) is deleted.
- Honest messages: `dbsp_save()` reports "circuit checkpoint: N views";
  `dbsp_load()` reports "M from checkpoint". The regression test now
  asserts both counts (1 on restore, 0 on stale) — the silent-failure
  mode cannot ship again.
- Measured (test_checkpoint_restore, 1M rows): restore 3.17s -> 1.32s,
  speedup vs cold build 3.1x. Post-fix restore breakdown via DBSP_TIMING:
  source sync ~1.08s, arrangement backfill ~0.36s, blob decode ~0.014s —
  source sync is now the dominant follow-up target (baseline
  persistence / columnar scan), not the blob codec.
- New: `DBSP_TIMING=1` env flag emits per-phase wall-clock lines
  (source_sync / arr_backfill / blob_decode) to stderr for restore
  profiling.

## Feature: incremental recursive deletion (DRed) - Jul 2026

- `WITH RECURSIVE ... UNION` views now maintain incrementally under
  DELETIONS via Delete-Rederive (DRed), replacing the previous full
  fixed-point recompute. Overdelete over-approximates the retraction
  (negative-frontier fixpoint); rederive restores rows that still have
  alternative support (`I(survivors)` image + `anchor_total_` re-seed +
  a bounded forward fixpoint), cycles included. Overdelete is O(affected
  subgraph); the whole path is strictly cheaper than the recompute it
  replaces.
- Correctness gate: a `DRed == oracle` differential test (targeted cases
  + a 200-round randomized insert/delete/mixed sequence) compares the
  incremental view against a direct DuckDB recursive query. ASAN clean.
- Mixed insert+delete deltas in one sync (an `UPDATE`, or batched DML
  before a single `dbsp_sync`) are handled correctly — an early cut
  admitted phantom rows here; fixed before ship.
- On the rare path where a DRed loop exhausts `max_iterations_`, the node
  restores its pre-delta state and falls back to `recompute()` (correct,
  self-healing) rather than silently desyncing.
- Scope: `UNION` (set-semantics) recursion. `UNION ALL` (multiplicity)
  deletion keeps the full recompute — weighted deletion in a cycle is
  ill-defined. This was the last non-incremental correctness hole.

## Fix: crash-marker hygiene - Jul 2026

- In-memory instances no longer create recovery markers at all: their view
  state dies with the process, so there is nothing to recover, and the old
  CWD fallback littered `./.dbsp_recovery` into the embedding process's
  working directory (e.g. an API server's repo root).
- Marker path resolution fixed: the old lookup asked for a database named
  `main` (DEFAULT_SCHEMA), which never matched, so file-backed databases
  also fell back to the CWD. Markers now live next to the database file.
- Clean shutdown releases the session lock when the last user connection
  closes (previously only a global static destructor removed it, which
  embedders' teardown never runs — every restart falsely claimed
  "previous session crashed").
- `DBSP_DEBUG_RECOVERY=1` prints the resolved db/recovery paths.
- Regression test: test/python/test_recovery_markers.py.

## Phase D3b: circuit-state checkpointing - Jul 2026

- dbsp_save() now also snapshots per-view operator state (aggregate
  group scalars, private INNER-join indexes) and sink results into
  _dbsp_ckpt, with per-source watermarks (COUNT + bit_xor(hash(row)))
  in _dbsp_ckpt_meta. dbsp_load() cold-creates covered views (sources
  tracked/synced and arrangements backfilled, but NO circuit replay)
  and injects the checkpointed state; post-restore incremental updates
  are exact. Stale checkpoints (source changed since save) fail the
  watermark check and fall back to a full rebuild.
- Scope: count/sum/avg aggregate family and INNER joins. Value-
  collecting aggregates (MIN/MAX/DISTINCT/ordered/quantiles), outer or
  mark joins, and spilled state are not checkpointed - those views
  rebuild by replay as before.
- Node API: dbsp::Node::state_kind()/serialize_state()/restore_state()
  (byte-level; the core circuit stays free of storage dependencies),
  NativeMaterializedView::checkpointable()/serialize_circuit_state()/
  restore_circuit_state(), CDCManager::save_checkpoint()/
  checkpoint_valid()/restore_view_state(), and create_view(...,
  skip_init_replay).
- Measured (1M-row join+SUM view): restore 3.3s vs rebuild 4.6s in the
  engine; in the NumPad host, second-session first edit dropped from
  25s to 10s. Remaining restore cost is source sync + arrangement
  backfill + blob decode - codec/arrangement follow-ups tracked in
  docs/PHASE_D_PLAN.md.
- Regression test: test/python/test_checkpoint_restore.py.

## Fix: recovery deadlock on second connection - Jul 2026

- Crash recovery ran inside OnConnectionOpened, which executes under
  ConnectionManager::connections_lock; recovery opens internal
  Connections whose constructors re-enter AddConnection on the same
  mutex — self-deadlock. Single-connection scripts never hit it (the
  callback registers at LOAD, after that connection was added), but
  embedded hosts opening a connection per operation hung on connection
  #2. Recovery now runs once at the first QueryBegin (no lock held).
  Regression test: test/python/test_recovery_no_deadlock.py.
- Recovery's view reload now skips views already live in the session
  instead of failing "View already exists" per view.

## Phase D3: view definitions persist in DuckDB tables - Jul 2026

- dbsp_save() / dbsp_load() (zero-arg) now work: view definitions are
  stored in a _dbsp_views table in the default catalog, so they travel
  with the database file and its copies/backups. Named-table forms
  dbsp_save('view', 'table') and dbsp_load('table') are honored, with
  identifier validation (restores absolute-path/traversal rejection).
  JSON file save/load unchanged.
- create_view's best-effort _dbsp_views insert now creates the table if
  missing, so crash recovery finds definitions without an explicit save.
- Planner naming fix: output column names now come from the prepared
  statement's binder names; positional GROUP BY (GROUP BY 1,2) no longer
  degrades group columns to "0","1" — at creation or after load.
- Scope note: persistence covers definitions; loading rebuilds view state
  from current table data (O(source)). O(state) restore requires circuit
  checkpointing — tracked as D3b in docs/PHASE_D_PLAN.md.
- Regression test: test/python/test_table_persistence.py.

## Phase D2: attached-catalog tables as view sources - Jul 2026

- Materialized views can now source tables in ATTACHed databases
  (CREATE MATERIALIZED VIEW ... FROM m.li_1). Tracked-table keys are
  canonical catalog.schema.table, derived from the bound plan's
  TableCatalogEntry — never from SQL text — so bare and qualified
  references key identically. Same-name tables in different catalogs no
  longer collide.
- All internal lookups/scans resolve through the catalog (any attached
  db) instead of hardcoded main-catalog + DEFAULT_SCHEMA; scan/guard SQL
  emits quoted qualified names; commit-hook write attribution
  canonicalizes parsed statement targets (two-part refs try schema-first
  then catalog-first, mirroring the binder).
- Autocommit callers (no active transaction) canonicalize via a textual
  fallback against the tracked-key map — starting a transaction inside
  executing queries deadlocks.
- DETACH with live views degrades gracefully: the view keeps its last
  state (queryable, droppable); syncs against the gone catalog fail with
  a clear error; everything else keeps working.
- Temp-catalog entries keep bare names: views-on-views bind through TEMP
  shadow tables whose names must match view keys.
- Regression test: test/python/test_attached_catalog.py. Debug tracing:
  DBSP_DEBUG_SYNC=1.

## Phase D4: dbsp_changes() delta read-back - Jul 2026

- New table function dbsp_changes('view'): rows added/removed by the
  view's most recent sync as (view columns..., weight BIGINT). Exposes the
  per-view last-step delta the propagation engine already retains
  (NativeMaterializedView::get_delta) via a locked CDCManager accessor
  (scan_view_delta). Single-generation buffer; aggregate updates on a
  surviving group arrive as (old,-1),(new,+1) pairs.
  Regression test: test/python/test_dbsp_changes.py.

## Phase D1: per-instance CDCManager - Jul 2026

- One CDCManager per DatabaseInstance (CDCManagerRegistry) instead of one
  process-wide singleton. get_cdc_manager() now requires a
  DatabaseInstance&/ClientContext&; two databases open in one process get
  isolated views and tracked tables (test/python/test_multi_instance.py).
- Last-user-connection teardown now take()s the instance's manager out of
  the registry (atomic single-flight) and destroys it on a detached thread.
- Known gap: crash recovery still runs once per process (static
  recovery_done in OnConnectionOpened), so persisted views auto-load only
  for the first instance opened; later instances need explicit dbsp_load.

## Fix: release instance state on last connection close - Jul 2026

- Same-process close + reopen of a database that had materialized views
  used to hang forever: views' internal Connections (PlanKeepAlive) pinned
  the DatabaseInstance, and DuckDB's DBInstanceCache busy-spins waiting for
  a dying instance. Now an OnConnectionClosed callback releases all CDC
  state on a detached thread when the last user connection closes (new
  InstanceRegistry tells DBSP-internal connections apart from user ones).
  Regression test: test/python/test_reopen_hang.py.
- Build fixes: test/CMakeLists.txt no longer re-adds the duckdb
  subdirectory under the root build; benchmark_runner target is skipped
  when its sources are absent.

## Zero-ceremony views: auto-sync ON by default - Jul 2026

- dbsp_auto_sync now defaults ON. Creating a view already auto-tracked
  and loaded its source tables; with auto-sync on by default, a
  materialized view keeps itself current on every commit with no
  dbsp_track or dbsp_sync calls at all: CREATE TABLE, CREATE
  MATERIALIZED VIEW, INSERT, SELECT - done. Explicit INSERT-only
  transactions take the O(delta) captured-delta path; other writes
  scan-and-diff scoped to touched tables.
- Bulk-load escape hatch documented: dbsp_auto_sync(false), load,
  dbsp_sync() once, re-enable.
- dbsp_track/dbsp_sync stay for manual workflows; docs and quickstart
  rewritten to drop the ceremony. Benchmarks disable auto-sync
  explicitly (they time manual syncs).

## Phase O4: Cross-projection arrangement sharing - Jul 2026

- Views needing DIFFERENT column subsets of the same join side now
  share one arrangement. Arrangements store full table rows; each
  consumer's key expressions are remapped into full-table column space
  (BoundReference index rewrite, clones pinned in PlanKeepAlive) so
  fingerprints canonicalize across projections, and each consumer
  projects bucket rows to its own shape at probe time. Bare-scan
  consumers (identity projection) keep the zero-copy hoisted fast path.
  Projection can collapse distinct full rows to equal projected rows -
  probe materialization ACCUMULATES weights (the differential caught an
  emplace that silently dropped the second copy). Keys reading virtual
  columns skip sharing.
- Perf gate (rollback condition): projected probes 55.7ms vs identity
  54.8ms per sync on the 8-view bench (~noise), vs 68.5ms when the same
  views needed 9 private arrangements - 9 -> 2 arrangements AND ~19%
  faster. Standard benches untouched. Kept.

## Phase N: RAM-state closeout - Jul 2026

Four items, individually committed for per-item rollback; default-path
benches verified unchanged after each (join 443k rows/s, aggregate
2.0M, propagate 14.7us/row - all within noise of the ledger).

- N1 grouping-set input sharing: ROLLUP/CUBE branches share ONE input
  subtree via the CTE machinery (synthetic index) instead of
  recomputing it per grouping set - CUBE(3) input work drops 8x.
- N2 bounded top-K (spill mode): constant-LIMIT sort views keep only
  offset+limit+margin rows in RAM; the rest lives in a disk record log
  and promotes back on window underflow (one log pass, margin absorbs
  normal churn). Kept set is provably identical to the full multiset's
  (same comparator, full-row tie-break). Percentage limits and plain
  ORDER BY keep full state.
- N3 local join index spill (spill mode): unshareable probe-target
  sides (filtered inputs, subqueries) put their private indexes in disk
  bucket logs - same self-padding exclusions as arrangement sharing.
- N4 oversized holistic groups (spill mode): a values multiset past
  65536 entries migrates to a disk log; touched groups reload on
  render. mode counts and ordered-aggregate entries stay in RAM.

Remaining RAM state after N: self-padding join sides' pad/weight/mark
structures, mode/ordered aggregate state, plain ORDER BY and window
views (their answer IS the total order), and cross-projection
arrangement duplication (excluded by decision - emit-path risk).

## Phase M: SQL-coverage leaves - Jul 2026

- M1 window PARTITION BY / ORDER BY / argument expressions: the
  translator projects non-column expressions into helper columns below
  the window (MAP -> WINDOW -> MAP sandwich; helper columns stripped
  above, user-visible layout unchanged). Removes the "use a plain
  column" rewrite footgun for every window function including window
  aggregates. Also fixed a latent NULL crash in the window view's peer
  comparison (raw Value != throws on NULL order keys - affected
  nullable plain columns too).
- M2 mad: median absolute deviation over the sorted per-group multiset
  (interpolated median, deviations merged sorted from the two halves).
  Numeric arguments only; temporal mad (DATE -> INTERVAL) stays E110.
- M3 LIMIT p PERCENT: the limit view recomputes its cutoff as
  trunc(p/100 * input rows) on every apply - matches DuckDB's
  truncation exactly and tracks table growth/shrinkage incrementally.
  Truly non-constant limits (expressions) remain E110.

Remaining E110 after M: USING KEY recursion, expression LIMIT,
approx_/reservoir_ quantiles + approx_top_k (approximate results cannot
match DuckDB's, mapping them to exact would silently differ), unordered
string_agg/array_agg, DISTINCT on holistic aggregates.

## Phase L: Holistic aggregates & intra-operator sharding - Jul 2026

- L1 median / quantile_cont / quantile_disc / mode: median and the
  quantiles read the sorted per-group multiset the engine already keeps
  for MIN/MAX (interpolation and ceil-index semantics match DuckDB;
  fraction read from the public QuantileBindData header - the argument
  is erased at bind time). mode maintains per-value multiplicities;
  ties break by smallest value (DuckDB's tie choice is scan-order-
  dependent and unreproducible incrementally - documented). FILTER
  combines; DISTINCT on holistic aggregates stays E110.
- L2 intra-operator sharding: residual-free inner equi-join probe
  passes over deltas >= 4096 rows split across threads (up to 8, tied
  to the dbsp_parallel knob alongside I2 view-level parallelism).
  Probes are read-only; each shard emits into its own Z-set, merged
  after; spilled-arrangement probes use shard-local scratches. The
  serial path keeps its hoisted index reference - re-resolving the
  side per row cost ~20% and was caught and reverted by bench. Join
  delta bench: serial 434k rows/s, sharded 533k (thin buckets; fat
  buckets gain more). Pads/marks/residual joins stay serial.

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

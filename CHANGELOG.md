# Changelog

## Streaming construction: 144M create measured complete - Aug 2026

- With streaming scans/backfills (no build maps), the 144M leafjoin
  create COMPLETES in 10min13s with a 4.4GB end footprint (the 22GB
  transient peak is DuckDB's own uncapped buffer pool on the create
  connection). Reopen attach 0.7-2s; dirty save 0.19-0.39s; clean save
  0.17s. Known one-time cost: the first edit after the first reopen
  pages in ~11GB of mapped indexes (~3min); subsequent edits ~6s at
  144M. Prior attempts died at 9h/39GB (map era) and 22GB-climbing
  (fold era).

## Memory-mapped shared-arrangement arenas - Aug 2026

- FlatPackedIndex gains a MAPPED mode behind read accessors
  (dir_at/bucket_at/arena_data): the v2 sidecar file (sections 8-byte
  aligned; v1 rejected — one backfill on first reopen after upgrade)
  mmaps read-only and the arena-class bytes become reclaimable page
  cache instead of process-resident vectors. Move-only (owns fd +
  mapping); build paths keep owned vectors.
- Wiring: create writes the v2 sidecar at backfill completion
  (commit-stable inside CREATE VIEW) and re-points `flat` at the
  mapping — the FIRST SAVE then skips the arrangement entirely; reopen
  adopt maps instead of copying.
- Measured 18M create end: footprint 4513 (pre-fold) / 2708 (folded) ->
  1581MB mapped; accounted arrangement state 1475 -> 179MB; first save
  11.7s -> 0.0s; reopen attach 0.12s with parity exact through
  create-edit-save-reopen-edit. Battery PASS, ctest 44/44.

## Tier-2 formats on the create path - Aug 2026

- Checkpointable-mode create held its working state in mutable maps until
  the first save folded them: 39GB footprint at 144M (measured, killed).
  Baselines now fold+self-adopt into the mmap'd digest sidecar right
  after their commit-stable initial scan (fold_fresh_baseline), and
  shared arrangements fold their bucket maps into the flat arena at
  backfill completion (compact_to_flat) — the same two-layer shape a
  sidecar adopt produces, built locally.
- Interplay: enable_spill migrates the merged flat+overlay view (K2 live
  migration previously walked only the maps); the sidecar save writes a
  base file for locally folded flats (the old skip assumed flat implies
  file) and records any full write as a delta-chain base.
- Measured 18M create end: footprint 4513 -> 2708MB (-40%), parity exact,
  save 3.1s (folds no longer in it). Projected 144M: ~39GB -> ~14-16GB;
  remaining resident class is the arrangement arena (~12GB at 144M) —
  next increment if needed: write+mmap its sidecar at compact time.

## NTH_VALUE frame-relative + recursive-step shape guard - Aug 2026

- NTH_VALUE: both render call sites (dbsp_window_view.hpp) indexed the
  nth row of the whole PARTITION, ignoring frame bounds — wrong for the
  default frame (rows before the nth must be NULL) and for explicit ROWS
  frames. Now frame-relative (same boundary math as LAST_VALUE); the
  deliberately narrow bare_constant_int gate is lifted (constant_int,
  cast-tolerant) per the recorded plan "fix render first, then widen".
  Regression: test/python/test_nth_value_frames.py (default frame,
  sliding ROWS frame, unbounded frame, through inserts and a delete, vs
  stock DuckDB truth); integration test updated from "stays gated" to
  "accepted and frame-relative".
- Recursive steps containing row-collapsing operators (DISTINCT /
  GROUP BY / window / non-UNION-ALL set ops) were ACCEPTED and silently
  wrong on every execution path (the fixpoint iterates frontiers, not
  SQL's per-iteration working table). The planner frontend now rejects
  them loudly at CREATE (reusing scan_step_linearity); LIMIT is already
  a stock parse error inside recursion. Hosts no longer need their own
  guard (NuEPM's assert_step_linear stays as defense in depth).
  Regression: test/python/test_recursive_step_guard.py (linear recursion
  still works incrementally; DISTINCT/GROUP BY/LIMIT steps reject).

## Statement-less commits stop triggering full scans (engine-hook mode) - Aug 2026

- Fresh connections fire implicit setup/teardown commits that never ran a
  classified statement (stmt NONE, nothing captured). The pre-H1 safety
  net treated them as "writes we could not attribute" and ran sync_all —
  a full scan-diff of every tracked table, 1-2x nondeterministically per
  session (thread/WAL timing). With the engine hook proven live
  (first-delivery latch), such a commit provably wrote nothing: every
  tuple write is delivered by the patched engine's commit callback, and
  DDL is statement-shaped (classifies WRITE_UNKNOWN, still syncs). The
  fallback now returns early in that exact signature; stock builds keep
  the pessimistic net (Appender writes are invisible without the patch).
- Residual (documented): strays BEFORE the first tracked-table delivery
  (e.g. during initial model create, where inserts precede tracking)
  still scan — one-time create-phase cost; adopted state survives them
  via the empty-diff rebuild discard.
- Diagnostics: DBSP_DEBUG_SYNC now attributes fallback syncs (thread,
  context, tracked-table count, last classified query) and save_index
  branch decisions.

## Delta-append sidecars: dirty save O(changed rows) - Aug 2026

- A dirty dbsp_save folded and rewrote every durable sidecar wholesale —
  the baseline digest index (fold+sort+write of all entries) and each
  shared-arrangement fingerprint file (fold_packed of flat+overlay).
  Measured at 18M rows: 7.1-7.8s dirty, 3.4s "clean" (the skip only held
  until the first dirty save). Now 0.03s for both:
  - Digest index: with an adopted base and a small overlay (<10% of the
    base), save writes ONLY the overlay as `.idx.d` (sorted, watermark-
    stamped, chained to the base by its watermark + entry count + live
    log size). Adopt loads base mmap + delta into the overlay. A big
    overlay compacts: full fold, delta deleted, and the just-written
    file is SELF-ADOPTED (fresh mmap, cleared overlay) so later saves
    skip or write small deltas.
  - Arrangement sidecars: replacement buckets for the keys touched since
    adopt (`fold_packed_touched`, O(touched)) written as `<sharr>.d`,
    chained to the base by the register-time watermark; the loader diffs
    them back into the packed overlay. Non-adopted (backfill-built)
    arrangements keep the full fold.
  - Clean-skip fixed both places: every successful save records the
    watermark it saved under; a same-watermark save writes nothing.
- `end_rebuild` with an EMPTY diff now discards the rebuild instead of
  swapping: a no-change sync is a no-op on state. The swap used to
  rewrite the log and silently destroy the adopted flat layer, the
  saved-watermark skip, and any delta chain — triggered nondeterministically
  by unattributable commits (stmt-less, e.g. WAL-checkpoint timing)
  falling back to sync_all. OPEN follow-up: root-cause that stray
  commit classification; its full scan cost remains.
- Regression: test/python/test_delta_append_sidecars.py (bases untouched
  by dirty save, delta files appear, delta-chain reopen exact, clean-save
  writes nothing, compaction folds and reopens exact).

## Notify rows cast to tracked schema types - Aug 2026

- `dbsp_notify_insert/delete` passed raw argument Values into the delta:
  a literal like `5000.0` parses as DECIMAL(5,1), not the column's
  DOUBLE. Packed arrangements/join indexes encode by schema type and
  threw "unencodable row" (test_checkpoint_restore /
  test_quack_checkpoint); even boxed, a differently-typed row would not
  cancel its baseline twin. Notify rows are now cast to the tracked
  table's column types (`CDCManager::tracked_column_types` + notify-side
  `CastNotifyRow`); arity mismatches and failed casts throw. Untracked
  tables pass through unchanged.

## Multi-table commit = one circuit pass - Aug 2026

- A transaction writing several tracked tables used to propagate once per
  table at COMMIT. Each pass rewrote every downstream view's
  single-generation dbsp_changes buffer — a view reading BOTH tables kept
  only the last table's effects for dbsp_changes consumers — and a join
  both of whose sides changed in one commit missed its −Δl⋈Δr
  both-shared correction. All three commit paths now collect every
  table's delta first and run ONE propagation pass:
  - engine-hook fast path: `CDCManager::apply_captured_deltas` (new)
    applies every baseline then propagates once; failed tables still fall
    to the scan path per table.
  - write-capture fast path (`apply_captured`): merged per-table deltas
    go through the same `apply_captured_deltas`.
  - scan fallback (`sync_tables`): scans/consumes every table first, then
    one `propagate_changes_multi` pass.
- `propagate_changes_multi(sources)` generalizes `propagate_changes` (now
  a delegating wrapper): pending map seeded with all sources, arrangements
  updated for each source before any view steps, levels built over the
  union topo order (`DependencyGraph::topological_order(vector)` — new
  overload). One commit_seq_ bump per pass, so every stepped view shares
  the pass's delta generation.
- Regression: test/python/test_multi_table_commit.py (two-table commit;
  per-source views, a both-tables view, both-sides join insert, shared
  generation stamp).

## Bounded-RAM Phase 2d increment 2: packed shared arrangements - Aug 2026

- SharedArrangement stores its buckets packed (same codec/pattern as the
  join-node indexes: packed_ok at registration when side types are
  codec-clean and no MARK counters; probes decode into consumer scratch
  incl. O4 projection; spill migrates both directions).
- Measured (141-view wfp DAG): arrangement class 2.43GB -> 1.66GB,
  accounted total 4.83 -> 4.07GB, footprint 10-11GB. Edits 0.2s steady.
- Window partition caches (0.85GB) deliberately left boxed: packing them
  would force decode inside the sort/positional fast paths for ~0.5GB —
  poor risk/reward at current sizes. Revisit only if soak shows models
  where window state dominates.

## Bounded-RAM Phase 2d: packed join-index rows - Aug 2026

- PlanJoinNode's local indexes store rows as compact self-describing byte
  strings (dbsp_packed_row.hpp: tag+payload per value; ~450-600B boxed ->
  tens of bytes) for INNER/LEFT-pad joins whose side types are all
  codec-supported. Buckets decode into the probe scratch on access — the
  same materialize-into-scratch pattern the spilled/projected shared paths
  use — so probe/pads/match_count route through probe_side unchanged.
  MARK and right-padding joins stay boxed (not emitted by compiled plans).
- Serialization: packed-native blob layout behind a magic header; boxed-era
  checkpoints decline cleanly (rebuild by replay). own_row_weight() unifies
  the shared/packed/boxed weight lookup for pad reconciliation.
- Measured (141-view wfp DAG): arrangement class 7.35GB -> 2.43GB,
  accounted total 9.65 -> 4.83GB, process footprint 13-15 -> 11-12GB.
  Cost: cold create +~34s (encode at replay; background + once per model
  lifetime under Phase 3 reattach) and steady edits 0.17 -> ~0.2s.
- Fixed during bring-up (LEFT JOIN differentials caught it): the packed
  probe branch must yield to a SHARED side — intercepting before
  side_index() probed an empty local map and pads never retracted.
- Remaining boxed: shared arrangements (~1GB) and window caches — next.

## dbsp_realize(view): background warming with RAM-budget feedback - Aug 2026

- New TF: realizes ONE pending (lazy-restored) view's circuit state and
  returns (realized BOOLEAN, state_bytes BIGINT) — the view's resident
  bytes after realization (single-view accounting scan). An embedder pumps
  it on a worker thread after reattach, summing state_bytes against a RAM
  budget so warming stops and the tail stays lazy.
- scan_one_view_state(): per-view StateBytes accounting (fresh dedupe
  scope — overestimates shared rows, the safe direction for a budget).
- CALLER NOTE: the TF's side effect runs in Bind — use execute()/fetch
  paths that bind once; the relation API's double-bind reports
  was_pending=false on its second pass (the side effect itself stays
  correct/idempotent).

## Bounded-RAM Phase 3: reattach instead of replay - Aug 2026

- **Reattach**: a reopened disk-backed database ADOPTS its __mv_ tables.
  __dbsp_mv_meta is read at LOAD start (before any view exists), marking
  views table-backed so load-time consumers stream from tables instead of
  the checkpoint's deliberately-empty result blobs; checkpoint-restored
  views skip backfill; fingerprint-declined views cold-create and rewrite
  their tables. 141-view DAG: reattach 0.45s vs 48.9s cold; first read
  0.16s; post-reattach edits incremental (first touch realizes its cone).
- **Serializable UNION/DISTINCT state**: PlanSetOpNode/PlanDistinctNode
  now checkpoint (per-row counts) — they previously declined, forcing
  every union view to cold-replay at each load (13.9s of the reattach).
- **True-lazy checkpoint blobs**: on disk-backed databases the load reads
  only the blob CATALOG; bytes are fetched per view at realize time.
- **Exit-race hardening**: the detached close-time auto-save could run
  SQL while the host process exits (segfault in static destructors). The
  auto-save now skips when nothing changed since the last save
  (dirty_since_save_), dbsp_save() is the explicit sync path, and
  dbsp_wait_teardown() lets an embedder drain the teardown thread before
  exiting. Contract: call dbsp_save() before your final close, then
  dbsp_wait_teardown() from a throwaway connection if exiting immediately.
- **Tests**: test_mv_tables.py grew reattach + kill-crash recovery
  sections (8/8 stable); ctest 44/44.

## Bounded-RAM Phase 2 findings: shared padding-left attempted + reverted - Aug 2026

- Attempted: shared arrangements for self-padding LEFT join sides (the
  compiled dense-driver plans put one static cross table on the LEFT of
  every view's LEFT JOIN; a probe showed 8 such views dropping 196MB of
  private indexes to 0 with one 51MB shared arrangement, pad-transition
  parity clean).
- Reverted: init is ORDER-DEPENDENT — with the arrangement backfilled at
  registration and the left replay not skipped, a source replayed before
  the left table double-counts matches; with the replay skipped, pads are
  never emitted (planner_frontend LEFT/FULL differentials caught it). A
  correct version needs an explicit init-pads pass in PlanJoinNode; the
  eligibility gate now documents this. ctest back to 44/44.
- Measured for the record (141-view DAG, disk-backed): private
  arrangements 7.05GB — dominated by CHAINED-join intermediate left
  indexes, which no sharing can address; spill toggled post-attach costs
  13s and reaches 5.97GB (digest+cache resident). Remaining reduction
  requires packed row representation (spec Phase 2d).

## Bounded-RAM Phase 1c: table-backed reads, results out of RAM - Aug 2026

- With `dbsp_mv_tables(true)`, sinks stop integrating: the __mv_ table IS
  the view result. `dbsp_query`/scan, create-replay of view sources, and
  shared-arrangement backfill stream from the table; checkpoint save skips
  result rows for table-backed views (durable in the table already); the
  weight-multiplicity fallback rebuilds from the table's own rows + delta
  (no RAM result needed). Disable schedules a view rebuild (reintegrate).
- **Fixed en route**: `dbsp_mv_tables` ran its side effect in Bind, and the
  relation API binds twice per statement — the second enable re-backfilled
  AFTER the results were dropped, wiping every table to 0 rows. Enable is
  now idempotent.
- Measured (141-view DAG): result class 2.3GB -> 0; reads of a 167k-row
  view from the table in 0.04s; edits 0.17-0.78s; parity suites green.
  Remaining RAM: join arrangements 7.35GB (Phase 2), window caches 0.85GB.

## Bounded-RAM Phase 1b: disk-backed MV result tables + lost-update fix - Aug 2026

- **dbsp_mv_tables(enable)**: every view mirrors its result into a
  `__mv_<view>` table in the same database file, backfilled at enable/create
  and delta-maintained per propagation pass (one internal transaction; ±1
  weights via staged DELETE+INSERT, anything else full-rewrite);
  `__dbsp_mv_meta` watermarks each write. Measured on the 141-view DAG:
  backfill 4.2s, 168MB on disk, edits 0.04-0.44s, 80/80 views identical to
  their tables after an edit run. Internal writes run under
  InternalQueryGuard — commit hooks never re-enter the manager.
- **Fixed (pre-existing, exposed by the new test): first write after a
  TF-triggered autoload silently LOST.** Table-function binds called
  maybe_autoload without EnsureContextState, so a connection whose first
  DBSP call was a TF (python clients) had no QueryBegin hook: the next
  UPDATE skipped pre-write deferred-baseline materialization, the commit
  path saw a watermark mismatch, declared a false out-of-band change, and
  dropped the update (views stale until manual rebuild). Every TF bind now
  ensures context state. Repro: reopen -> dbsp_views() -> UPDATE.
- **Tests**: test/python/test_mv_tables.py (parity across agg/join/window/
  recursive/chained/duplicate-row shapes, reopen durability, disable);
  ctest 44/44.

## Bounded-RAM Phase 1a: transient node outputs dropped post-step - Aug 2026

- **Why**: every circuit node's `output_` z-set kept its LAST step's rows for
  the view's lifetime — after create, that is the full initial population per
  node: 11.4GB of a 23GB production-shaped DAG footprint.
- **What**: `SinkNode` now OWNS its delta (copy = H6 refcount bumps, not
  Value copies) instead of borrowing the upstream buffer; every other node
  gets `clear_output()`, called by the owning view after each step
  (`trim_outputs`). Create-time initial-population deltas are dropped
  (`drop_delta`) once the generation stamp is taken — nobody consumes them
  (generation filtering skips create buffers), and they pinned the full
  result twice. `dbsp_changes` after CREATE on a populated table is now
  EMPTY (semantics change; post-create edits unaffected).
- **Measured**: 141-view DAG footprint 23GB -> 15GB; "other" class
  11.4GB -> 0.6GB; steady edits unchanged (0.03-0.05s).
- **Also fixed** (pre-existing flake the gate caught): `created_at` doubles
  as checkpoint LOAD order but was wall-clock milliseconds — back-to-back
  creates tied and the unstable load sort could order a dependent before
  its source (intermittent "2 from checkpoint" cold-create fallback). Now
  strictly monotonic; load sorts stable. lazy_restore 8/8, ctest 44/44.

## dbsp_view_state(): per-view circuit-state accounting - Aug 2026

- **Why**: bounded-RAM roadmap Phase 0 — you cannot diet or budget state you
  cannot see. Measurement on a 141-view production-shaped DAG: 23GB true
  physical footprint (ps RSS showed 2.5GB — macOS compression hid 90%),
  ~19.5GB accounted by class: 11.4GB node output/delta buffers, 5.8GB join
  arrangements (incl shared), 2.3GB result z-sets, 0.8GB window caches.
- **What**: `account_state(StateBytes&, StateAccounting&)` virtuals on
  NativeMaterializedView + dbsp::Node (join/aggregate/distinct/set-op/
  recursive/embedded/window/circuit views); `CDCManager::scan_view_state`;
  `dbsp_view_state()` table function with per-class bytes and a
  `__shared_arrangements` synthetic row. H6 payload-shared rows counted once
  per scan. `DBSP_ACCT_DEBUG=1` prints sighting/payload counters.
- **Tests**: `test/python/test_view_state.py` (class attribution +
  monotonic growth); ctest 44/44.

## dbsp_delta_generations(): per-view delta-buffer provenance - Aug 2026

- **Why**: `dbsp_changes` is a single-generation buffer that reading does not
  drain — an untouched view serves its last delta (create-time initial
  replay, at worst) forever. A change-feed consumer polling every view after
  each commit re-harvested those stale full-view buffers on every edit
  (measured downstream: a 1-cell edit yielded ~229k spurious "changed" cells
  across 55 line items, and stale `old=NULL` pairs would corrupt undo).
- **Fix**: `CDCManager` stamps `commit_seq_` into `view_delta_generation_`
  whenever a view's delta buffer is rewritten (create-time initial replay,
  `propagate_changes` step); new `dbsp_delta_generations()` table function
  exposes `(view_name, generation)`. Consumers baseline on
  `MAX(generation)` and harvest only views whose generation advanced.
- **Tests**: `test/python/test_delta_generations.py` pins the contract
  (touched-only advance, reads never advance, drop cleanup); ctest 44/44.

## Perf: CREATE MATERIALIZED VIEW classified READ — per-create full scan-diff removed - Aug 2026

- **Symptom** (residual after the shadow-table fix): per-view create cost still
  grew with total view count (1.9→6.0ms/view from 500→2,000 views).
- **Root cause**: `CREATE MATERIALIZED VIEW` is dbsp syntax the core parser
  cannot parse, so `classify()` hit "unparseable: assume the worst" →
  WRITE_UNKNOWN → the commit hook ran `sync_all` — a full scan-diff of EVERY
  tracked table on every view creation: O(views × rows) DAG builds.
- **Fix**: a leading-keyword carve-out classifies `CREATE MATERIALIZED VIEW`
  as READ — view creation only reads tracked tables to populate the new view.
- **Measured**: per-view create now FLAT (1.06/1.02/1.15 ms at 500/1,000/2,000
  views); a 2,000-view DAG builds in 2.3s (~112s before the two fixes).
- **Tests**: ctest 44/44; NumPad gated suites (incl. incremental edit sync and
  MV-authority deltas) green.

## Perf: view creation shadows only referenced MVs — O(N^2) DAG registration fixed - Aug 2026

- **Symptom**: creating or loading a large view DAG was quadratic in total
  view count — per-view create cost grew linearly (3.5ms/view at 40 views to
  28ms/view at 1,000), and peak RSS ran ~5MB per view even for tiny views.
- **Root cause**: `CDCManager::create_view` handed `PlanTranslator::translate`
  the schemas of ALL existing MVs; each translate shadowed every one as a
  `CREATE TEMP TABLE` so views-on-views bind. View #k paid k-1 real catalog
  creates (DependencyManager dominated — ~83% of create time in a 1,000-view
  profile), and the shadow tables were also the per-view memory.
- **Fix**: shadow only views the SQL can reference (case-insensitive substring
  match on the SQL text — a false positive creates one extra temp table; a
  referenced identifier always appears verbatim).
- **Measured** (compiled-DAG bench, depth-4 chains): 500 views create
  6.5s → 0.97s, load 6.9s → 0.54s, RSS 1.7GB → 134MB; 1,000 views create
  28s → 2.9s, load 29.4s → 1.2s, RSS ~5GB → 203MB; 2,000 views create 12s
  (pre-fix projection ~112s). A smaller residual per-view growth remains
  (~10x lower constant) — separate item.
- **Tests**: full ctest 44/44; downstream NumPad gated suites (compiled wfp
  DAG all-MV parity + incremental edit sync) green.

## Fix: NTILE bucket-boundary math + ungate constant bucket counts (Task 2) - Jul 2026

- **Bug**: `NativeWindowView`'s NTILE (`include/dbsp_window_view.hpp`, two
  render call sites — the fast-path `render_row()` and the full-partition
  re-render loop inside `apply_changes()`) computed
  `bucket = floor(row_idx * n / p) + 1` for `p` partition rows and `n`
  requested buckets. This formula coincides with stock DuckDB's answer
  when `n <= p`, but diverges whenever there are more buckets than rows
  (`n > p`): it skips bucket numbers instead of assigning rows
  sequentially to buckets `1..p` the way stock does (found during the
  2026-07-31 constant-frame fix's differential check; confirmed
  pre-existing and unrelated to that fix, so left gated rather than
  shipped broken — see TODO.md's prior entry).
- **Root cause**: the proportional-floor formula is a well-known
  balanced-partition identity, but it silently assumes `n <= p`; for
  `n > p` it needs the same "clamp bucket count to row count" step stock
  DuckDB takes (`WindowNtileExecutor::EvaluateInternal`,
  `duckdb/src/function/window/window_rownumber_function.cpp:171-175`) —
  `if (n_param > n_total) n_param = n_total;` — before splitting rows into
  `n_large = p - n*floor(p/n)` "large" buckets of `floor(p/n)+1` rows and
  the rest at `floor(p/n)` rows.
- **Fix**: both NTILE call sites now clamp `num_buckets` to the partition
  size, then apply the same `n_size`/`n_large`/`i_small` split as stock's
  `WindowNtileExecutor` (verified by reading that function, not by
  running it — see the code comment for the derivation and Global
  Constraint on no builds/tests for this task).
- **Ungate**: `dbsp_plan_translator.hpp`'s `visit_window` now extracts
  NTILE's bucket-count argument with `constant_int` (the `BOUND_CAST`-
  unwrapping extractor already used for window frame bounds and LAG/LEAD
  offsets) instead of the strict `bare_constant_int`, so
  `NTILE(4) OVER (...)` with a literal bucket count is accepted instead of
  failing `DBSP-E110: NTILE with non-constant bucket count`.
- **NTH_VALUE verdict — stays gated**: differentially reading NTH_VALUE's
  render logic (both call sites, same file) found it always evaluates
  `rows[n-1]` / `partition_rows[n-1]` — the N-th row of the whole
  *partition* — never consulting the window's frame bounds
  (`win.start`/`win.end`) at all. Stock DuckDB's NTH_VALUE is
  frame-relative ("value evaluated at the row that is the n'th row of the
  window frame", `window_value_function.cpp`): with the default frame
  (`RANGE UNBOUNDED PRECEDING AND CURRENT ROW`), stock returns NULL for
  rows before the frame has grown to N rows, and generally answers
  relative to `[frame_start, frame_end]`, not `[0, partition_end]`. This
  is a distinct, pre-existing bug from the NTILE one — ungating NTH_VALUE
  now would ship a silently wrong result for every frame narrower than
  the full partition (i.e. most uses). Left gated (`bare_constant_int`
  unchanged); TODO.md's gap entry updated to describe this specifically
  rather than bundling it with NTILE.
- **Test**: `test/integration/test_advanced_window.cpp` — two new
  differential tests (materialized view vs. stock DuckDB, exact values):
  one covering `p % n == 0`, `p % n != 0`, `n > p` (empty buckets), and a
  single-row partition all in one partitioned query; one walking a single
  partition's row count through `n > p` → `p == n` → `n > p` (different
  remainder) via INSERT then DELETE, exercising the full-partition
  re-render path (NTILE has no fast-path rule in `affected_indices()`, so
  every edit — structural or not — re-renders the whole partition). The
  old combined "NTILE and NTH_VALUE stay gated" scope-boundary test is
  now NTH_VALUE-only; its NTILE half is superseded by the differential
  tests above. Pre-fix (bucket math wrong, gate still narrow), the new
  differential tests' `dbsp_create_view` calls fail at creation with
  `DBSP-E110: NTILE with non-constant bucket count` (gate not yet
  widened) — they only exercise the fixed math once both changes land
  together, which is why this is one commit. **Untested-by-build**:
  verification deferred to the batched cycle (Task 3); this is a static,
  read-the-source fix with no compiler or test run in this task per the
  binding build protocol.

## Feature: lazy per-view checkpoint restore (D-lazy, Task 1) - Jul 2026

- **Motivation**: a warm reopen paid the blob-decode cost of every
  checkpointed view up front, even for views the session might not touch
  for minutes — e.g. 43 views' worth of `restore_circuit_state`/sink
  decode before `dbsp_load()` could even return.
- `load_from_duck_table`'s D3b checkpoint fast path no longer decodes a
  cold-created view's node/sink blobs immediately. It stashes the
  already-read (but undecoded) bytes in a new `CDCManager::pending_restore_`
  map (`name -> {nodes, sink}`) and marks the view pending
  (`NativeMaterializedView::mark_pending_restore()`), gated by a new
  `dbsp_lazy_restore(BOOLEAN)` setting, **default ON**. `dbsp_lazy_restore(false)`
  reproduces the exact pre-D-lazy eager behavior (every checkpointed view
  fully restored during the load call itself) for bulk benchmarks or
  debugging a restore issue without the pending indirection in the way.
- `CDCManager::realize_pending_view[_locked]` decodes a pending view's
  stash on first need (mirrors D3c's `TrackedTable::is_deferred()` +
  `materialize_deferred_locked` for table baselines — same
  lazy-materialize-on-first-need shape, same locking discipline: no new
  lock level, `pending_restore_` guarded by the existing `view_mutex_`
  tier that already owns view content). Wired into every surface that
  reads a view's live state:
  - `scan_view`/`query_view` (the `dbsp_query`/`dbsp_changes`... read
    path) — self-locking `realize_pending_view()`, a cheap shared-lock
    peek before ever taking `view_mutex_` exclusively.
  - `propagate_changes` — a sequential pre-pass realizes, before any
    `step_view` runs, every view in the delta's topological dependent set
    (`topo`) **and every view that any of those transitively depends on**
    (walked via the existing `dep_graph_.get_dependencies` reverse edges —
    every SQL-declared source `create_view` already records there,
    arrangement-shared or not), not `topo` alone. **Correction (same-day,
    reviewer-reproduced Critical):** the first cut of this pre-pass
    realized only `topo` — source_name's *dependents* — on the mistaken
    assumption that this also covers "every pending ancestor of it". It
    does not: a join view whose shared/arrangement-probed side is a
    *sibling* dependency of the edited table (e.g. `v2 = side1 LEFT JOIN
    v1`, editing `side1`) has `topo = {v2, v3}` — `v1` is never reached by
    walking dependents forward from `side1`, however pending it still is.
    `probe_side` has no `needs_backfill` guard, so an unrealized `v1`'s
    still-empty arrangement silently produced a NULL-padded join result
    instead of a real match — a wrong delta that then propagated further
    downstream in the same pass via `apply_to_arrangements`. Fixed by
    walking topo's dependency closure too (a small worklist over
    `get_dependencies`, deliberately the "boringly-safe" superset — it
    also realizes a still-pending view source that *isn't* actually
    arrangement-shared, harmlessly, rather than requiring a precise
    `arrangement_requests()`-only walk). Mirrors the D3c table-side
    precedent (`materialize_for_delta` sweeps every tracked table before
    any delta propagates), scoped here to topo's closure rather than
    every view process-wide, since unlike tables, views already have a
    dependency graph to scope the sweep with — an unrelated view stays
    lazily unrealized. Done sequentially, before the per-level loop that
    may spawn threads under `dbsp_parallel(true)` — `pending_restore_` is
    one map shared by every view, not a per-view lock, so realizing
    everything up front keeps the existing "each view's own disjoint
    state" assumption behind the lock-free parallel `step_view` section
    true.
  - `create_view`'s warm-replay loop reads a source **view**'s
    `get_result()` directly, so it realizes that source first,
    unconditionally.
  - `register_arrangements`'s view-sourced arrangement backfill only
    realizes the source view for a **warm** (non-`cold`) registration —
    mirroring exactly the table-side rule immediately above it
    (`materialize_deferred_locked` for a still-deferred table when
    `!cold`): a warm create's *other*, non-shared-skip sources replay
    through this same join synchronously right after registration
    (`create_view`'s warm-replay loop, same `view_mutex_` hold), and would
    probe a still-empty arrangement mid-replay if the source weren't real
    yet. A **cold** (checkpoint-loop) registration, by contrast, defers:
    the arrangement is created empty and flagged `needs_backfill`, wired
    into the join node, and left alone — no read of the source view
    happens at registration time, and nothing probes it either, since the
    checkpoint fast path runs no warm-replay at all
    (`skip_init_replay` empties `resolved_sources`). The one guaranteed
    fill point for a deferred one is `realize_pending_view_locked` itself:
    once a pending view decodes (from any of the call sites above), it
    now also backfills every `needs_backfill` arrangement registered over
    it (`backfill_deferred_arrangements_locked`, generalized from its
    previous table-only shape to also read a view's `get_result()`).
    Every join-node probe of such an arrangement is downstream of that
    point — `propagate_changes`'s pre-pass realizes a delta's entire
    transitive dependent set (which necessarily includes the arrangement's
    source view, by dependency-graph construction) before its per-level
    `step_view` loop runs; a directly-queried view's own checkpoint-decoded
    result never reads the arrangement at all. (Originally shipped
    realizing the source view unconditionally here, cold or warm — on a
    fully chained DAG, where every view feeds another, the cold case
    transitively decoded the entire DAG during the checkpoint-load loop
    itself, defeating laziness. Fixed same-day, still Task 1, by gating
    the eager realize on `!cold` instead of dropping it outright — the
    warm case still needs it for the reason above.)
  - `save_checkpoint` re-saves a still-pending view's stash **verbatim**:
    no decode, no re-encode, byte-identical to what `checkpoint_valid`
    read. Valid because "pending" is definitionally "zero deltas applied
    since the stash" (guaranteed by gating every `apply_changes_batch` on
    a prior realize) — a `DBSP_DEBUG_SYNC`-gated check logs loudly if a
    pending view's live result is ever non-empty at save time instead of
    silently trusting the invariant.
  - `drop_view`/`drop_view_cascade` discard (not realize) a dropped
    view's pending stash — cheaper than decoding state that's about to be
    thrown away, and `dbsp_replace_view` gets this for free (it drops via
    the cascade, then recreates via `create_view`, which is already
    realize-aware for any view-sourced dependents it touches).
  - `scan_view_delta` (`dbsp_changes`) and `get_view` (test/debug raw
    accessor, no production caller) deliberately do **not** realize —
    audited and found safe: a pending view's last delta is legitimately
    empty (realize only ever calls `set_result()`, never touches the
    delta field), and `get_view()` has no call site that exercises the
    checkpoint fast path today (documented as a known gap for future
    test authors).
- A corrupt/mismatched stash (should not happen — the same bytes just
  round-tripped through `checkpoint_valid`'s read) cannot be handled with
  the eager path's per-view `drop_view`+`create_view` replay: every
  `realize_pending_view_locked` call site already holds `view_mutex_`
  exclusively, and both of those re-acquire it. It escalates instead to
  the existing D3c `rebuild_pending_` global rebuild-all-views escape
  hatch (checked at every `QueryBegin`) — same "rare correctness escape
  hatch" the codebase already accepts for a deferred-baseline watermark
  mismatch, reused rather than duplicated.
- `dbsp_load()`'s report string gains a distinct `"N pending lazy
  restore"` clause alongside the existing checkpoint/deferred-sources
  counts.
- New test-observable counter `g_lazy_view_decodes` (g_* convention, see
  `g_recompute_invocations`) counts actual stash decodes process-wide, for
  asserting "only this view's chain decoded" without parsing
  `DBSP_TIMING` stderr output; `realize_pending_view_locked` also emits
  the same `"blob_decode"` `DBSP_TIMING` phase the eager path already did.
- **No checkpoint format change**: the stashed bytes are the exact bytes
  `checkpoint_valid` read and, for a still-pending view, the exact bytes
  `save_checkpoint` writes back — v5 layout, unchanged.
- **Tests**: `test/integration/test_lazy_restore.cpp` — first-touch
  decodes exactly one view's chain (`g_lazy_view_decodes` delta) leaving
  siblings pending; a delta on a pending view's source realizes it before
  applying, differential vs a continuously-live twin; save-after-partial-
  realization round-trips (one view re-encoded, the other re-saved
  verbatim, both correct after a second reload); `dbsp_lazy_restore(false)`
  reproduces the eager behavior exactly (0 pending, the lazy decode
  counter never moves). A fifth case added with the register_arrangements
  fix above: a chained view-on-view DAG (`base -> v1 -> v2 -> v3`, `v2` a
  `LEFT JOIN` with `v1` as its shared/probe-target side, `v3` the same
  shape one level down over `v2`) asserts `pending_restore_count() == 3`
  immediately after reopen, before any query — the assertion the original
  (defective) unconditional-realize behavior would have failed below 3 —
  then a delta on `v1`'s own source (`items`) realizes the whole chain in
  one pre-pass (asserts the decode count moves by exactly 3), then two
  further deltas each probe one link's backfilled arrangement from the
  join's other (local) side against the live twins. A sixth case is the
  reviewer's Critical repro made permanent: the fifth case's first delta
  always lands on `items` (`v1`'s own source, already in that delta's
  topological dependent set), so it never exercises the sibling-branch
  gap above — this case's first delta after reopen instead lands directly
  on `side1` (`v2`'s *local*, non-shared join side) while `v1` is still
  fully pending; asserts `v2`'s new row carries `v1`'s real value, not a
  silent NULL pad, matching the reviewer's exact reproduced shape.

## Feature: window-view checkpoint state (restore-tail, Task 2) - Jul 2026

- Circuit-state checkpointing (D3b) now covers `NativeWindowView` embedded
  behind `EmbeddedViewNode` (the WINDOW plan shape). `EmbeddedViewNode`
  previously had no `state_kind()` override at all, so every embedded view
  — window, sort/limit, distinct-on — fell back to a full rebuild-by-replay
  on load regardless of shape. `EmbeddedViewNode::state_kind()` (and
  `serialize_state`/`restore_state`) now delegate to three new
  `NativeMaterializedView` virtuals (`circuit_state_kind()`/
  `serialize_circuit_node_state()`/`restore_circuit_node_state()` —
  intentionally distinct names/signatures from the existing
  `checkpointable()`/`serialize_circuit_state()`/`restore_circuit_state()`,
  which compose *multiple* inner `dbsp::Node`s for a circuit-backed view
  like `PlannedCircuitView`; an embedded view is a single opaque leaf to
  that composition, not a nested circuit). Default `UNSUPPORTED`; only
  `NativeWindowView` overrides this pass — `NativeSortView`/
  `NativeLimitView`/`NativeDistinctOnView` still decline via the base
  default, so a sort/limit/distinct-on embedded view still forces its
  whole outer view to rebuild-by-replay, unchanged from before this pass.
- `NativeWindowView` has no spill/overflow storage (checked: no `spill_`
  member, no `SpilledBucketIndex`/spill-store usage anywhere in the file)
  — unlike joins/aggregates it never needs a runtime decline, so
  `circuit_state_kind()` unconditionally reports `SERIALIZABLE`.
- `serialize_circuit_node_state`/`restore_circuit_node_state` carry
  exactly what `apply_changes`' incremental fast path (the
  size-unchanged overwrite-in-place branch) and its structural
  full-render path each read to resume: `partitions_` (each partition's
  ORDER-BY-sorted source rows — both paths key membership, order, and
  overwrite-vs-structural classification off this) and
  `partition_outputs_` (the rendered-output cache, index-aligned with
  each partition's sorted rows — the fast path's
  size-unchanged gate and its retract-before-overwrite both read this
  directly; the full path's opening retraction loop reads it too).
  `result_` (`get_result()`) is *not* independently serialized: nothing
  reads it back inside this class (`apply_changes` only ever writes to
  it, always in lockstep with `partition_outputs_`), and nothing external
  calls `get_result()`/`set_result()` on an embedded view directly (unlike
  a top-level view's own sink, whose integrated total *is* restored at
  the outer `NativeMaterializedView::get_result()`/`set_result()` level —
  see `dbsp::SinkNode::state_kind()`'s doc comment for that precedent).
  Restore reconstructs it for free from the just-restored
  `partition_outputs_` instead (provably always the multiset union of
  every partition's current cache, by induction over `apply_changes`)
  rather than leaving it silently empty or paying bytes for fully
  redundant data.
- **No checkpoint format version bump**: unlike Task 1's addition, this
  does not change the byte layout of any blob a checkpoint could
  *already* contain — `EmbeddedViewNode` never wrote a node blob before
  (it was `UNSUPPORTED`), so an existing v5 checkpoint simply has no entry
  keyed by a window-embedded node's id. `restore_circuit_state`'s
  per-node loop already treats a missing key as a decline for that node
  (`blobs.find(n.id()) == blobs.end()` → that view's checkpoint fast path
  declines), gracefully falling back to rebuild-by-replay for *that view
  only* — no need to invalidate the whole checkpoint via a version bump.
  `kDbspCkptFormatVersion` stays 5.
- **Tests**: `test/integration/test_join_checkpoint.cpp` — a `state_kind`
  gate case (a window embedded view checkpointable; a sort embedded view
  and a limit embedded view, same `EmbeddedViewNode` wrapper, still not),
  and a twin round-trip case (save, drop, reload, then a post-restore
  PURE VALUE UPDATE mid-partition — the incremental fast path, which also
  drives one cell's bounded frame fully NULL, hand-verified directly
  against the restored twin as the recent all-NULL SUM fix (66b3806) must
  survive the round-trip — followed by a post-restore size-changing
  INSERT, the structural full-render path, both asserted against a
  continuously-live twin).
- **Untested-by-build**: this task ran under a no-build constraint (plan:
  `docs/superpowers/plans/2026-07-31-restore-tail.md`) — verification is
  deferred to that plan's Task 3 batched build/ctest cycle.

## Feature: recursive-view checkpoint state (restore-tail, Task 1) - Jul 2026

- Circuit-state checkpointing (D3b) now covers `WITH RECURSIVE` fixed-point
  state: `PlanRecursiveNode::state_kind()` reports `SERIALIZABLE` for a
  UNION ALL recursive view whose nested step circuit is itself wholly
  checkpointable (`PlannedCircuitView::checkpointable()` — the same gate
  every other view is already held to), so it restores from a checkpoint
  instead of a full rebuild-by-replay on load. UNION (set-semantics)
  recursion's DRed support bookkeeping is not captured this pass and stays
  `UNSUPPORTED`, as does a UNION ALL step containing any other unsupported
  node (a spilled join, etc.) — `state_kind()` is independent of
  `linear_step_`: the fallback `recompute()` path (a `max_iterations_` trip,
  an exception during signed admission, or any deletion on a nonlinear
  union_all step) rebuilds entirely from the restored totals and
  `step_view_->reset()`, so a restored node resumes correctly whether or
  not the step is linear.
- `serialize_state`/`restore_state` gained `accumulated_` (the integrated
  fixed-point result `recompute()` diffs against), `anchor_total_` and
  `base_totals_` (the running per-source totals `recompute()` reseeds its
  whole rebuild from), and the nested `step_view_` circuit's own per-node
  state (equi-key indexes / group state) via its existing
  `serialize_circuit_state`/`restore_circuit_state`, embedded as a
  length-prefixed sub-blob keyed by inner node id.
- **Checkpoint format version bump (4 → 5)**: the recursive-node blob is
  new (the node was `UNSUPPORTED` before, so `save_checkpoint` never wrote
  one), so `kDbspCkptFormatVersion` advanced. `checkpoint_valid` treats a
  v4 (or earlier) checkpoint as absent rather than misparse it against the
  new `restore_state`, same one-time rebuild-by-replay cost as prior bumps.
- **Tests**: `test/integration/test_join_checkpoint.cpp` — a `state_kind`
  gate case (linear UNION ALL checkpointable; UNION recursion and a step
  forced into spill-mode storage still not), a twin round-trip case (save,
  drop, reload, then an insert-only extension and mid-chain UPDATE/DELETE
  retractions resuming the signed path from restored state, asserted
  against a continuously-live twin), and a post-restore
  `DBSP_REC_MAX_ITER`-capped fallback case proving `recompute()` rebuilds
  correctly from the just-restored totals when a deep retraction trips the
  iteration cap after restore.
- **Untested-by-build**: this task ran under a no-build constraint (plan:
  `docs/superpowers/plans/2026-07-31-restore-tail.md`) — verification is
  deferred to that plan's Task 3 batched build/ctest cycle.

## Fix: window SUM/AVG over an all-NULL frame returns NULL - Jul 2026

- `NativeWindowView`'s SUM and AVG emitted `0.0` when every value in the
  frame was NULL; stock DuckDB (and SQL semantics) return NULL — an
  all-NULL frame is not a frame that summed to zero. Both render paths
  (fast-path renderer and full re-render) fixed; COUNT correctly stays 0
  and MIN/MAX already returned NULL. Surfaced by the NumPad wfp
  measurement the same day constant bounded frames became acceptable —
  the bug predates that change but was unreachable through SQL until it.
- Differential tests vs stock DuckDB across SUM/AVG/COUNT/MIN/MAX,
  bounded and unbounded frames, including edits that make a frame
  all-NULL and back.

## Perf: checkpoint restore hash pre-seed (cold/restore attack, Task 3) - Jul 2026

- **Corrected premise**: the plan for this task assumed checkpoint blobs
  (`_dbsp_ckpt` sink/node rows) still used a slow, separate row codec and
  needed to be switched over to the spill path's typed fast-path codec
  (`serialize_row`/`deserialize_row`, "spill codec 12x", `ba26922`). Audit
  found this already true and had been since before that codec existed:
  `BlobWriter::row`/`BlobReader::row` (`dbsp_checkpoint.hpp`) have called
  those exact functions since D3b's first commit (`8c77abd`, predating the
  spill codec's typed-fast-path rewrite by 11 days), so checkpoint decode
  inherited the speedup automatically, for free, with no code change ever
  needed there. There is (and was) exactly one row codec, shared, no
  per-site duplication.
- **Real bottleneck, found by profiling instead of assuming**: a timing
  probe reproducing the design doc's "851ms/58k rows" figure (a 58k-group
  checkpointable `SUM` aggregate over `INTEGER`/`BIGINT`/`VARCHAR`/
  `BOOLEAN` columns — the value shapes NumPad's wfp workload actually
  uses) measured `blob_decode` at 1309.5ms (~22.6us/row) on this build,
  confirming the cost is real but NOT in the byte-level codec (which,
  at ~260ns/row for a 5-column fast-path row per the 12x/3.8x numbers
  already published, would only account for ~15ms of that). The actual
  cost: every row decoded from a checkpoint blob becomes a hash-map key
  (aggregate `states_`, join `Index`/`RowWeights`) or a `DuckDBZSet` entry
  on its first use, and `ColumnVec::hash()`'s lazy path calls
  `duckdb::Value::Hash()` per column — which builds a throwaway 1-element
  `Vector` and runs the full `VectorOperations::Hash` dispatch machinery
  for a single value. `dbsp_engine_hook.hpp` already documents this exact
  cost elsewhere as "~2/3 of ingestion time" for the same reason.
- **Fix**: `BlobReader::hashed_row()` (`dbsp_checkpoint.hpp`) decodes a row
  via the existing (unchanged) codec and pre-seeds its hash cache with
  `hash_row_fast`, which calls `duckdb::Hash<T>` directly for the fast-path
  types (`INTEGER`/`BIGINT`/`DOUBLE`/`VARCHAR`/`BOOLEAN`) — the exact
  primitive `VectorOperations::Hash` itself dispatches to per element for a
  flat vector of that physical type (confirmed against
  `vector_operations/vector_hash.cpp`; `duckdb::Hash(string_t)` is
  documented+asserted in duckdb's own source to equal
  `duckdb::Hash(ptr, size)` for every string, inlined or not), without
  building the temporary `Vector`. NULLs use `ColumnVec::kNullHash`
  directly (matching `ColumnVec::hash()`'s own null convention, not
  duckdb's); fallback-typed values still pay `Value::Hash()`, same
  tradeoff the byte codec already makes for its own fallback tag. Wired at
  every checkpoint row-decode call site that becomes a hash-map/Z-set key:
  the CDC sink loop (`dbsp_cdc.hpp`), `PlanAggregateNode::restore_state`'s
  group keys, and `PlanJoinNode::restore_state`'s index keys and
  row-weight keys (`dbsp_plan_translator.hpp`) — one shared helper, no
  per-site duplication.
- **No format version bump**: the checkpoint blob's on-disk bytes are
  unchanged (same codec, same layout) — this is a read-side in-memory
  cache optimization only, so `kDbspCkptFormatVersion` stays at 4 and no
  existing checkpoint is invalidated.
- **Measured**: the same 58k-row probe drops from 1309.5ms to ~25.4ms
  (mean of 6 runs: 23.6/24.0/25.4/24.2/26.9/28.4ms) — **~51.5x**, well
  past the ≥3x target.
- **Tests**: `test/integration/test_join_checkpoint.cpp` — a unit-level
  case proving `hashed_row()`'s pre-seeded hash matches what the lazy
  per-Value path computes for the same content, across every codec value
  shape (`INTEGER`/`BIGINT`/`DOUBLE`/`VARCHAR`/`BOOLEAN`, typed `NULL`s of
  each, and `DECIMAL` as the fallback type, both non-`NULL` and `NULL`);
  and an integration case checkpointing a view whose `GROUP BY` spans all
  of those types, restoring it, then inserting a row whose key exactly
  matches an already-restored group — the scenario that would silently
  duplicate a group instead of bumping its count if the pre-seeded hash
  ever diverged from what a freshly-built equal row would hash to. Both
  cases were verified to fail under a deliberately wrong hash before being
  restored to the real implementation (sabotage-and-revert check, not just
  written-and-passed).

## Feature: MIN/MAX aggregate checkpoint state (cold/restore attack, Task 2) - Jul 2026

- Circuit-state checkpointing (D3b) now covers plain (non-`DISTINCT`) `MIN`/
  `MAX` aggregates, not just the count/sum/avg family:
  `PlanAggregateNode::state_kind()` reports `SERIALIZABLE` for them, so
  MIN/MAX-bearing views restore from a checkpoint instead of rebuild-by-
  replay on load. `DISTINCT` (any function) and holistic/ordered aggregates
  (`FIRST`, `STRING_AGG`, `ARRAY_AGG`, `MEDIAN`, `QUANTILE_CONT`/`DISC`,
  `MODE`, `MAD`) stay `UNSUPPORTED`. A group whose values have spilled to
  disk (N4, >65536 values under `dbsp_spill(true)`) also declines the whole
  node — the spilled log isn't captured this pass, mirroring the join
  node's exclusion of spilled equi-key indexes.
- `serialize_state`/`restore_state` gained the per-group `values` multiset
  (duplicates preserved, written as a single row per group/agg) — exactly
  the retraction state `apply()`/`agg_value()` already read for MIN/MAX, so
  a post-restore delete of the current extreme retreats to the next
  remaining value the same way live maintenance would, including a group
  whose extreme retreats more than once in a row.
- **Checkpoint format version bump (3 → 4)**: the aggregate blob layout
  changed (new field per group/agg), so `kDbspCkptFormatVersion` advanced.
  `checkpoint_valid` treats a v3 (or earlier) checkpoint as absent rather
  than misparse it against the new `restore_state` — one-time
  rebuild-by-replay on the first post-upgrade `dbsp_load()`, then
  re-checkpoints in the new format on the next save.
- **Tests**: `test/integration/test_join_checkpoint.cpp` — a `state_kind`
  gate case (`MIN`/`MAX` checkpointable; `DISTINCT`/`MEDIAN`/spilled still
  not) plus twin round-trip cases for `MAX` and `MIN`: a checkpointed view
  is dropped and reloaded, then post-restore deltas force a group's extreme
  to advance (new value beats it) and to retreat twice in a row (the
  current extreme deleted twice), asserted against a continuously-live
  twin via exact `(row, weight)` snapshot equality.

## Fix: truthful `dbsp_load()` report on an already-populated registry - Jul 2026

- **Bug**: calling `dbsp_load()` when the registry was already populated
  (e.g. right after the auto-load that fires on the first DBSP entry point
  of a session) reported `"(N views, 0 from checkpoint, ...)"` — read as
  "the checkpoint was never used", when in fact an earlier load (auto-load
  or a prior explicit `dbsp_load()`) had already consumed it. Cost real
  debugging time chasing a checkpoint-fast-path regression that didn't
  exist.
- **Root cause**: `load_from_duck_table` (`include/dbsp_cdc.hpp`) already
  skips per-row work for any view name already live in this session's
  registry (`views_.count(name)` → `continue`), but unconditionally
  overwrote `last_loaded_count_`/`last_ckpt_restored_count_` with this
  call's (zero) counts at the end — discarding the true history from
  whichever earlier call actually did the restoring. `dbsp_load()`'s
  message (`src/dbsp_extension.cpp`) then read those zeroed counters at
  face value.
- **Fix**: `load_from_duck_table` now also tracks `last_skipped_count_`
  (rows skipped because already live). When a call loads 0 new views but
  skips ≥1 already-live ones, `dbsp_load()`'s message says so plainly
  (`"0 loaded this call (N already loaded)"`) instead of the ambiguous
  `"0 from checkpoint"` framing. A call that legitimately restores nothing
  (nothing was ever persisted) is unaffected — same message as before.
  Reload-over-an-already-populated-registry is intentionally still
  supported (a second `dbsp_load()` can still pick up newly-saved views not
  yet live in this session) — only the report was misleading, not the
  behavior.
- **Test**: `test/python/test_autopersist.py` — after auto-load populates
  the registry on reopen, an explicit `dbsp_load()` must not contain
  `"0 from checkpoint"` and must say `"already loaded"`. Confirmed failing
  (old message: `"(1 views, 0 from checkpoint, 1 sources deferred)"`) before
  the fix, passing after. Existing checkpoint-restore tests
  (`test_checkpoint_restore.py`, `test_join_checkpoint.cpp`) that assert
  `"0 from checkpoint"`/`"1 from checkpoint"` on fresh (never-yet-loaded)
  registries are unaffected — they hit `skipped == 0`, the unchanged branch.

## Feature: accept constant window frames and LAG/LEAD offsets in the planner frontend - Jul 2026

- **Bug**: `CREATE MATERIALIZED VIEW ... AVG(v) OVER (... ROWS BETWEEN 11
  PRECEDING AND CURRENT ROW)` and `LAG(v, 12) OVER (...)` were rejected as
  `DBSP-E110: non-constant frame start` / `non-constant offset`, even
  though `LAG(v)` (implicit offset 1) worked fine.
- **Root cause**: `constant_int` (`include/dbsp_plan_translator.hpp`)
  required a bare `BOUND_CONSTANT` expression class. The binder actually
  wraps frame-bound and LAG/LEAD-offset literals in a `BOUND_CAST` shell
  (confirmed via probe: `11 PRECEDING` arrives as `CAST(11 AS BIGINT)`,
  `LAG(v, 12)`'s offset as `CAST(12 AS BIGINT)`) — `constant_int` saw
  `BOUND_CAST`, not `BOUND_CONSTANT`, and rejected both. The downstream
  window machinery (`NativeWindowView`'s `start_offset`/`end_offset`,
  `render_row`, `affected_indices`) already fully supported bounded ROWS
  frames; only the frontend gate misfired.
- **Fix**: `constant_int` now unwraps a chain of `BOUND_CAST` nodes before
  checking for `BOUND_CONSTANT` underneath. Used for window frame bounds
  (`start_expr`/`end_expr`) and LAG/LEAD offsets — the shapes verified
  against the render/incremental machinery for this fix. NTILE's bucket
  count and NTH_VALUE's `N` deliberately keep the old strict bare-constant
  check (`bare_constant_int`, unchanged behavior, still gated): the same
  `BOUND_CAST` shell reaches them, but a differential check found NTILE's
  bucket-boundary math already diverges from stock DuckDB for uneven
  partition sizes (pre-existing, out of scope here) — widening the gate
  there would have silently shipped a broken feature instead of a loud
  "unsupported" error.
- **Test**: `test/integration/test_advanced_window.cpp` — differential
  checks (materialized view vs. stock DuckDB, exact values) for the
  bounded-AVG-frame and `LAG(v,12)` cases, each run before and after (a) a
  mid-window value UPDATE and (b) a partition-growing INSERT that pushes
  earlier values outside later rows' bounded frames (the fast re-emit path
  and the full structural-rebuild path, respectively) — plus a regression
  guard that `LAG(v)`'s default offset still works, and a scope-boundary
  test asserting NTILE/NTH_VALUE remain gated. Confirmed both new cases
  failing with `DBSP-E110` on the pre-fix code, passing after.

## Fix: silent-correctness bug — WHERE+aggregate join subquery side returned empty - Jul 2026

- **Bug**: a materialized view whose `JOIN` had a subquery side containing
  both a `WHERE` filter and a `GROUP BY` aggregate (e.g. `... JOIN (SELECT
  k, MAX(v) m FROM t WHERE v IS NOT NULL GROUP BY k) x ON ...`) silently
  returned an **empty** result from initial materialization — no error, no
  fallback — while stock DuckDB returned the correct rows.
- **Root cause**: `plan_ir::fuse_map_cols` (`include/dbsp_plan_translator.hpp`)
  elides a `MAP_COLS` (column-selection) node beneath its parent and remaps
  the parent's own bound-ref expressions through the eliminated node's
  column mapping. This is sound for `MAP_EXPR`/`FILTER_MAP`, whose `exprs`
  fully and independently define their output columns. It was *also*
  applied to a bare `FILTER_EXPR`, which is a pure passthrough — its output
  row layout **is** its input's layout, not something its own predicate
  list redefines. Eliding the `MAP_COLS` beneath a bare `FILTER_EXPR`
  correctly remapped the filter's own predicate, but silently changed the
  column order/width the filter now hands to its *parent*. When that
  parent was an `AGGREGATE` sitting directly above a `WHERE ... GROUP BY`
  filter (only reachable when the filter's own parent isn't a `MAP_EXPR` —
  otherwise `fuse_filter_map` already fuses it into a self-defining
  `FILTER_MAP` first), the aggregate's group-by key and aggregate-argument
  indices were left pointing at the pre-elision column positions —
  silently grouping/aggregating the wrong columns. In the repro this
  produced a `MAX` keyed and valued on swapped columns, so the outer join's
  equi-keys never matched and the view came back empty.
- **Fix**: `fuse_map_cols` no longer fires on bare `FILTER_EXPR` — only on
  `MAP_EXPR`/`FILTER_MAP`, whose output is self-defined and therefore safe
  against a reordering child being elided. A `WHERE ... GROUP BY` filter
  keeps its `MAP_COLS` node (one extra node, matching the
  `g_plan_ir_optimize=false` behavior, which was already correct); the
  fusion still fires for the common `SELECT ... WHERE ...` case where the
  filter is directly under a projection.
- **Test**: `planner C4: WHERE+aggregate join subquery side stays exact`
  (`test/integration/test_planner_frontend.cpp`) — initial materialization
  against hand-computed truth, plus an incremental follow-up (insert a new
  per-group max, then update it away) verified against DuckDB's own answer.
  Confirmed failing (0 rows vs. 2) on the pre-fix code, passing after.

## Feature: signed incremental deltas for linear UNION ALL recursion - Jul 2026

- **Motivation**: profiling the NumPad wfp roll-forward edit path
  (`DBSP_TIMING`) attributed ~590ms of the ~597ms per-commit sync to ONE
  recursive view (`v_closingheadcount`, 58,500 rows × 36 iterations): every
  retraction-bearing delta (every `UPDATE`) routed `PlanRecursiveNode::
  step()` to a full `recompute()`. The UNION/DRed alternative measures
  *worse* here (884ms) — rederive image-passes the full relation per
  fixpoint depth.
- **Insight**: a **linear** recursive step (the recursive relation
  referenced exactly once in the step) is linear in that relation —
  `step(R + δ) = step(R) + step(δ)` — so its fixpoint's output delta for an
  input delta δ is `Σᵢ stepⁱ(δ)`: the existing insert-only incremental path,
  run with **signed** weights. No deletion special-case is needed;
  retractions propagate through the same iteration as insertions.
- **Change**: `PlanRecursiveNode` gains `linear_step_`, detected at build
  time (the `REC_CTE` build site walks the step's `PlanOpSpec` subtree
  counting sentinel-table `SOURCE` refs — exactly one → linear-eligible).
  `step()` now skips the deletion special-case entirely when
  `union_all_ && linear_step_`: the incremental seed/`admit`/`iterate` path
  is generalized to signed weights (no more `w > 0` filter), `admit`'s
  UNION ALL branch does plain multiset arithmetic on `accumulated_`
  (`ZSet::insert` already erases a row whose running weight nets to zero —
  verified, not assumed), and a `max_iterations_` trip **or an exception**
  during admission/iteration on the signed path discards the attempt
  (restoring `accumulated_`/`output_` from a pre-admission snapshot) and
  falls back to `recompute()` — logged under `DBSP_DEBUG_SYNC` — rather
  than emit a partial delta.
- **Linearity veto (defense-in-depth)**: the planner already accepts
  `DISTINCT`/`GROUP BY`/`ORDER BY … LIMIT`/`WINDOW` inside a recursive
  step, and those shapes are pre-existing-wrong on every path (out of
  scope here — not newly broken by this change). But `AGGREGATE`,
  `DISTINCT`, `DISTINCT_ON`, `WINDOW`, `SORT_LIMIT`, and non-`UNION ALL`
  `SET_OP` anywhere in the step subtree now veto `linear_step_` even when
  the sentinel is referenced exactly once: these operators are not
  weight-linear (`step(R + δ) ≠ step(R) + step(δ)` — they collapse or
  reorder rows), so without the veto the new signed path would have
  wrongly claimed linearity for them. `admit`'s set-semantics (non-`UNION
  ALL`) branch is also now explicitly guarded on `w > 0` (previously
  implicit, relying on `has_deletion` never routing negatives there — now
  `iterate()` passes signed weights through unconditionally for the
  `union_all_` linear path, so the guard is explicit rather than
  incidental).
- **Unchanged by design**: nonlinear UNION ALL (recursive relation
  referenced ≥2 times — weighted deletion through a self-join doesn't
  distribute) and UNION (set-semantics, still DRed) keep their existing
  full/DRed paths bit-for-bit; insert-only behavior on every shape is
  unchanged. `recompute()` is retained as the differential-test oracle for
  all three paths.
- **Test hooks**: `dbsp_native::g_recompute_invocations`, an inline atomic
  counter incremented in `recompute()` (same directly-test-accessible
  convention as `g_plan_ir_optimize`/`g_intraop_shards`, no new SQL
  surface), lets integration tests assert an `UPDATE`/`DELETE` on a linear
  UNION ALL recursive view does not fall back to full recompute.
  `DBSP_REC_MAX_ITER` (env var, read once at `PlanRecursiveNode`
  construction) lets tests force a small iteration cap to exercise the
  `max_iterations_` fallback deterministically.
- Regression coverage: `test/integration/test_recursive_deletion.cpp`
  gained a wfp-shaped (per-partition running-total chain) value test, a
  routing-assertion test (`g_recompute_invocations` before/after delta), a
  100-round randomized UPDATE/DELETE/re-seed differential against the same
  oracle-comparison harness the existing DRed tests use, and a
  `DBSP_REC_MAX_ITER`-forced fallback test asserting both that recompute()
  fires AND that its recovered result is correct.
- Design: `docs/superpowers/specs/2026-07-31-linear-recursion-deltas-design.md`.

## Fix: checkpoint declines a stale view-definition restore - Jul 2026

- **Gap**: checkpoint restore only guarded against source-table drift (the
  row-count/hash watermark) and blob-layout drift (the format version),
  not against the view's own *definition* changing. `dbsp_replace_view` /
  `CREATE OR REPLACE MATERIALIZED VIEW` (or a same-name drop+recreate)
  followed by an unclean close (crash, autopersist off, a failed
  auto-save) could leave a `_dbsp_views` row with the *new* SQL sitting
  next to `_dbsp_ckpt` node/sink blobs still holding the *old* view's
  operator state and sink result. The next load would cold-create the
  view from the new SQL and silently inject the old blobs on top —
  serving stale data forever, since incoming deltas never retract a wrong
  sink row they never produced.
- **Fix**: `save_checkpoint` now writes an additional `kind='sql'` row per
  checkpointed view, holding that view's exact SQL at save time. On load,
  `checkpoint_valid` returns this per-view fingerprint; `load_from_duck_
  table` compares it against the SQL it is about to use to cold-create the
  view and only takes the checkpoint fast path when they match. A
  mismatch declines the fast path for *that view alone* — it rebuilds by
  replay from current table data — while every other view's checkpoint
  restore is unaffected. Crash recovery (`src/dbsp_recovery.cpp`
  `load_views`) shares `load_from_duck_table` and gets the same guard for
  free.
- **Belt-and-braces**: `replace_view` now also best-effort deletes the
  `_dbsp_ckpt` rows for the entire subtree it just dropped (the replaced
  view plus its transitive dependents), closing the staleness window
  earlier rather than waiting on the next full `save_checkpoint()` (which
  already rewrites `_dbsp_ckpt` from scratch) — the fingerprint check
  above is the correctness backstop either way.
- **Checkpoint format version bump (v2 → v3)**: the blob set changed
  shape (new `sql` rows), so a v2 checkpoint — which has none — is treated
  as wholesale absent and rebuilt by replay once, same one-time cost as
  the v1 → v2 bump.
- Dead code removed: `CDCManager::auto_sync_all` had zero callers (a
  leftover from an earlier checkpoint-interval design); the actual
  checkpoint-interval deferral runs through the `TransactionCommit` hook's
  `CheckpointGuard` and `dbsp_sync`, corrected in the note below.
- Regression coverage: `test/integration/test_join_checkpoint.cpp` gained
  a stale-fingerprint test (view SQL changed underneath an unrefreshed
  checkpoint must rebuild from the new SQL, not serve the old blob) and a
  RIGHT-join save/restore round-trip test mirroring the existing LEFT one.

## Feature: LEFT/RIGHT outer-join checkpoint state - Jul 2026

- Circuit-state checkpointing (D3b) now covers `LEFT` and `RIGHT` outer
  joins, not just `INNER`: `PlanJoinNode::state_kind()` reports
  `SERIALIZABLE` for non-spilled, non-mark LEFT/RIGHT joins, so pad-carrying
  join views restore from a checkpoint instead of rebuild-by-replay on
  load. `FULL` (`OUTER`) and `MARK` joins, and any spilled join side, stay
  `UNSUPPORTED` this pass.
- `serialize_state`/`restore_state` gained `left_pad_`/`right_pad_` (the
  currently-emitted NULL-pad weight per row) alongside the existing
  equi-key indexes and per-row weight totals — exactly what
  `reconcile_pads` reads to compute the correct pad diff on the first
  post-restore delta. `right_total_`/`right_null_` are MARK-only
  bookkeeping and stay unserialized (MARK joins are still UNSUPPORTED).
- **Checkpoint format version bump**: the blob layout changed, so a new
  `_dbsp_ckpt_version` table now travels with every checkpoint.
  `checkpoint_valid` treats a missing or mismatched version as no
  checkpoint at all (rebuild-by-replay) rather than risk misparsing an
  older blob. Practical effect: **the first `dbsp_load()` against a
  database checkpointed by a pre-upgrade build pays one rebuild-by-replay,
  then re-checkpoints in the new format on the next save** — no data loss,
  no crash, just a one-time cost.

## Feature: dbsp_replace_view + CREATE OR REPLACE MATERIALIZED VIEW - subtree-only repopulation - Jul 2026

- New `dbsp_replace_view(name, sql)` table function and
  `CREATE OR REPLACE MATERIALIZED VIEW name AS <query>` DDL (routes to
  plain create when `name` doesn't exist yet, to replace when it does):
  change one view's definition and rebuild only it and its transitive
  dependents — upstream sources and every other view are left untouched,
  and each dependent's own DDL is preserved verbatim (only the named view
  gets the new SQL).
- Algorithm: verify the view exists (else `DBSP-E206`); snapshot
  `(name, sql)` for every transitive dependent via the dependency graph's
  `topological_order()` (already filters to the dependent set and orders
  it so a dependent's in-set dependencies precede it); drop the whole
  subtree through the existing `drop_view_cascade` path; recreate the
  named view with the new SQL, then recreate each dependent with its
  saved SQL in that order. Recreation replays each view from its sources'
  current materialized state, so cost is proportional to the subtree, not
  the whole model.
- Failure semantics (v1, no transactional rollback): if a create fails
  mid-sequence, the remaining dependents are still attempted with their
  saved old DDL, and the error (`DBSP-E304`) lists every failure plus
  which views are still queryable afterward — the registry itself is
  never left in a half-registered/inconsistent state.
- `drop_view_cascade` is called without a `ClientContext` here (matching
  every other cascade-drop call site in the file): passing one would call
  `context->Query()` to delete `_dbsp_views` rows while already running
  inside a table function on that same context — a reentrant, deadlocking
  call. Skipping it is harmless: `create_view`'s own `INSERT OR REPLACE`
  (gated on `dbsp_autopersist`, same as a plain `CREATE`) restores the
  `_dbsp_views` row for every view recreated below.

## Feature: dbsp_autopersist - views survive a clean reopen - Jul 2026

- New `dbsp_autopersist(enable)` setting, **ON by default**: a database
  that used DBSP views now reopens with them alive, with no explicit
  `dbsp_save()`/`dbsp_load()` calls. Auto-save runs on a clean connection
  close (the existing crash-marker release hook — the last user connection
  to an instance closing) when autopersist is on and any view exists;
  auto-load runs at the first DBSP entry point with a `ClientContext` in a
  fresh session (`dbsp_query`, `dbsp_views`, `dbsp_track`,
  `CREATE MATERIALIZED VIEW`, `dbsp_sync`, `dbsp_changes`) if the view
  registry is still empty — the existing checkpoint-fast-path `dbsp_load()`
  machinery underneath is unchanged. Fires at most once per session and
  never after a view has already been created here (`CDCManager::
  maybe_autoload`/`autoload_attempted_`). `dbsp_autopersist_interval(n)`
  (default 0 = off) additionally piggybacks a circuit-state checkpoint
  every `n` commits, so a crash between clean shutdowns loses at most `n`
  commits of operator state — the data itself is never at risk (base
  tables are DuckDB-durable; a stale checkpoint fails its watermark check
  and rebuilds).
- Auto-save could not run inline in the `OnConnectionClosed` hook: it's
  called from inside `ConnectionManager::RemoveConnection`, which holds
  `connections_lock`, and `save_to_duck_table`/`save_checkpoint` each build
  a fresh `duckdb::Connection` to do their work — whose constructor calls
  `AddConnection`, re-locking the same non-recursive mutex on the same
  thread. It now runs on the same detached thread that already tears down
  the manager (destroying views does the same thing for the same reason),
  ordered before that teardown; the manager's own views keep the
  `DatabaseInstance` alive for the save without an extra reference, and a
  same-process reopen already busy-spins until that instance is fully
  destroyed (`DBInstanceCache`), which serializes the reopen after the
  save completes. The checkpoint-interval counter is incremented inside
  `propagate_changes` (lock-free), but the actual `save_checkpoint()` call
  is deferred to a point after the caller's `struct_mutex_` is released
  (the `TransactionCommit` hook's `CheckpointGuard` destructor, and
  `dbsp_sync`) for the identical reason — `propagate_changes` runs under
  callers' shared lock on `struct_mutex_`, and `save_checkpoint` takes its
  own shared lock on it. (`CDCManager::auto_sync_all`, an earlier
  candidate for this deferral point, was dead code — no caller ever
  reached it — and has been removed; the two call sites above are the only
  live ones.)
- `autoload_attempted_` is an atomic (compare-exchanged in
  `maybe_autoload`), not a plain bool: two connections to the same
  `DatabaseInstance` can race their first DBSP call. A failed auto-load
  logs `last_error()` under `DBSP_DEBUG_SYNC` (the existing debug var this
  file already used for other best-effort CDC diagnostics); a failed or
  thrown auto-save on close logs under `DBSP_DEBUG_TEARDOWN` (the existing
  crash-marker/teardown debug var, now also captured into the detached
  save thread).
- Regression test: test/python/test_autopersist.py.

## Fix: same-pass multi-source deltas apply as one circuit step - Jul 2026

- A view fed by two sources updated in the SAME propagation pass (join of
  two sibling MVs over one base table) received its deltas as two separate
  circuit steps. Each step ran against post-delta shared arrangements, so
  PlanJoinNode's both-shared bilinear correction (emit −Δl⋈Δr) never
  fired: the join overcounted Δl⋈Δr on every commit — duplicate,
  conflicting rows per key accumulated unboundedly (quadratic growth) and
  stale rows were never retracted. Base-table multi-writes were unaffected
  (separate propagation passes are sequentially correct); the bug was
  MV-cascade-only.
- Fix: `NativeMaterializedView::apply_changes_batch` — the CDC cascade
  now hands a view ALL of its pending source deltas at once;
  `PlannedCircuitView` pushes every input and steps the circuit once, so
  the in-step correction fires. Default implementation preserves
  sequential per-source behavior (with delta-union) for legacy views.
  `propagate_changes`'s per-view apply loop and multi-source union arena
  are replaced by the batched call.
- Found by NumPad's full-SQL/DBSP spike (view-vs-oracle parity + row-count
  sentinels); regression-tested in test_cascading_views.cpp ("Join of two
  sibling MVs stays exact across repeated updates").

## Perf/coverage: spill codec 12x + bounded percentage limits - Jul 2026

- Spill row codec: duckdb's BinarySerializer cost ~650ns/row with a
  fresh MemoryStream per row. New typed fast-path codec (tag byte + raw
  payload for INTEGER/BIGINT/DOUBLE/VARCHAR/BOOLEAN and primitive typed
  NULLs; BinarySerializer as the length-prefixed fallback for everything
  else) with a reused scratch buffer: serialize 12x faster, deserialize
  3.8x, files 2.1x smaller. Spill files are process-disposable, so the
  format change carries no compatibility burden. Spilled RESYNC drops
  from ~2.8x of the RAM path to ~1.3x — spill mode is now nearly free at
  steady state; roundtrip pinned across every value shape incl. nested
  types and typed NULLs.
- LIMIT p% views join spill mode: the percentage cutoff needs the total
  input count, which previously forced the full sorted multiset into
  RAM. total_weight_ now carries the count as a scalar; the bounded
  window cap tracks the moving cutoff and the existing overflow-log
  refill absorbs cutoff growth (and shrink-on-delete). Window stays
  ~2x the result size regardless of table size (unit-tested incl.
  deletion-driven refill).
- Plain ORDER BY / window views documented as the true memory floor:
  the result Z-set returned by reference IS the answer, and payloads
  are COW-shared with upstream state — recorded in TODO.md with the
  weight-collapse follow-up.

## Perf: joins 2x + CI - Jul 2026

- Join-path profiling repeated the filter story: MAP_COLS survives under
  joins (fusion can't elide it — the join needs the projected layout) at
  56ms/100k, and plan_join spent ~half its 134ms lazy-hashing concat
  output rows. New PlanMapColsNode (typed column-map chunk fill +
  vectorized output hashes, COW value copies) and buffered inner-join
  emits with pre-seeded hashes: join delta 454k -> 925k rows/s,
  aggregate throughput 4.1M rows/s, filter overhead vs a hand lambda
  ~1.2x. Sharded probes stay on the lazy path and now lose to serial at
  100k scale (noted in DESIGN_DATA_PLANE.md).
- CI: GitHub Actions (build + engine-assumption canaries first + full
  ctest + bench gates), duckdb tree converted to a proper submodule
  pinned at upstream v1.5.4. First run green in ~16 minutes.

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

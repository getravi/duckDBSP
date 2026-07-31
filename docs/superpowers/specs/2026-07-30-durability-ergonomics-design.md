# Durability & View-Lifecycle Ergonomics — Design

**Date:** 2026-07-30
**Status:** Approved (verbal, session) — build in order 1 → 2 → 3
**Motivation:** NumPad's full-SQL spike surfaced three gaps: MV state
silently dies on connection reopen (two-call `dbsp_save`/`dbsp_load`
contract nobody calls), changing one view definition has no subtree-rebuild
ergonomics (`dbsp_drop_cascade` destroys dependents and loses their DDL),
and checkpoint restore rebuilds exactly the expensive views (LEFT joins,
recursion, windows are UNSUPPORTED node states — wfp's four costliest views
all replay on load).

## Feature 1: Auto-persist

**Goal:** a database that used DBSP views reopens with them alive, no
explicit calls.

- New setting function `dbsp_autopersist(enable BOOLEAN)` — default **ON**.
  Query form `dbsp_autopersist()` reports state.
- **Auto-load:** on the first DBSP entry point that has a `ClientContext`
  (same trigger discipline as existing lazy paths — extension Load() has no
  client), if `_dbsp_views` exists in the default catalog and the view
  registry is empty, run the `dbsp_load()` path (checkpoint fast path
  included, watermark-validated, per-view rebuild fallback as today).
  Never auto-load twice; never auto-load after the user has created any
  view in this process; a failed auto-load surfaces as a warning on the
  triggering call, not an error.
- **Auto-save:** on clean shutdown (the existing crash-marker release hook
  in `dbsp_extension.cpp`), if autopersist is on and any tracked view
  exists, run `dbsp_save()` + `save_checkpoint()`. Also piggyback a
  checkpoint after `dbsp_sync`-driven propagation every N commits
  (N = 0 = off by default; explicit setting `dbsp_autopersist_interval(N)`)
  so a crash loses at most N commits of operator state — the data itself is
  never at risk (base tables are DuckDB-durable; stale checkpoints fail the
  watermark check and rebuild).
- Bulk-load workflows: `dbsp_autopersist(false)` before, `(true)` after —
  same shape as `dbsp_auto_sync`.

## Feature 2: `dbsp_replace_view(name, sql)`

**Goal:** change one view's definition; only it and its transitive
dependents repopulate — upstream state untouched, dependent DDL preserved.

- Table function `dbsp_replace_view(name VARCHAR, sql VARCHAR)`:
  1. Verify `name` exists (else DBSP-E2xx unknown-view error).
  2. Snapshot the DDL (name, sql) of every transitive dependent from the
     registry, in topological order.
  3. Drop the subtree (existing cascade machinery).
  4. Create `name` with the new SQL, then recreate each dependent with its
     saved SQL, topo order. Initialization replays upstream **materialized
     results** (existing `views_[source]->get_result()` path), so cost =
     subtree only.
  5. On any creation failure mid-sequence: continue recreating the
     remaining dependents with their old DDL where possible, and return an
     error listing what failed and what state the registry is in. No
     attempt at full transactional rollback in v1 — the failure report must
     be complete instead.
- Also support the parser-extension spelling
  `CREATE OR REPLACE MATERIALIZED VIEW name AS <query>` mapping to the same
  path when `name` already exists (plain CREATE when it doesn't).

## Feature 3: LEFT/RIGHT outer-join checkpoint state

**Goal:** stop rebuild-by-replay for the pad-carrying join shapes that
dominate wfp's cold cost (~2s of the 6.15s head).

- `PlanJoinNode::state_kind()`: allow `JoinType::LEFT` and `RIGHT` (keep
  FULL and mark joins UNSUPPORTED in this pass; spilled state stays
  UNSUPPORTED).
- Serialize, in addition to the existing INNER equi-key indexes: the
  per-row weighted match counters / pad bookkeeping (`left_weights_` /
  `right_weights_`, `left_pad_` / `right_pad_`, and the total/null
  counters) — whatever `reconcile_pads` needs to resume exactly.
- Restore must round-trip: after `restore_circuit_state`, an incremental
  delta must produce byte-identical output to a never-checkpointed view
  (test: build → edit → save → reopen → restore → edit → compare against a
  continuously-live twin).
- Non-goals this pass: recursion state (PlanRecursiveNode), window/sort
  embedded views, value-collecting aggregates. Recorded as the next two
  levers (~1.2s and ~0.5s on the wfp profile).

## Testing

- Feature 1: python round-trip test (create → close cleanly → reopen →
  views alive and correct; crash-sim path still rebuilds; autopersist(false)
  honored). `test/python/` follows existing patterns.
- Feature 2: C++ integration test in `test_cascading_views.cpp` — replace a
  mid-chain view, assert downstream values reflect the new definition,
  upstream untouched (timing not asserted), dependents' DDL survived; error
  path (bad SQL) reports and leaves registry queryable.
- Feature 3: C++ unit/integration — LEFT-join view checkpoint round-trip
  equality vs live twin, incl. pads changing after restore (row gains its
  first match post-restore; row loses its last match post-restore).
- Everything: full ctest suite stays green.

## Documentation (repo rule)

CHANGELOG.md entry per feature; API.md (`dbsp_autopersist`,
`dbsp_replace_view`, CREATE OR REPLACE); ARCHITECTURE.md checkpoint
section (state-kind table gains LEFT/RIGHT); README feature list.

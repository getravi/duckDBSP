# Phase D Plan: Embeddable Instance Model

**Goal:** make duckDBSP safe and fast for embedded hosts that open, close,
attach, and detach databases in one process (the NumPad/NuEPM calc engine is
the driving consumer). Three workstreams, strictly ordered: per-instance
state (D1), attached-catalog support (D2), materialized-state persistence
(D3). A delta read-back API (D4) rides along for hosts that need old/new
values per sync.

**Prerequisite (done, Jul 2026):** instance teardown on last user connection
close — `InstanceRegistry` + `OnConnectionClosed` callback + detached-thread
`release_instance_state`. Fixed the same-process reopen hang
(`test/python/test_reopen_hang.py`).

**Motivating spike numbers (1M-cell table, 4-level hierarchy):**
- notify-path incremental update: 6.9 ms/edit (correct to 1e-9)
- auto-sync scan-diff: 2.4 s/edit — why D4 matters for hosts with known deltas
- view rebuild on open (`dbsp_load`): 4.8 s — why D3 exists

## D1 — Per-instance CDCManager

One `CDCManager` per `DatabaseInstance`, not one per process.

- Registry: `unordered_map<DatabaseInstance *, unique_ptr<CDCManager>>`
  behind a mutex, in the (still deliberately leaked) global holder. Entry
  created on first use, erased by the existing last-connection teardown —
  `release_instance_state` becomes "erase the entry".
- Resolution: every call site has a `ClientContext` (31 sites, 4 files:
  dbsp_extension.cpp, dbsp_recovery.cpp, dbsp_context_state.hpp,
  dbsp_cdc.hpp). `get_cdc_manager()` gains a required
  `DatabaseInstance &` (or context) parameter; the zero-arg form is deleted
  so the compiler finds every site.
- `bound_instance_` and the rebind-on-second-instance limitation disappear:
  the map key IS the instance.
- Teardown thread owns the erased entry and destroys it off the
  connections_lock path (same deadlock rule as today).
- Tests: two file DBs open simultaneously in one process, views on each,
  edits interleaved, no cross-talk; reopen test still passes.

## D2 — Attached-catalog tables as view sources

`CREATE MATERIALIZED VIEW ... FROM m.li_1` currently fails ("Could not find
source"). Two halves:

1. **Naming.** Tracked-table keys become canonical qualified names
   (`catalog.schema.table`), derived from the bound plan's `LOGICAL_GET`
   (TableCatalogEntry has catalog + schema), never from SQL text. Unqualified
   user input resolves through the binder, so `li_1` and `m.li_1` map to the
   same key. All internal SQL (schema introspection, scan-diff, COUNT guards)
   emits quoted qualified names.
2. **CDC scope.** Commit hooks receive the MetaTransaction, which spans
   attached catalogs; captured-delta classification (dbsp_context_state)
   must attribute INSERT/UPDATE/DELETE targets to qualified keys. DETACH of
   a catalog with tracked tables drops the affected views + tracking
   (hook: catalog removal — investigate `AttachedDatabase` teardown path;
   fallback is erroring the next sync with a clear DBSP error).
- Tests: MV over attached table (the failing spike case), edit-after-attach
  propagation, detach-with-views behavior, same-name tables in two catalogs.

## D3 — Materialized-state persistence

> **Status Jul 2026:** definitions-in-DuckDB-tables landed (zero-arg
> dbsp_save/dbsp_load, _dbsp_views in the database file, column-name
> fidelity incl. positional GROUP BY). O(state) restore did NOT land:
> `set_result` only fills the sink; internal operator state (aggregate
> groups, arrangements, top-K, distinct counts) would be empty and later
> deltas would compute wrong increments. True O(state) restore requires
> per-node-type circuit checkpointing — tracked below as **D3b**, the
> remaining open workstream.

Today `_dbsp_views` persists definitions; state is rebuilt by full
recompute on load (4.8 s at 1M rows). Persist the integrated result so open
cost is proportional to state size, not source size.

- Storage: per view, a `_dbsp_state_<view>` table in the SAME database file
  (travels with file copies/backups); one `_dbsp_meta` row records source
  table versions (rowid watermarks or count+hash) at save time.
- Write path: `dbsp_save()` writes state tables inside one transaction via
  an internal connection under `InternalQueryGuard` (no commit-hook
  recursion). Explicit save first; auto-save-on-close is a later flag once
  proven (close-path work must stay off connections_lock).
- Read path: `dbsp_load()` restores each view's integrated z-set from its
  state table, then reconciles: if source watermarks match, done (O(state));
  if not, scan-and-diff each source once (current behavior as fallback).
- Correctness: restored view must be bit-identical to rebuilt view —
  differential test compares both paths after randomized edit sequences.
- Fixes ride along: restored views currently lose GROUP BY column names
  ("0", "1") — persist the output schema with the definition.

## D4 — Delta read-back API

`dbsp_changes('view')` table function: rows added/removed by the most recent
sync of that view, as (columns..., weight) with +1/-1 weights. Hosts that
track their own edits (NuEPM Delta log, undo/redo, WebSocket broadcast) read
exactly what changed without diffing snapshots.

- Implementation: each view already produces a per-sync output delta z-set
  during propagation; retain the last one per view (bounded: one generation)
  and expose it. Cleared on view drop; empty result when no sync has run.
- Contract: single-generation buffer — callers consume between syncs (matches
  the notify-path usage pattern; document it).

## Ordering & verification

D1 → D2 → D3, D4 anytime after D1. Each workstream lands with: new C++ or
Python regression tests, full ctest green, differential soak
(soak_differential) for D2/D3, and the doc checklist from CLAUDE.md
(ARCHITECTURE.md, API.md, README.md, CHANGELOG.md).

Exit criterion for the NuEPM consumer: open model file → dbsp_load →
edit via notify → read dbsp_changes → close, at interactive latency
(open < 100 ms at 1M rows, edit < 10 ms), two models open concurrently.

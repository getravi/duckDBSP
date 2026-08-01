# Lazy Per-View Restore + NTILE Fix — Design

**Date:** 2026-07-31
**Status:** Approved (user directive). Build protocol: build-free
implementation, ONE batched build/verify cycle at the end.

## Feature 1: Lazy per-view checkpoint restore

**Motivation:** Warm reopen is 2.5s, ~1.7s of it blob decode for 43 views
the user may not touch for minutes. Goal: reopen returns in ~ms; each
view's state decodes on first need. Precedent to mirror: D3c deferred
baselines (`TrackedTable::is_deferred` + `materialize_deferred_locked` —
same lazy-materialize-on-first-need shape, already proven).

**Design:**
- `load_from_duck_table`'s checkpoint fast path, instead of decoding and
  injecting per view eagerly: cold-create the view (`skip_init_replay` as
  today), stash its pending blobs (node blobs + sink blob, already read
  from `_dbsp_ckpt` — stash the raw bytes, do NOT decode) in a CDC-side
  map `pending_restore_: name -> {nodes, sink}`, and mark it pending.
- `realize_pending_view(name)` (CDC): decode + `restore_view_state`
  equivalent injection; on failure → drop + recreate by replay (existing
  decline discipline). Called from EVERY surface that needs live state:
  - `dbsp_query` (before scan),
  - `propagate_changes` / `step_view` — a delta arriving for a pending
    view (or any of its pending ancestors) realizes it FIRST; deltas must
    never apply to un-restored state,
  - `save_checkpoint` (realize, or write the stashed blobs back verbatim
    — verbatim re-save is better: no decode cost; only valid if the view
    saw no deltas since stash, which "pending" guarantees),
  - `dbsp_replace_view` / drops (realize-or-discard, then proceed),
  - `get_result()`-consuming internals (arrangement registration reads?
    — audit callers of `get_result`/`get_delta` on freshly-loaded views).
- Arrangements: `register_arrangements`/backfill for a pending view's
  sources — audit whether backfill needs the view live (it reads TABLE
  state, not view state — verify) and whether a pending view's own output
  feeding another view's shared arrangement forces realization (MV-sourced
  arrangements: apply_to_arrangements on its delta only happens post-
  realize, but the ARRANGEMENT INITIAL population for a downstream
  consumer reads the pending view's result → must realize. Audit
  `arrangements_by_table_` initial fill for view-named sources.)
- Locking: realization follows `materialize_deferred_locked`'s discipline
  (who holds struct/view locks at each call site — derive per site, no
  new lock ordering).
- Setting `dbsp_lazy_restore(BOOLEAN)` default ON; OFF = today's eager
  behavior (bulk benchmarks, debugging).
- Report string: `dbsp_load` reports "N pending lazy restore" distinctly.

**Acceptance:** reopen→first `dbsp_query('one_view')` decodes ONLY that
view's chain (timing shows one blob_decode, not 43); an UPDATE against a
pending view's source realizes before applying (differential correctness
vs eager twin); save-after-partial-realization round-trips (verbatim
re-save of still-pending blobs); wfp: reopen ms-scale, first-touch per
view ~25-300ms, parity clean after touching everything.

## Feature 2: NTILE bucket algorithm fix + ungate

**Motivation:** NTILE's bucket-boundary math diverges from stock DuckDB
(found during the constant-frame work); currently unreachable only
because `bare_constant_int` rejects even literal bucket counts.

**Design:**
- Fix `NativeWindowView`'s NTILE: stock semantics — for `p` rows and `n`
  buckets: first `p % n` buckets hold `p/n + 1` rows, remainder hold
  `p/n`; bucket assigned by row position in partition order. Differential
  vs stock is the spec.
- Then ungate: NTILE (and NTH_VALUE if its machinery verifies clean —
  differential-test it; if NTH_VALUE is broken too, fix or keep it gated
  and say so) moves from `bare_constant_int` to the cast-unwrapping
  `constant_int`.
- Docs: API.md + TODO.md entries removed/updated to match.

**Acceptance:** NTILE(k) differential vs stock across: p % n == 0 and
!= 0, n > p (empty buckets), single-row partitions, post-edit
re-rendering (insert/delete changes p). All-suite green.

## Build protocol (binding)

Tasks 1-2 write code + tests with NO builds; fable-depth static review per
task substitutes for the compiler. ONE batched cycle at the end (full
suite + ctest + extension rebuild + wfp probes), then fixes if needed.

## Non-goals

Lazy realization of tracked-table baselines (already D3c), partial/
per-node lazy decode within one view, NTH_VALUE fix if it proves broken
(gate stays acceptable), format version change (stash/re-save uses the
existing v5 layout — no byte changes).

## Docs (repo rule)

CHANGELOG per feature; API.md (`dbsp_lazy_restore`, NTILE support);
ARCHITECTURE restore section.

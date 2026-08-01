# Restore Tail: Recursion + Window Checkpoint State — Design

**Date:** 2026-07-31
**Status:** Approved (user selection). Build constraint: full-suite/extension
rebuilds ONLY with explicit user sign-off — batch them.
**Motivation:** Warm reopen = 3.2s with 40/43 views restoring. The 3
holdouts rebuild by replay every open: `v_closingheadcount`
(PlanRecursiveNode — no state_kind override) and `v_cumulativehiresytd` /
`v_ytdpersonnelcost` (unbounded-window views behind EmbeddedViewNode — no
override). Goal: 43/43 restore; replay path remains the fallback.

## Feature 1: PlanRecursiveNode checkpoint state

- `state_kind()`: SERIALIZABLE iff `union_all_` AND every node of the
  nested `step_view_` circuit is SERIALIZABLE/STATELESS (reuse
  `PlannedCircuitView::checkpointable()`); UNION/set-semantics recursion
  (DRed support structures) stays UNSUPPORTED this pass.
- `serialize_state`: `accumulated_`, `anchor_total_`, `base_totals_`
  (name-keyed), plus the nested step_view circuit state via its existing
  `serialize_circuit_state` (embed as a length-prefixed sub-blob keyed by
  inner node id). Serialize exactly what `step()`/`recompute()` read to
  resume — read the code, derive the list, justify each field in the
  report (the join-pad and MIN/MAX precedents).
- `restore_state`: mirror; any inner-node mismatch → false → whole-view
  rebuild (existing decline discipline).
- Format version 4 → 5.

## Feature 2: Window view checkpoint state (EmbeddedViewNode)

- `EmbeddedViewNode::state_kind()`: delegate to a new capability on the
  wrapped view (`NativeWindowView` first; sort/limit/distinct-on wrapped
  views stay UNSUPPORTED unless trivially identical — do NOT widen scope
  beyond NativeWindowView in this pass).
- Serialize what `apply_changes`' incremental path needs to resume
  (partition orderings / cached rows / rendered outputs — derive from
  code, same discipline). Restore must round-trip: post-restore deltas
  produce byte-identical results vs a continuously-live twin, including
  the fast path and the structural full-render path.

## Testing

- Twin round-trip tests per feature in `test_join_checkpoint.cpp` (the
  harness lives there): recursion — post-restore retraction mid-chain +
  re-derive, exact equality (the linear signed-delta path must resume
  from restored state, and a fallback-to-recompute after restore must
  also work: test both); window — post-restore edits hitting the
  incremental fast path AND a structural (size-changing) delta.
- Existing version-gate test covers v4→v5 invalidation (version-agnostic).
- wfp validation: 43/43 restored, parity clean, reopen time recorded.
- Full ctest green (batched — see build constraint).

## Build protocol (binding on implementers)

Single-target test builds are allowed freely. FULL-suite rebuilds
(`make` all in test/build_test), `ctest` runs that require them, and
loadable-extension rebuilds (`build/`) are BATCHED to the end of the
plan and run only after the controller confirms with the user.

## Non-goals

Lazy per-view restore (separate backlog lever, ~1s decode amortization);
DRed-recursion state; sort/limit/distinct-on embedded views.

## Docs (repo rule)

CHANGELOG per feature; ARCHITECTURE state-kind coverage; format bump note.

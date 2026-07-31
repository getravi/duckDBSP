# Incremental Deletions for Linear UNION ALL Recursion — Design

**Date:** 2026-07-31
**Status:** Approved (session directive: "attack the sql sync floor")
**Motivation:** Profiling the NumPad wfp edit path (DBSP_TIMING) attributes
~590ms of the ~597ms per-commit sync to ONE view: the recursive roll-forward
(`v_closingheadcount`). Every retraction-bearing delta (every UPDATE) routes
`PlanRecursiveNode::step()` to `recompute()` — a full fixpoint over the
58,500-row relation × 36 iterations. The UNION/DRed alternative measures
*worse* (884ms: rederive image-passes the full relation per fixpoint depth).
All 26 other views combined cost ~5ms.

## The insight

For a **linear** recursive step (the recursive relation referenced exactly
once) under **UNION ALL** (multiset) semantics, the step operator is linear
in the recursive relation: `step(R + δ) = step(R) + step(δ)`. Therefore the
fixpoint's output delta for an input delta δ is `Σᵢ stepⁱ(δ)` — the existing
insert-only incremental path, run with **signed** weights. No deletion
special-case is needed; retractions propagate through the same iteration as
insertions, with negative weights.

- Divergence risk (a δ cycling forever) is identical to the existing
  insert-only path's and is already bounded by `max_iterations_` + sentinel.
- **Nonlinear** steps (recursive relation referenced ≥2 times):
  `step(R+δ) ≠ step(R) + step(δ)` — keep `recompute()`.
- **UNION (set semantics)**: admission/support counting doesn't distribute
  over signed weights — keep DRed, unchanged.
- `recompute()` is retained untouched as the differential-test oracle (the
  code already documents this role).

## Changes

1. **Linearity detection (build time).** Where the planner constructs
   `PlanRecursiveNode` (plan translation of WITH RECURSIVE), count
   references to the recursive working relation (the `sentinel_` table) in
   the step plan. Exactly one → `linear_step_ = true` on the node. Zero or
   ≥2 → false. The count must be over the *step* subtree only (anchor
   doesn't matter).
2. **Signed incremental path.** In `step()`: when `union_all_ &&
   linear_step_`, skip the `has_deletion` branch entirely — run the
   incremental path for every delta. Generalize it for signed weights:
   - seed = anchor delta + step-view deltas from base-input changes, with
     weights as-is (no `w > 0` filter);
   - `admit`/accumulation becomes plain multiset weight arithmetic on
     `accumulated_` (insert signed weight; a row reaching weight 0 is
     erased); every admitted signed change joins the frontier and the
     output delta;
   - iterate: push the frontier through `step_view_` (signed weights flow
     through join/filter/project nodes natively — all are weight-linear),
     admit its output delta, repeat until the frontier is empty or
     `max_iterations_` trips (then fall back to `recompute()` and log under
     `DBSP_DEBUG_SYNC`).
   - `step_view_`'s internal state must integrate the signed deltas exactly
     as it integrates positive ones today (`apply_changes` already does —
     verify, don't assume).
3. **Fallback correctness.** If anything in the signed path fails
   structurally (max-iterations, exception), fall back to `recompute()` in
   that same step — never emit a partial delta.

## Testing

- **Differential tests (the core requirement):** randomized edit sequences
  (inserts, updates, deletes on anchor and base inputs) against a linear
  UNION ALL recursive view; after every commit, compare the incremental
  node's accumulated result against a fresh `recompute()` (the in-code
  oracle) — exact multiset equality. Include: retraction that empties a
  chain link mid-sequence, re-seed after full deletion, delta that touches
  multiple partitions, and an edit sequence on the wfp roll-forward shape
  (time-spine chain).
- **Linearity gate tests:** nonlinear step (self-join of the recursive
  relation) still routes deletions to `recompute()`; UNION recursion still
  routes to DRed; insert-only behavior unchanged on all shapes.
- **Existing suites green:** full ctest (recursive + recursive-deletion
  integration tests are the regression net) + NumPad spike parity clean.
- **Benchmark:** wfp 300-employee driver re-run; expect SQL expensive-edit
  p25 to drop from ~475ms to the low tens of ms. Delta counts must stay
  identical (same logical output).

## Non-goals

- Checkpointing recursion state (separate lever, PlanRecursiveNode stays
  UNSUPPORTED state-kind).
- Nonlinear UNION ALL incremental deletions (theory exists — differential
  dataflow — but not this pass).
- DRed performance for set-semantics recursion.

## Docs (repo rule)

CHANGELOG; ARCHITECTURE.md WITH RECURSIVE paragraph (linear signed-delta
path); THEORY.md if it discusses recursion incrementality.

# Incremental Recursive Deletion (DRed)

**Date:** 2026-07-07
**Status:** Design approved, pending implementation plan
**Scope:** `PlanRecursiveNode` in `include/dbsp_plan_translator.hpp`

## Problem

`PlanRecursiveNode::step()` maintains a `WITH RECURSIVE` view incrementally
for insert-only deltas. Any negative-weight input (a deletion in the anchor
or a base table) currently falls back to `recompute()`: it re-runs the entire
fixed point from integrated inputs (`anchor_total_`, `base_totals_`) and emits
the diff against the previous `accumulated_`.

That is correct but O(fixed point), not O(Δ). It is the last remaining
incremental-correctness hole in the engine — cascades (Phase E1) and aggregate
retraction are already incremental.

The difficulty is intrinsic to recursion: a derived row may be supported by
many derivation paths, including cycles. Deleting one base fact does not
locally determine whether a derived row dies or survives via another path.

## Approach: DRed (Delete-Rederive)

DRed is the standard datalog algorithm for incremental recursion under
deletions. It is correct for **set semantics** (`UNION` / distinct), cycles
included.

**UNION ALL (multiplicity) recursion is out of scope** and keeps the existing
`recompute()` path: weighted deletion in a cycle is ill-defined (path counts
can be unbounded) and the case is rare. This split is documented in code and
in `TODO.md`.

### Algorithm

Entered from `step()` when `!union_all_ && has_deletion`. New method
`dred(const DuckDBZSet &anchor_delta, base_deltas)`.

1. **Seed retractions.** Collect negative-weight rows to retract:
   - direct anchor deletions (`w < 0` in `anchor_delta`);
   - `step_view_` reactions to base-table deltas — apply each base delta to
     `step_view_`, keep the negative rows from `get_delta()` as retraction
     seed. Hold the positive rows from the same deltas for step 3
     (rederive seed).

2. **Overdelete (retraction fixpoint).** Iterate a *negative* frontier
   through `step_view_`:
   - Apply the current retraction frontier to `step_view_`.
   - For each retracted row whose support in `accumulated_` drops to zero,
     remove it from `accumulated_`, emit `-1` to `output_`, and push it onto
     the next frontier so its dependents are reconsidered.
   - Stop when the frontier is empty (bounded by `max_iterations_`).

   This over-approximates: rows held up only by a now-broken cycle are pulled
   even if they have external support. Step 3 repairs that.

3. **Rederive.** Run one forward fixed point using the surviving
   `accumulated_` set *plus* the positive seed rows as the frontier:
   - For each row `step_view_` produces that is currently absent from
     `accumulated_`, re-admit it: add to `accumulated_`, emit `+1` to
     `output_`, push to the forward frontier.
   - Stop when no new rows appear.

   This restores every over-deleted row that still has an alternative
   derivation from surviving facts.

4. `has_output_ = !output_.empty()`.

`accumulated_` stays authoritative throughout. `output_` is built
incrementally (negatives during overdelete, positives during rederive), so
cost tracks the affected subgraph — no full O(accumulated) diff at the end.

### Why no support-counting

A per-row derivation count (decrement on delete, die at zero) is cheaper but
**wrong for cycles**: mutually-supporting rows keep each other's count above
zero with no external support (the well-founded problem). DRed's
overdelete-then-rederive is the correct handling for cyclic graphs and is the
reason it is the datalog standard.

## Integration

- `anchor_total_` / `base_totals_` retained: they are the integrated-input
  baseline the `step_view_` sub-circuit already tracks, and support the
  rederive fixpoint against current inputs.
- The `step_view_` sub-circuit (join + project, linear) propagates negative
  deltas exactly, so overdelete falls out of existing machinery.
- `recompute()` is retained: it remains the UNION ALL deletion path and
  serves as the differential-test oracle.
- Output/emit contract unchanged: `output_` is the signed delta consumed
  downstream; `has_output_` gates it.

## Correctness invariant

For any interleaved insert/delete sequence on a UNION recursive view, the
DRed-maintained `accumulated_` must equal a from-scratch fixed point over the
same integrated inputs. This is the test gate:

> **DRed result ≡ recompute result**, over randomized insert/delete sequences.

## Testing (TDD)

Property test is the gate. Write it first:

- Randomized insert/delete sequences over cyclic graphs (transitive closure,
  reachability). After each step, assert DRed `accumulated_` equals the
  full-recompute fixed point over the same inputs.

Targeted cases:

- Delete a cycle edge that removes reachability → dependent rows retracted.
- Delete an edge with an alternate path → dependent rows survive (overdelete
  then rederive brings them back).
- Delete the anchor seed row → downstream closure retracts.
- Multi-table step: deletion in a base table joined inside the step.
- Insert-then-delete round-trip returns to the pre-insert state.

## Risk

Pathological dense graphs (near-everything mutually supported) push rederive
toward full-recompute cost — same asymptotic worst case as today, never worse,
and much cheaper in the typical sparse case. `recompute()` stays available.

## Out of scope

- UNION ALL / multiplicity recursion deletion (keeps `recompute()`).
- `WITH RECURSIVE ... USING KEY` (already E110 at view creation).

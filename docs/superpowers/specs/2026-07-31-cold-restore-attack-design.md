# Cold Populate / Restore Attack — Design

**Date:** 2026-07-31
**Status:** Approved (session directive: "attack the cold populate")
**Motivation:** wfp full-coverage numbers: cold populate 15.5s, restore ~7.0s
(29/43 views from checkpoint). Profiling attribution:
- Cold: `v_rolling12mavgheadcount` 3.8s (range-join fan) + a broad ~37-view
  tail; restore rebuilds the 14 non-checkpointable views (10 LNB rewrites
  use MAX = value-collecting = checkpoint-UNSUPPORTED; recursion; 3 misc)
  plus ~2s of slow blob decode (851ms/58k rows ≈ 15µs/row on one view).
- Discovered along the way: the window frontend rejects CONSTANT bounded
  frames (`ROWS BETWEEN 11 PRECEDING`) and `LAG(x, 12)` as "non-constant"
  — `constant_int` only unwraps a bare `BOUND_CONSTANT`, but the binder
  wraps frame/offset literals (cast/expression shell). Bounded-frame
  support exists downstream (`def.start_offset` is consumed); only the
  gate misfires.
- Also: calling `dbsp_load()` on an already-loaded registry reports
  "(N views, 0 from checkpoint)" — misleading (the checkpoint WAS consumed
  by the earlier auto-load); cost us an hour of false debugging.

## Features (priority order)

### 1. Fix constant detection for window frames and LAG/LEAD offsets

`constant_int` (dbsp_plan_translator.hpp) must fold through expression
shells to reach the constant: unwrap `BOUND_CAST` chains (and, if cheap,
any foldable expression via DuckDB's scalar-folding helper) before the
`BOUND_CONSTANT` check. Acceptance: `ROWS BETWEEN 11 PRECEDING AND CURRENT
ROW` and `LAG(x, 12)` create as true MVs and maintain correctly through
edits (differential vs stock DuckDB). `LAG(x)` (offset 1) must keep
working.

### 2. Checkpoint MIN/MAX (value-collecting) aggregate state

`PlanAggregateNode` currently reports UNSUPPORTED for anything
non-scalar-fn. Extend SERIALIZABLE to plain MIN/MAX (non-DISTINCT):
serialize the per-group value multiset (the same structure the operator
keeps for retraction handling). DISTINCT/holistic (MODE/MEDIAN/quantiles/
ordered) stay UNSUPPORTED. Checkpoint format version bump. Acceptance:
save/restore round-trip equality vs a continuously-live twin through
retractions that force a group's max to retreat; wfp LNB views restore
from checkpoint (restored count rises from 29 toward 40+).

### 3. Fast typed codec for checkpoint blobs

The spill path has a typed fast-path row codec (12x serialize / 3.8x
deserialize vs BinarySerializer — see CHANGELOG "Perf/coverage: spill
codec"). Checkpoint sink/node blobs still use the slow row path
(~15µs/row observed). Reuse the same codec for `_dbsp_ckpt` blob
encode/decode. Format version bump (shared with feature 2's). Acceptance:
blob_decode total for the wfp DAG drops materially (target ≥3x on the
decode lines); round-trip exactness preserved incl. NULLs and all value
shapes the spill codec covers.

### 4. Truthful load report

`dbsp_load()` on an already-populated registry must not claim
"0 from checkpoint" when it did not attempt restore (or must report the
skip explicitly, e.g. "registry already loaded; N views live"). Smallest
truthful fix wins.

### NumPad follow-up (outside this repo, after feature 1)

Revert the two rolling views from range-join form back to the native
bounded-window form (kills the 3.8s cold hotspot; windows are
EmbeddedViewNode = non-checkpointable, an accepted trade at ~0.6s rebuild),
re-run parity + benchmark + cold/restore measurements.

## Non-goals

- Recursion checkpoint state (separate, ~0.9s).
- DISTINCT/holistic aggregate checkpointing.
- Cold-populate parallelism across views.

## Testing

Per feature above, plus: full ctest green; wfp parity clean with zero
exclusions; final cold + restore numbers recorded against the 15.5s/7.0s
baselines.

## Docs (repo rule)

CHANGELOG per feature; API.md window-support paragraph (constant frames now
accepted); ARCHITECTURE checkpoint state-kind table (MIN/MAX serializable,
codec note).

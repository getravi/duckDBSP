# Incremental window-function maintenance — design spec

Status: approved 2026-07-18. Implementation plan to follow
(`docs/superpowers/plans/`).

## Problem

duckDBSP maintains SQL window functions by **full-partition recompute**:
any delta to a partition retracts the whole partition's window output and
re-renders every row — O(partition), not O(delta)
(`include/dbsp_window_view.hpp:158–185` retract-all, `:221–426` render-all).

NumPad compiles its **entire time-intelligence vocabulary to SQL window
functions** (`calcengine/engine/pushdown_sql.py`):

- CUMSUM → `SUM(COALESCE(x,0)) OVER w_cum` (ROWS UNBOUNDED PRECEDING)
- YTD/QTD/MTD → `SUM(...) OVER w_ytd|w_qtd|w_mtd` (unbounded preceding
  within a fiscal-reset bucket / partition)
- ROLLING_SUM/AVG → `SUM(...) OVER w_roll_n` (ROWS n-1 PRECEDING .. CURRENT)
- LAG/LEAD/SHIFT → `LAG/LEAD(x,k) OVER w_pos`
- FILLFORWARD → `LAST_VALUE(x IGNORE NULLS) OVER w_ff`

These are partitioned by the non-time dimensions and ordered by time, so a
**partition is one coordinate's full time series**. Editing a single leaf
cell costs O(all periods) — for a daily-over-years model, O(thousands) per
edit.

Gap analysis (2026-07-18): duckDBSP already maintains aggregates
(SUM/COUNT/AVG O(1)/group; MIN/MAX O(log n); SUMIF as SUM(CASE)) and joins
(bilinear O(Δ×matches)) truly incrementally. **Windows are the only
non-incremental shape NumPad actually hits.** duckDBSP's E110 rejections
(recursive USING KEY, approx quantile, holistic DISTINCT, string_agg
without ORDER BY) are shapes NumPad never emits, so closing them has no
NumPad value.

## Goal

Maintain the window shapes NumPad emits in **O(affected rows)** instead of
O(partition), emitting only the changed output rows. Keep the existing
full-partition renderer as the fallback for everything else.

Success:
- Window-delta cost scales with **affected rows, not partition size**
  (measured on a single-row edit into a large partition).
- Results are **bit-for-bit identical** to the full-partition renderer.

## Approach

**A — all per-shape fast paths** (chosen). Build the incremental path for
every window shape NumPad emits; keep the full-partition renderer as
fallback. Land order:

1. **Phase 1**: LAG/LEAD/SHIFT (offset) + ROLLING_SUM/AVG (bounded frame)
   — the clean asymptotic wins, simplest correctness.
2. **Phase 2**: CUMSUM / YTD / QTD / MTD (running-sum, suffix shift).
3. **Phase 3**: FILLFORWARD.

### Architecture

Today, on a partition delta, `WindowView` updates the sorted multiset,
then retracts the whole partition output and re-renders every row. Change:
after the multiset update, compute the **affected output-row set** for the
delta (per window function, shape-specific), recompute only those rows, and
emit retract(old)+insert(new) for just them. Partition identification, the
per-partition `std::multiset<DuckDBRow>` ordered state, and the output
Z-set plumbing are unchanged. Public behavior identical; the emitted delta
shrinks O(partition) → O(affected).

### Per-shape affected-set rules

NumPad's dominant edit is a **value update at a fixed time position** (the
time spine is a fixed member set), so positions are stable and affected
sets are small:

| Shape | Affected rows on a value update at position t | Cost |
|---|---|---|
| Offset (LAG/LEAD/SHIFT, k) | the single reader row | **O(1)** |
| Bounded frame (ROLLING n, ROWS) | rows whose frame covers t | **O(frame)** |
| Running-sum (CUMSUM/YTD/QTD/MTD) | suffix t..bucket-end shifts by the value-delta | **O(suffix)**, O(1)/row via accumulator |
| Fillforward (LAST_VALUE ignore-nulls) | forward run to next non-null | **O(run)** |

Structural insert/delete (a period added/removed — rare for NumPad, common
for a general consumer) shifts positions: offset becomes O(k), the others
recompute their affected neighborhood. Both cases handled; the value-update
fast case is the common one.

When a partition carries **multiple** window functions, union their
affected sets and recompute each affected row's full output tuple.

### Data structures

Keep the per-partition `std::multiset<DuckDBRow>` (ordered by ORDER BY
columns). Affected rows found by **bounded neighbor iteration** from the
edit position (offset / rolling / fillforward) or a **suffix walk**
(running-sum). Add one **per-partition running accumulator** for
CUMSUM/PTD so the suffix shift is O(1)/row (no rescan). No new global
structures.

### Correctness + fallback

Every fast path must equal the current full-partition renderer
**bit-for-bit**: NULL handling, ROWS-frame boundaries, IGNORE NULLS,
fiscal reset buckets, and tie/peer handling. Anything not fast-pathed —
RANK/DENSE_RANK/NTILE, holistic (MEDIAN/QUANTILE/MODE), RANGE frames, or a
structural edge a fast path declines (none of which NumPad emits) — **falls
back to today's full-partition re-render** for that function/partition.
Fallback is always safe: it is the current behavior.

### Testing + gate

- **Differential test** (the repo's incremental==full discipline): for
  each shape, random partitions + random single-row and structural edits
  → assert the fast-path emitted delta equals the full-recompute emitted
  delta. Any divergence fails.
- **Window-delta throughput bench**: partition size N, single-row edit →
  confirm cost scales with affected rows, not N. Wire into the ctest gate
  alongside `planner_eval_smoke`.
- Full suite + ASAN + TSAN stay green.

## Scope / non-goals

- **In**: ROWS-frame windows for the four NumPad shapes (offset, bounded
  frame, running-sum within a reset bucket, fillforward).
- **Out (ride the fallback)**: RANGE frames; RANK/DENSE_RANK/NTILE;
  holistic aggregates (MEDIAN/QUANTILE/MODE); string_agg/array_agg;
  arbitrary user window functions. NumPad emits none of these.
- **Not** the general segment-tree framework (Approach B) — deferred under
  YAGNI until a workload needs arbitrary frames.
- Running-sum's O(suffix) is fundamental (the suffix outputs genuinely
  change), not a structure limit; still ≪ O(partition) for late edits, and
  the offset/rolling/fillforward wins are uniform.

## Consumer note

duckDBSP-only scope: this makes the engine capable of O(Δ) window
maintenance. NumPad currently recomputes windows in its own scoped
pushdown; routing them to duckDBSP-maintained window views is a downstream
NumPad change, out of scope here.

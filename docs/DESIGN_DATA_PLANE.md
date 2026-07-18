# Data-plane roadmap: from Value rows toward columnar deltas

Status: DP1 shipped 2026-07-17. Profiling 2026-07-17 (bench_planner_eval)
reclassified DP2 — keys/args are already vectorized; only residual
predicates remain — and promoted DP3 as the measured lever for a 14.4x
row<->chunk marshaling overhead. Later phases workload-gated.

## Measured baseline (bench_dataplane, 1M rows x 3 cols, M-series)

Chunk -> Z-set ingestion decomposes as:

| stage | cost | share |
|---|---|---|
| row building (per-cell Value boxing) | ~99 ms | 15% |
| lazy row hashing (per-Value `Value::Hash`) | ~465 ms | **68%** |
| Z-set map insert (hash cached) | ~121 ms | 18% |

`Value::Hash()` internally constructs two Vectors per call — the lazy
path pays vector setup a million times per column.

## DP1 (SHIPPED): vectorized row hashing

`chunk_row_hashes()` (dbsp_duckdb_types.hpp) computes per-row hashes
with one `VectorOperations::Hash` per column and replicates the lazy
combine exactly (NULL -> kNullHash substitution, boost-style fold in
column order). Equality with the lazy path holds BY CONSTRUCTION per
element — `Value::Hash` is implemented as a 1-element
`VectorOperations::Hash` — and is pinned by test_row_hash.cpp across
scalar types, NULLs, nested types, and column-subset selection.
`ColumnVec::set_hash` pre-seeds the cache; `stream_table_rows` (syncs,
baseline loads, arrangement backfills) is the wired consumer.

Result: ingestion 684 ms -> 304 ms per 1M rows (2.25x); hashing itself
~5.5x. Scan-path sync ~0.27 us/row (was ~0.8). Gate lives in
bench_dataplane (vectorized must beat legacy by >=1.5x).

CORRECTNESS INVARIANT (why test_row_hash matters): one logical row must
hash identically from every construction path — scan-loaded baselines,
notify-built rows, capture/tee rows — or Z-set dedup corrupts silently.
Any change to either hash path must keep the equality suite green.

Deliberately NOT wired: per-statement capture/tee sites (deltas are a
few rows; hash cost is noise there).

## Profiling harness (bench_planner_eval)

The planner-eval throughput benches had silently rotted: PlanTranslator
now reports fully-qualified sources (`memory.main.t`) and `apply_changes`
routes only on an exact `source_table_` match, so feeding the bare SQL
name (`"t"`) stepped the circuit empty — `get_result()` returned 0 and the
filter/aggregate/join result-size REQUIREs failed unnoticed (the benches
were manual-only, never in ctest). Fixed (feed each view its own
`source_tables()` name) and gated in ctest (`planner_eval_smoke`,
`[planner_eval],[shard_bench]`) so the numbers below are reproducible and
the gate cannot rot again. Restored (M-series, 100k-row delta): aggregate
~2.5M rows/s, join delta ~530k rows/s, fused filter ~1.0M rows/s.

## DP2: batched key/argument evaluation — LARGELY SHIPPED (residuals only)

Reclassified 2026-07-17 by profiling. The original premise was stale:
aggregate group keys + arguments, join keys, and filter/projection
expressions ALREADY run through the D1 `BatchEvaluator`
(STANDARD_VECTOR_SIZE-batched `ExpressionExecutor`) — see its construction
sites in `dbsp_plan_translator.hpp` (FILTER_MAP node; the aggregate node's
`fill`/`execute`; join `batch_left_keys_`/`batch_right_keys_`). Measured
confirmation:

- aggregate `SUM(id)` vs `SUM(id*2+1)` (identical key/plan, only the arg
  expression differs): ~-0.7% median-of-7 — argument eval is already
  batched; no per-row cost to recover.
- bare-column vs expression `GROUP BY` could NOT isolate key eval: the two
  produce different planner circuits (expression key reproducibly
  *faster*), so an SQL A/B measures plan shape, not eval. There is no
  per-row key-eval cost left — it is already batched.

REMAINING — the only per-row `RowExprEval` still on a hot path: join /
complex-filter RESIDUAL predicates (the residual `left`/`right`
`RowExprEval` members in `dbsp_plan_translator.hpp`). Defer per `TODO.md`:
batch only if residual-heavy joins show up in a profile.

## DP3: late materialization (large) — THE measured next lever

Promoted 2026-07-17. The filter bench ("RowExprEval vs hand-written lambda
on filter path") measures the fused FILTER_MAP path — already on
`BatchEvaluator` — at **14.4x slower than a hand lambda** doing the same
predicate + projection. That gap is NOT expression eval; it is row<->chunk
MARSHALING. Every operator builds a DataChunk from `DuckDBRow`s (per-cell
Value boxing), batch-executes, then reads results back per row
(`read_result` -> `std::vector<Value>`) and re-materializes a `DuckDBRow`
per row (confirmed in the aggregate node's `step()` and the FILTER_MAP
consume path). The batched execute is bracketed by scalar boxing on both
ends; the hand lambda reads `row.columns[1]` directly and skips all of it.

DP3 removes exactly this: deltas flow between linear operators as
(DataChunk, weight vector) pairs; `DuckDBRow` materializes only at stateful
boundaries (Z-set / arrangement insertion). Filters/projections then run
on native vectors end to end, deleting the read-back boxing that IS the
14.4x. Touches the circuit node interfaces — `propagate_changes`, every
PlannedCircuitView node's consume path. The gating workload the phase
required now exists and is measured: the pure linear filter/projection
bench at 14.4x is precisely linear-chain-propagation-dominant.

## DP4: columnar state (research-scale)

Replace hash-map Z-sets/arrangements with sorted columnar runs and
merge-based consolidation (the Feldera batch model). Removes the map
insert cost (~18%) and shrinks memory materially, but rewrites the
storage layer of every stateful operator including spill. Only worth it
with sustained multi-million-row delta workloads; revisit after DP2/DP3
evidence.

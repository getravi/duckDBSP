# Data-plane roadmap: from Value rows toward columnar deltas

Status: DP1 shipped 2026-07-17. Later phases unscheduled, workload-gated.

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

## DP2: batched key/argument evaluation (next, moderate)

Aggregate keys/arguments, join keys, and join residual predicates still
evaluate per row through RowExprEval (known TODO). The D1
BatchEvaluator already does vectorized filter/projection — extend it to
key extraction: evaluate key expressions over the delta as chunks, then
hash keys with chunk_row_hashes. Expected win bounded by how
key-heavy the workload is; profile bench_planner_eval aggregate/join
cases first.

## DP3: late materialization (large)

Deltas flow between linear operators as (DataChunk, weight vector)
pairs; DuckDBRow materializes only at stateful boundaries (Z-set/
arrangement insertion). Removes the remaining Value boxing (~15%) from
pass-through operators entirely and lets filters/projections run on
native vectors end to end. Touches the circuit node interfaces —
propagate_changes, every PlannedCircuitView node's consume path. Do not
start without a workload where linear-chain propagation dominates.

## DP4: columnar state (research-scale)

Replace hash-map Z-sets/arrangements with sorted columnar runs and
merge-based consolidation (the Feldera batch model). Removes the map
insert cost (~18%) and shrinks memory materially, but rewrites the
storage layer of every stateful operator including spill. Only worth it
with sustained multi-million-row delta workloads; revisit after DP2/DP3
evidence.

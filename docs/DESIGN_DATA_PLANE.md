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

## DP3a (SHIPPED 2026-07-17): output-hash preseed + MAP_COLS fusion

Per-node step profiling (env DBSP_STEP_PROF, kept in Circuit::step) broke
the 14.4x down differently than assumed: the fused FILTER_MAP node was
only ~15ms of the 90ms sync — **72ms was a hidden `plan_scan_cols`
(MAP_COLS) node**: a per-row RowMap projecting GET's column permutation,
paying per-Value copies plus a lazily hashed output Z-set insert for
every input row before the filter even ran.

Two fixes shipped:

1. **fuse_map_cols IR pass**: FILTER_MAP/FILTER_EXPR/MAP_EXPR over
   MAP_COLS(x) clones the consumer's bound expressions, remaps their
   BOUND_REF leaves through the column selection, and drops the MAP_COLS
   node (declines wholesale on out-of-range/virtual-rowid entries).
   MAP_COLS creation now records the CDC full-row types so the fused
   node's chunk layout and arity checks are exact.
2. **DP3a output-hash preseed**: PlanBatchNode folds its (already flat)
   projection result vectors into per-row hashes via fold_vector_hashes
   and pre-seeds output rows; PlanAggregateNode does the same for group
   keys before bucket-map lookups. Same lazy-formula replication as DP1;
   same test_row_hash equality gate.

Filter bench: 90ms -> 20ms per 100k rows (4.9M rows/s), overhead vs the
hand lambda 13.4x -> **2.06x**. planner_frontend (5.48M assertions),
full suite, soak, ASAN, TSAN clean.

## DP3b: full late materialization (remaining, workload-gated)

The remaining 2x vs the hand lambda is the true row<->chunk marshaling:
chunk fill from Value rows, read_result boxing of projection outputs, and
Z-set iteration. Removing it means deltas flowing between linear
operators as (DataChunk, weight vector) pairs with DuckDBRow
materializing only at stateful boundaries — a circuit-interface change
(`propagate_changes`, every node's consume path). At 2x residual the
payoff no longer clears the architectural cost on current evidence;
revisit only if a workload shows linear-chain propagation dominating
after DP3a.

## DP3a follow-up (SHIPPED 2026-07-18): joins + MAP_COLS everywhere

The join bench under DBSP_STEP_PROF repeated the filter-path story: of a
220ms 100k-row join sync, 56ms was plan_scan_cols (MAP_COLS survives
under joins — the fuse_map_cols pass cannot elide it there because the
join needs the projected layout) and 134ms was plan_join, roughly half
of it lazy-hashing the concat output rows at insert.

1. **PlanMapColsNode** replaces the per-row RowMap wherever MAP_COLS
   survives: typed chunk fill of the selected columns (BatchEvaluator
   gains a column-map fill), fold_vector_hashes for the output hashes,
   output values as COW copies of the source Values. 56 -> 20ms.
2. **Buffered join emits**: the serial inner-join try_emit path buffers
   concat rows and flushes through a typed chunk fill + hash fold +
   pre-seeded insert. plan_join 134 -> 66ms.

Results: join delta 454k -> 925k rows/s (2x); aggregate throughput
4.1M rows/s (MAP_COLS feeds aggregate pipelines too); filter overhead
vs hand lambda ~1.2x. Known trade: intra-op SHARDED probes emit into
shard-local Z-sets on the lazy-hash path, so sharding now loses to
serial at 100k-row scale — revisit the shard threshold only if a
workload shards at sizes where it used to win.

## DP4: columnar state (research-scale)

Replace hash-map Z-sets/arrangements with sorted columnar runs and
merge-based consolidation (the Feldera batch model). Removes the map
insert cost (~18%) and shrinks memory materially, but rewrites the
storage layer of every stateful operator including spill. Only worth it
with sustained multi-million-row delta workloads; revisit after DP2/DP3
evidence.

# Known limitations & candidate work

Accurate as of 2026-07-05 (post Phase C + pre-Phase-D hardening; history in
CHANGELOG.md). The February 2026 code-review dump that used to live here is
gone — everything actionable from it was either fixed (thread safety, SQL
injection, debug prints, singleton races, path traversal, read isolation)
or became obsolete when the code it referenced was deleted (checkpoint/WAL
subsystem, bespoke parser, standalone Z-set spilling).

## Not supported (DBSP-E110 at view creation)

- WITH RECURSIVE ... USING KEY
- Non-constant / percentage LIMIT
- Window ORDER BY / PARTITION BY over expressions (project first)
- ROLLUP / CUBE / GROUPING SETS; DISTINCT/FILTER/ORDER-BY-in-aggregate

## Performance

- **Scan-and-diff sync dominates end-to-end** (~119ms on 50k rows after
  the F1 constant-factor pass — typed chunk extraction, baseline swap-in,
  change-log removal — vs ~25µs to propagate the delta through a 3-level
  chain). The remaining floor is two O(n) hash passes (build scanned
  state + diff), gated on the row-hash refactor below. True O(Δ) sync =
  capture transaction-local changes in the TransactionCommit hook, which
  fires BEFORE DuckTransaction::Finalize since DuckDB 1.5 (see the
  comment in sync_table_scan_and_consume) — appends live in the
  transaction's LocalStorage at that point. Full design task: extraction
  from LocalStorage/version info, auto-CDC rewrite, fallback scan-diff
  for anything uncapturable. on_insert/on_delete already provide the
  row-level entry points.
- Phase D1 vectorized filter/map/fused evaluation + zero-copy circuit
  deltas: fused filter 259k→644k rows/s, aggregate 770k→1.88M, join delta
  140k→265k. Remaining ~2.4× gap vs a hand-written lambda is Z-set insert
  hashing (DuckDBRow hashes every column Value) — candidate: cache row
  hashes. See test/benchmarks/bench_planner_eval.cpp.
- Aggregate keys/args, join keys, and residuals still evaluate per-row
  (RowExprEval); batch if profiles demand.
- Row-hash caching (the ~2.4× insert-hashing gap) requires encapsulating
  DuckDBRow::columns behind const/mutating accessors (~350 call sites,
  compiler-enforced) so a cached hash can never go stale — do as a
  dedicated mechanical refactor, not alongside feature work.
- Deletions through recursive views trigger a full fixed-point recompute
  (correct but non-incremental).

## Architectural

- CDCManager is a deliberately leaked process-wide singleton (views pin
  the DatabaseInstance; instance-scoped ownership would be a reference
  cycle). Multi-database processes share one manager.

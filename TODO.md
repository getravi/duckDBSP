# Known limitations & candidate work

Accurate as of 2026-07-05 (post Phase C + pre-Phase-D hardening; history in
CHANGELOG.md). The February 2026 code-review dump that used to live here is
gone — everything actionable from it was either fixed (thread safety, SQL
injection, debug prints, singleton races, path traversal, read isolation)
or became obsolete when the code it referenced was deleted (checkpoint/WAL
subsystem, bespoke parser, standalone Z-set spilling).

## Not supported (DBSP-E110 at view creation)

- Outer joins (LEFT/RIGHT/FULL)
- Correlated subqueries (rewrite as JOIN or intermediate view)
- WITH RECURSIVE ... USING KEY
- Non-constant / percentage LIMIT
- Window ORDER BY / PARTITION BY over expressions (project first)
- ROLLUP / CUBE / GROUPING SETS; DISTINCT/FILTER/ORDER-BY-in-aggregate

## Performance (Phase D candidates, benchmarked 2026-07-05)

- RowExprEval is ~4.6× slower than a hand-written lambda (259k vs 1.2M
  rows/s on the filter path): vectorize node evaluation with batched
  DataChunks. Recovery replays full tables through this path, so it pays
  the cost too. See test/benchmarks/bench_planner_eval.cpp.
- Deletions through recursive views trigger a full fixed-point recompute
  (correct but non-incremental).

## Architectural

- CDCManager is a deliberately leaked process-wide singleton (views pin
  the DatabaseInstance; instance-scoped ownership would be a reference
  cycle). Multi-database processes share one manager.

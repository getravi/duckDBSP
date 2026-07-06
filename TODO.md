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
- string_agg/array_agg WITHOUT ORDER BY inside the aggregate (result
  order unreproducible incrementally; the ordered forms are supported —
  ties on order keys break by value, not input order)

## Performance

- **O(Δ) sync exists for the batch-append pattern** (G2): explicit
  transactions containing only INSERTs into tracked tables commit via
  captured deltas (~0.6ms/commit incl. the COUNT(*) guard vs ~47ms
  scan-diff on a 50k-row chain — 74×). Wrap streaming appends in
  BEGIN/COMMIT to get it. Autocommit statements cannot be captured (the
  transaction is destroyed before any extension hook fires — probed
  empirically; TransactionCommit sees a gutted DuckTransaction, and
  QueryEnd sees no transaction) and fall back to scan-diff (~47ms on 50k
  after F1+G1). DELETE/UPDATE/upsert transactions also fall back by
  design. Extending capture to deletes/updates needs DuckDB version-info
  access that 1.5.4 does not expose to extensions.
- Phase D1 vectorized filter/map/fused evaluation + zero-copy circuit
  deltas: fused filter 259k→644k rows/s, aggregate 770k→1.88M, join delta
  140k→265k. Remaining ~2.4× gap vs a hand-written lambda is Z-set insert
  hashing (DuckDBRow hashes every column Value) — candidate: cache row
  hashes. See test/benchmarks/bench_planner_eval.cpp.
- Aggregate keys/args, join keys, and residuals still evaluate per-row
  (RowExprEval); batch if profiles demand.
- Row-hash caching: DONE (G1, ColumnVec; H3 dense-map storage on top).
- Compact row encoding: SUPERSEDED by H6' copy-on-write payloads (row
  copies are refcount bumps now). Byte encoding would additionally need
  order-preserving encodings for every typed comparator — revisit only if
  profiles show payload allocation itself dominating.
- Join residual predicates still evaluate per candidate pair (RowExprEval);
  batch only if residual-heavy joins show up in profiles.
- Shared join arrangements: self-padding sides (right of RIGHT/FULL,
  left of LEFT/FULL/MARK) stay private because init-replay skip would
  lose their unmatched-row pads. Fingerprints include the side's column
  projection, so views needing different column subsets of the same
  table do not share (a maximal-columns arrangement + per-consumer
  projection could lift this).
- Deletions through recursive views trigger a full fixed-point recompute
  (correct but non-incremental).

## Architectural

- Baseline spill (K1) covers tracked-table baselines only. Join
  arrangements, aggregate states, and embedded sort/window views still
  live in RAM — spilling those needs cached random access (a real trace
  storage layer), not the sequential record log baselines use.

- CDCManager is a deliberately leaked process-wide singleton (views pin
  the DatabaseInstance; instance-scoped ownership would be a reference
  cycle). Multi-database processes share one manager.

# Known limitations & candidate work

Accurate as of 2026-07-05 (post Phase C + pre-Phase-D hardening; history in
CHANGELOG.md). The February 2026 code-review dump that used to live here is
gone — everything actionable from it was either fixed (thread safety, SQL
injection, debug prints, singleton races, path traversal, read isolation)
or became obsolete when the code it referenced was deleted (checkpoint/WAL
subsystem, bespoke parser, standalone Z-set spilling).

## Not supported (DBSP-E110 at view creation)

- WITH RECURSIVE ... USING KEY
- Non-constant (expression) LIMIT — constant and percentage forms work
- approx_quantile / reservoir_quantile / approx_top_k (approximate
  results can't match DuckDB in differentials; exact mapping would
  silently differ)
- DISTINCT on holistic aggregates (median/quantile/mode)
- MODE tie-breaking differs from DuckDB on ties (ours: smallest value;
  DuckDB: scan-order-dependent) — unreproducible incrementally
- string_agg/array_agg WITHOUT ORDER BY inside the aggregate (result
  order unreproducible incrementally; the ordered forms are supported —
  ties on order keys break by value, not input order)

## Performance

- **O(Δ) sync covers most plain SQL writes.** G2: explicit transactions
  containing only INSERTs commit via captured deltas (~0.6ms/commit incl.
  the COUNT(*) guard vs ~47ms scan-diff on a 50k-row chain — 74×); wrap
  streaming appends in BEGIN/COMMIT to get it. Write capture
  (docs/DESIGN_WRITE_CAPTURE.md): whitelisted UPDATE/DELETE statements —
  explicit-txn AND autocommit — commit via captured deltas too (~1.5ms
  for a single-row UPDATE at 1M rows vs ~2.4s scan-diff); the old
  version-info blocker was worked around by capturing pre-images with an
  internal SELECT before the statement runs. Autocommit INSERTs (VALUES
  or deterministic SELECT source; partial column lists take declared
  DEFAULTs) are captured the same way — evaluated with the INSERT's own
  casts (~1.0ms at 1M rows). Upserts with an explicit
  conflict target and excluded.-qualified SET are captured via a LEFT
  JOIN probe. Subquery predicates and
  DELETE USING (EXISTS-probe rewrite) capture too when the statement sees
  pure committed state (autocommit, or explicit txn before its first
  write). D2 plan tee (optimizer
  extension): any remaining UPDATE or DELETE shape is captured from the
  rows the plan actually processed — UPDATE...FROM, parameters, volatile
  expressions, post-write subqueries, indexed-column UPDATEs,
  same-table-twice txns. The INSERT tee covers
  autocommit INSERTs with full-cover column maps (any source shape, incl.
  LIMIT/SAMPLE/table functions) and multi-statement DML strings (each
  sub-statement tees as its own autocommit txn). Still scan-diff:
  multi-match UPDATE...FROM (ambiguous; tee detects and steps aside),
  DEFAULT-filled partial-column INSERTs from non-repeatable sources
  (defaults resolve in a physical projection above the tee). Appender
  writes ARE captured: the flush runs through the statement hooks as a
  plain INSERT and G2's LocalStorage capture takes it (probed
  empirically; tested incl. Appender-then-UPDATE ordering).
  Engine-behavior assumptions are pinned
  by test/integration/test_engine_assumptions.cpp — run it FIRST on any
  engine bump.
- Phase D1 vectorized filter/map/fused evaluation + zero-copy circuit
  deltas: fused filter 259k→644k rows/s, aggregate 770k→1.88M, join delta
  140k→265k. The Z-set ingestion hash cost is now FIXED (DP1 vectorized
  row hashing, 2.25× ingestion; docs/DESIGN_DATA_PLANE.md). Remaining
  data-plane phases (batched key eval, late materialization, columnar
  state) are scoped there, workload-gated.
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
- UNION ALL recursive deletion still triggers a full fixed-point recompute
  (multiplicity-in-cycles is ill-defined; UNION recursion is incremental
  via DRed).

- **Engine-hook fork proposal** (docs/DESIGN_ENGINE_HOOK.md): a single
  ~300-line patch to the pinned engine (commit-time modification
  callback walking the UndoBuffer) would replace the entire capture
  stack with exact per-commit deltas — 100% write coverage incl.
  Appender, ~0.3ms per statement, guards deleted, upgrade cost collapsed
  to one rebaseable commit. Unscheduled; dual-mode design keeps
  official-build hosts on the shipped stack.

## Architectural

- Spill mode (K1+K2+N2-N4) covers baselines, shared arrangements,
  local probe-target join indexes, bounded top-K sort views (constant
  AND percentage limits — the percentage cutoff now rides a scalar
  total-count with a dynamic window cap; the overflow-log refill absorbs
  cutoff growth), and oversized holistic groups. Still RAM-resident:
  self-padding join sides (pad/weight/mark reconciliation walks full-row
  structures), mode value-counts, ordered-aggregate (string_agg)
  entries, and plain ORDER BY / window views — for those the answer IS
  the total order: the result Z-set the view interface returns by
  reference is the memory floor, and row payloads are already COW-shared
  with upstream state, so a payload-spill would save little beyond node
  overhead. A known residual: sorted multisets duplicate entries per
  weight unit (weight w = w nodes, shared payload) — a weight-collapsed
  ordered map is the cheap fix if a weighty workload shows up.
- Cross-projection arrangement sharing shipped (O4): full-row
  arrangements + canonical key fingerprints + probe-time consumer
  projection; bench-gated (no regression). Fingerprints no longer
  include projections.

- CDCManager is a deliberately leaked process-wide singleton (views pin
  the DatabaseInstance; instance-scoped ownership would be a reference
  cycle). Multi-database processes share one manager.

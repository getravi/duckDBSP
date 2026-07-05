# Changelog

## Phase B5: Planner Frontend Default ON - Jul 2026

### B5: Planner becomes the default frontend (Jul 4, 2026)
- `dbsp_use_planner` now defaults to ON: every view first translates
  through DuckDB's binder/planner; the bespoke parser handles what the
  planner rejects (ORDER BY/LIMIT, recursive CTEs, plus anything DBSP-E110).
  `dbsp_use_planner(false)` restores the old behavior entirely.
- Full suite green both ways: with the default OFF (previous commit) and
  ON (this commit) — 37/37.
- Flipping the default exposed two tests coded to parser quirks, both
  fixed: the pure non-equi JOIN test only ever passed through its error
  path (parser requires an equality condition; planner supports the join,
  and the test was missing a dbsp_sync), and the window RANGE test indexed
  a `val` column the parser leaked into window view output (planner honors
  the SQL projection exactly; test now reads the last column).
- Deviation from the plan: the bespoke parser extraction paths are NOT
  deleted yet. The planner deliberately defers ORDER BY/LIMIT and
  recursion to the parser, so deleting it would remove working features.
  Deletion moves to when those gaps close (Phase C or a dedicated
  follow-up).
- Known lifetime issue (workaround in place): planner views hold an
  internal Connection that keeps the DatabaseInstance alive; if the
  process-static CDCManager destroys them at exit, DuckDB shuts down
  during static teardown (intermittent exit segfaults). Tests now reset
  the manager before the database closes; the proper fix (instance-scoped
  view lifetime) is tracked for Phase C.

## Phase B4: Planner Frontend Windows + CTEs - Jul 2026

### B4: Window functions and CTEs through the planner (Jul 4, 2026)
- LOGICAL_WINDOW translated by mapping BoundWindowExpressions onto the
  proven `NativeWindowView` (ROW_NUMBER, RANK, DENSE_RANK, NTILE, LAG,
  LEAD, FIRST/LAST/NTH_VALUE, SUM/COUNT/AVG/MIN/MAX OVER), run mid-circuit
  via the new `EmbeddedViewNode` adapter. Column-ref partitions/orders/args
  and constant offsets/frames; anything fancier falls back.
- Non-recursive CTEs: with the optimizer disabled DuckDB emits
  LOGICAL_MATERIALIZED_CTE + LOGICAL_CTE_REF (no inlining) — the definition
  subtree is built once and shared by all references.
- Correlated subqueries (DELIM_JOIN) and recursive CTEs rejected with
  explicit DBSP-E110 messages; both fall back transparently.
- `dbsp_create_view` now accepts SQL starting with WITH (non-recursive).
  WITH RECURSIVE stays rejected at that entry point: enabling it exposed a
  latent parser-path bug (rows duplicated because view initialization
  applies the base table once per reference) — tracked separately.
- Differential tests: ROW_NUMBER/RANK/LAG windows, CTE single + double
  reference (self-join through the CTE). 37/37 tests green.

## Phase B3: Planner Frontend Joins, Distinct, Set Ops - Jul 2026

### B3: Multi-source plans through the planner (Jul 4, 2026)
- `PlannedCircuitView` generalized from a linear single-source pipeline to a
  multi-source operator tree: one SourceNode per base table, shared across
  subtrees (self-joins work), translated recursively from the logical plan.
- `PlanJoinNode`: incremental inner join, bilinear delta rule
  (Δl⋈R + L⋈Δr + Δl⋈Δr). Equi-keys from EQUAL join conditions (expression
  keys included), residual comparisons (>, <, >=, <=, <>) checked per
  candidate pair, NULL keys never match, no conditions = cross product.
- `PlanDistinctNode`: multiplicity tracking, emits ±1 on 0↔positive edges.
- `PlanSetOpNode`: UNION ALL / UNION (n-ary) and INTERSECT [ALL] /
  EXCEPT [ALL] (binary) via per-input multiplicity state.
- View schema column names deduplicated (t.val + u.val → val, val_1) so
  join results stay queryable through dbsp_query.
- Not translated (falls back): outer/semi/anti/mark joins, DELIM joins,
  ORDER BY / LIMIT (deliberate — parser path already handles them; wrapping
  presentation nodes adds no user value before B5).
- Differential tests: equi-join, residual join, join+aggregate, cross join,
  DISTINCT, all four set ops, randomized two-table mutation rounds.
  37/37 tests green.

## Phase B2: Planner Frontend Aggregation - Jul 2026

### B2: Aggregation through the planner (Jul 4, 2026)
- `PlanAggregateNode`: incremental GROUP BY with multiple aggregates per
  view (COUNT(*)/COUNT/SUM/AVG/MIN/MAX), expression group keys (e.g.
  `GROUP BY val % 3`), and expression aggregate arguments. Accumulators:
  int64/double sums, `multiset<Value>` for MIN/MAX (any orderable type).
- HAVING comes free: the planner emits it as a FILTER above the aggregate,
  which the existing FILTER translation already handles.
- Global aggregates (no GROUP BY) keep exactly one result row — including
  on an empty table (COUNT=0, SUM/AVG NULL), matching DuckDB semantics the
  bespoke parser never had.
- Not translated (falls back): DISTINCT aggregates, FILTER clauses,
  ORDER BY in aggregates, ROLLUP/CUBE/GROUPING SETS, DECIMAL SUM/AVG.
- Differential tests: multi-aggregate, expression keys, HAVING, global
  aggregate incl. delete-to-empty; randomized rounds now inject NULLs.
  37/37 tests green.

## Phase B1: Planner Frontend Skeleton - Jul 2026

### B1: DuckDB Planner as Frontend — scan/filter/project (Jul 4, 2026)
- New `dbsp_plan_translator.hpp`: view SQL parsed/bound/planned by DuckDB
  itself (`Connection::ExtractPlan` on an internal connection, optimizer
  disabled for canonical plan shapes). `LOGICAL_GET → LOGICAL_FILTER →
  LOGICAL_PROJECTION` chains translate to circuit nodes
  (`PlannedCircuitView`); bound expressions evaluate row-at-a-time via
  `ExpressionExecutor` — arbitrary expressions, function calls, and mixed
  AND/OR predicates now work (the bespoke parser silently dropped OR
  filters).
- Feature flag `dbsp_use_planner(true/false)` (default OFF). Unsupported
  plans yield DBSP-E110 internally and fall back to the bespoke parser
  transparently.
- Differential test harness (`test/integration/test_planner_frontend.cpp`):
  view result == direct DuckDB query result after every delta batch across
  randomized insert/delete sequences. 37/37 tests green.

## DuckDB 1.5.4 Upgrade, Deadlock Fix & Phase A Start - Jul 2026

### Engine Upgrade & v2 Extension API (Jul 4, 2026)
- DuckDB pinned v1.4.0 → v1.5.4; entry point via `DUCKDB_CPP_EXTENSION_ENTRY`,
  registration via `ExtensionLoader` (legacy `dbsp_init`/`dbsp_version` removed).
- Adapted to 1.5 API: n-ary `SetOperationNode`, `ParserExtension::Register`,
  `ExtensionCallback::Register`, split `duckdb_generated_extension_loader` link.

### Integration Test Deadlock Fixed (Jul 4, 2026)
- Root cause: same-thread `struct_mutex_` relock — internal helper connection
  triggered first-time recovery from inside a sync holding the lock.
- `InternalQueryGuard` (thread-local) marks internal queries; hooks and
  recovery skip them. `auto_sync_enabled_` now atomic. Nested `context.Query()`
  replaced with fresh connections.
- Commit-hook sync scans committed state via fresh connection (raw storage
  scan with the just-committed transaction sees no rows in 1.5).
- Result: 36/36 tests green in ~4s (previously 10 tests hung at 120s timeouts
  since February). Also fixed: recursive subquery detection, stale ORDER BY
  error test, checkpoint tests saving while recovery disabled.

### Phase A: Circuit IR Unification - COMPLETE (Jul 4, 2026)
All view execution now flows through the `dbsp::Circuit` substrate
(`dbsp_circuit_views.hpp`); verified 36/36 tests green.
- `DuckDBZSet` unified with generic `dbsp::ZSet<DuckDBRow, DuckDBRowHash>` —
  circuit nodes operate on production rows with no boundary conversion.
- Fine-grained circuit views: Filter (Source→Filter→Sink), Project
  (Source→Map→Sink), FilterProject (Source→Filter→Map→Sink), Aggregate
  (Source→RowAggregateNode→Sink; group state + MIN/MAX multiset live in the
  node, the sink integrates emitted retract/emit deltas).
- Opaque circuit nodes via `WrappedViewNode`/`CircuitWrappedView`: Join,
  Distinct, Sort, Limit, Window, DistinctOn — proven state logic reused
  verbatim, to be decomposed into fine-grained nodes later.
- SetOp / Recursive / CTE remain combinators composing circuit-backed views.
- `SinkNode::set_materialized` added for checkpoint restore.
- Deferred to Phase C: porting DBSPOptimizer's 4 passes from ParsedViewDef
  rewrites to IR graph rewrites (they run pre-construction and still work
  unchanged; rewriting them belongs with naive-circuit construction).

## Phase 5: Advanced SQL & Automation - Completed Feb 2026

### P5.3: Subqueries & Non-recursive CTEs
**Completion Date**: Feb 7, 2026
**Description**: Parser support for non-recursive CTEs and subqueries in FROM.
- Handle DuckDB `CTE_NODE` wrapper for non-recursive CTEs.
- Support multiple CTEs via both CTE_NODE unwrapping and cte_map fallback.
- Parse subqueries in FROM clause as derived tables.
- CDCManager creates intermediate views for CTEs and derived tables.
- Added 5 test cases with 21 assertions.

### P5.1: Advanced Window Functions
**Completion Date**: Feb 7, 2026
**Description**: Support for LAG, LEAD, FIRST_VALUE, LAST_VALUE, NTH_VALUE, NTILE, and custom ROWS BETWEEN frame bounds.
- Implemented `LAG()` and `LEAD()` with custom offsets in `NativeWindowView`.
- Implemented `FIRST_VALUE()`, `LAST_VALUE()`, `NTH_VALUE()`, `NTILE()`.
- Implemented support for `PRECEDING` and `FOLLOWING` offsets in frames.
- Added comprehensive tests (10 test cases, 41 assertions).

## Phase 4: Advanced Features - Completed Feb 2026

### P4.3: Window Functions and Streaming Aggregates
**Completion Date**: Feb 8, 2026
**Description**: Support window functions for time-based and row-based windows.
- Row-based windows work correctly.
- PARTITION BY works.
- ORDER BY within window works.
- Incremental maintenance of windows.
- Standard window functions (RANK, DENSE_RANK, ROW_NUMBER, LAG, LEAD).

### P4.2: ORDER BY and LIMIT Support
**Completion Date**: Feb 8, 2026
**Description**: Implement ORDER BY and LIMIT/OFFSET for sorted, paginated results.
- Implemented incremental sort using O(n log n) logic.
- Implemented LIMIT/OFFSET support.
- Fixed `ORDER BY` default direction (ASC).

## Phase 3: Production Readiness - Completed Feb 2026

### P3.4: Circuit Optimization Pass
**Completion Date**: Feb 7, 2026
**Description**: Implement circuit optimization passes for operator fusion and pushdown.
- Filter Pushdown: Move filters closer to data sources.
- Projection Pushdown: Eliminate unused columns early.
- Operator Fusion: Combine multiple map/filter operations.

### P3.2: Reader-Writer Locks for Concurrency
**Completion Date**: Feb 7, 2026
**Description**: Replace global mutex with reader-writer locks for concurrent queries.
- Replaced `std::mutex` with `std::shared_mutex` in `CDCManager`.
- Updated lock usage: shared locks for reads, exclusive locks for writes.
- Verified no data races with ThreadSanitizer.

### P3.1: Enhanced Error Messages and Diagnostics
**Completion Date**: Feb 7, 2026
**Description**: Improve error messages for unsupported features and edge cases.
- Rewrote error messages to include problem, workaround, and example.
- Added context (line numbers, table names).
- Created documentation pages for common errors.

## Phase 2: Core DBSP Completeness - Completed Feb 2026

### P2.4: Recursive Query Support
**Completion Date**: Feb 7, 2026
**Description**: Implement WITH RECURSIVE for transitive closures and recursive queries.
- Extended SQL parser for `WITH RECURSIVE`.
- Implemented stream introduction (δ₀) and elimination (∫).
- Created `RecursiveView` with fixed-point iteration.
- Validated transitive closure and hierarchy queries.

### P2.3: Support Complex JOIN Predicates
**Completion Date**: Feb 7, 2026
**Description**: Extend JOIN support from equality-only to complex predicates.
- Updated parser to handle compound `ON` clauses (AND-ed predicates).
- Updated `NativeJoinView` to evaluate complex predicates.
- Validated multi-column equality and non-equi joins.

### P2.2: Fix MIN/MAX Incremental Deletions
**Completion Date**: Feb 7, 2026
**Description**: Replace O(n) MIN/MAX deletion handling with O(log n) solution.
- Extended `AggState` to track all values using `std::multiset`.
- Implemented O(log n) insert/delete for MIN/MAX.
- Verified correctness and performance.

### P2.1: DISTINCT SQL Integration
**Completion Date**: Feb 7, 2026
**Description**: Expose existing DISTINCT operator to SQL parser.
- Updated SQL parser to detect `SELECT DISTINCT` syntax.
- Wired `DistinctView` in `CDCManager`.
- Verified duplicate removal and incremental updates.

## Phase 1: Foundation - Completed Feb 2026

### P1.3: Add MIN and MAX Aggregate Functions
**Completion Date**: Feb 6, 2026
**Description**: Implement MIN and MAX aggregate functions alongside SUM/COUNT/AVG.
- Extended `AggregateFunction` enum.
- Updated parser to recognize MIN/MAX.
- Implemented computation logic for initial data and updates.

### P1.2: Implement HAVING Clause for Aggregate Filtering
**Completion Date**: Feb 6, 2026
**Description**: Add HAVING clause support for filtering aggregated results.
- Extended `ParsedViewDef` to store HAVING filter.
- Updated `NativeAggregateView` to filter groups based on HAVING condition.
- Verified correct filtering and incremental updates.

### P1.1: Wire Parser Extension for DDL Syntax
**Completion Date**: Feb 6, 2026
**Description**: Complete parser extension integration to enable native SQL DDL syntax.
- Registered parser extension in `LoadInternal()`.
- Verified `CREATE MATERIALIZED VIEW`, `DROP`, `REFRESH` syntax.
- Added aliases `dbsp_drop` and `dbsp_drop_cascade`.

## Infrastructure & Migrations - Completed Feb 2026

### Extension Loading Infrastructure
**Date Completed**: Feb 5, 2026
- Fixed extension entry point naming.
- Fixed runtime deadlock issues.
- Integrated DuckDB metadata system.

### DependencyGraph Bug Fixes
**Date Completed**: Feb 5, 2026
- Fixed topological sort returning empty vector.
- Fixed transitive dependency tracking.

### Security Hardening
**Date Completed**: Feb 5, 2026
- Fixed null byte injection in `validate_filepath()`.
- Implemented cross-platform path validation.

### Test Infrastructure Setup
**Date Completed**: Feb 5, 2026
- Created CMake test configuration.
- Fixed Catch2 compatibility.
- Enabled CTest execution.

### DuckDB 1.4.0 Migration
**Date Completed**: Feb 5, 2026
- Upgraded to DuckDB v1.4.0 APIs.
- Updated build system and successfully compiled extension.

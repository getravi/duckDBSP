# Phase B Plan: DuckDB Planner as Frontend

**Goal:** replace the bespoke SQL parser (`dbsp_sql_parser.hpp` view-def
extraction) with DuckDB's own binder/planner. Any SELECT DuckDB can plan —
within the supported logical-operator set — becomes an incrementally
maintained view. Kills the "parser doesn't recognize this shape" failure mode
and gets DuckDB's full expression/type/function coverage for free.

**Prerequisite (done):** Phase A — all view execution on the `dbsp::Circuit`
substrate; unified `DuckDBZSet = dbsp::ZSet<DuckDBRow, DuckDBRowHash>`.

**Verified API surface (DuckDB v1.5.4):**
- `Connection::ExtractPlan(query) -> unique_ptr<LogicalOperator>` — parse,
  bind, plan, optimize in one call
- `ColumnBindingResolver` (`duckdb/execution/column_binding_resolver.hpp`) —
  rewrites bound column refs to physical indices
- `ExpressionExecutor` — evaluates bound `Expression`s against `DataChunk`s

## Architecture

```
view SQL ─► internal Connection (InternalQueryGuard)
        ─► ExtractPlan ─► LogicalOperator tree
        ─► ColumnBindingResolver
        ─► PlanTranslator (new: dbsp_plan_translator.hpp)
        ─► PlannedCircuitView : NativeMaterializedView
             sources: one SourceNode per base table (auto-tracked)
             nodes:   Filter/Map/Aggregate/Join/... from logical ops
             sink:    integrated result
```

### Operator mapping

| LogicalOperatorType        | Circuit node                              |
|----------------------------|-------------------------------------------|
| LOGICAL_GET                | SourceNode (register table with CDC)      |
| LOGICAL_FILTER             | FilterNode + ExpressionExecutor predicate |
| LOGICAL_PROJECTION         | MapNode + ExpressionExecutor projection   |
| LOGICAL_AGGREGATE_AND_GROUP_BY | RowAggregateNode (extended)           |
| LOGICAL_COMPARISON_JOIN    | join node (equi keys + residual filter)   |
| LOGICAL_DISTINCT           | DistinctNode                              |
| LOGICAL_ORDER_BY / LIMIT   | presentation nodes (wrap Sort/Limit)      |
| LOGICAL_SET_OPERATION      | set-op combinator                         |
| LOGICAL_WINDOW             | wrapped window node                       |
| LOGICAL_CTE_REF / RECURSIVE_CTE | combinator wiring                    |
| anything else              | DBSP-E1xx error naming the operator       |

### Expression evaluation

Bound expressions run through `ExpressionExecutor`. First pass: row-at-a-time
adapter — build a 1-row `DataChunk` per `DuckDBRow`, evaluate, read result
`Value`. Correct but slow; acceptable because current hand-rolled evaluation
is also row-at-a-time. Vectorization (Z-set batches as `DataChunk`s) is a
separate perf milestone at the end (B6, optional / Phase E overlap).

## Milestones

Each milestone: suite green + differential tests green before the next.

**B1 — Skeleton + scan/filter/project (1–1.5 wk)**
- `dbsp_plan_translator.hpp`: ExtractPlan via guarded internal connection,
  binding resolution, translate GET → FILTER → PROJECTION chains
- Row-wise ExpressionExecutor adapter
- `PlannedCircuitView` with per-table SourceNodes
- Feature flag: `dbsp_use_planner(true/false)` table function; default OFF;
  on unsupported operator, fall back to old parser transparently
- Differential test harness (new `test/integration/test_planner_frontend.cpp`):
  translated-view result == direct DuckDB query result, checked after every
  delta batch, across randomized insert/delete sequences

**B2 — Aggregation (1 wk)**
- Extend RowAggregateNode: multiple aggregates per GROUP BY, expression keys,
  HAVING (planner emits it as a FILTER above the aggregate — comes free)
- Aggregate value types beyond int64 where DuckDB Value allows cheaply

**B3 — Join, distinct, set ops, order/limit (1–1.5 wk)**
- LOGICAL_COMPARISON_JOIN: equi-key extraction + residual predicate via
  ExpressionExecutor; map to existing join state machine as a node
- LOGICAL_DISTINCT → DistinctNode; set ops → combinator; ORDER BY/LIMIT →
  presentation nodes

**B4 — Window + CTEs (1 wk)**
- LOGICAL_WINDOW → wrapped window node
- CTE / recursive CTE plan shapes → existing combinators
- Reject correlated-subquery plans (DELIM_JOIN) with a clear error

**B5 — Flip and delete (0.5 wk)**
- Default `dbsp_use_planner` ON; run full suite both ways for one release
- Delete the bespoke parsing paths in `dbsp_sql_parser.hpp` (keep the node
  builders that survived); update ARCHITECTURE.md, README, CHANGELOG

## Success criteria

1. Any SELECT within the operator table above becomes a working incremental
   view — including queries the old parser rejected (arbitrary expressions,
   function calls, mixed AND/OR predicates, multi-aggregate GROUP BY)
2. Unsupported operators fail with a DBSP-E1xx error naming the operator
3. 36/36 existing tests + differential suite green with planner ON
4. Bespoke parser extraction code deleted (B5)

## Risks

- **Internal API churn:** ExtractPlan/planner types are not stable ABI.
  Mitigated: engine pinned v1.5.4; upgrade deliberately.
- **Optimized plans surprise the translator:** DuckDB may push filters into
  GET (table_filters) or collapse projections. Translator must handle
  table_filters on GET or disable specific optimizers via
  `ClientConfig.disabled_optimizers` for extraction.
- **Column binding drift:** resolver rewrites must match our row layout;
  differential tests catch mismatches immediately.
- **CDC name mapping:** LOGICAL_GET exposes table via catalog entry — must
  map to CDC tracked-table names (schema-qualified vs bare).

# PRD: Incremental Materialized View Engine (DBSP-Based)

## 1. Executive Summary

An extension/plugin for a relational database that provides **incrementally maintained materialized views** using DBSP (Database Stream Processing) theory. Instead of recomputing an entire view when underlying data changes, the engine processes only the delta — the rows that were inserted or deleted — and updates view state in O(Δ) time rather than O(n).

**The core value proposition**: queries against materialized views are always instant (O(result_size)), and view maintenance after data changes is proportional only to the size of those changes, not the total table size.

---

## 2. Problem Statement

Standard SQL materialized views have a fundamental trade-off: either they are stale (not updated on every write) or expensive to maintain (full recomputation). This makes them impractical for high-write-frequency tables or complex aggregation pipelines.

**The target pain:**
- Dashboards that aggregate millions of rows must re-query the full table on every page load
- Materialized views are refreshed on a schedule, always showing stale data
- Complex derived aggregates (SUM per customer, per region, per month) cannot be maintained cheaply

**What this product changes:**
Any insert or delete to a tracked table triggers an incremental update across all dependent views. The update touches only the affected aggregates and join outputs. For a 10M-row orders table, adding one row updates a `SUM(amount) GROUP BY customer` view in microseconds, not seconds.

---

## 3. Target Users

| User | Need |
|---|---|
| **Analytics engineer** | Define views in SQL; get always-fresh aggregates without full refresh |
| **Application developer** | Low-latency reads from pre-aggregated data; no application-layer caching |
| **Data pipeline author** | Chain views in a dependency graph; changes flow automatically downstream |
| **DB administrator** | Persist view definitions across restarts; recover all committed data after a crash |

---

## 4. Core Concepts

These are database-agnostic and must be preserved in any reimplementation.

### 4.1 Z-Set

The fundamental data structure. A Z-Set is a map from row → integer weight.

```
{(Alice, 100): +1, (Bob, 200): +1}   -- two rows present
{(Alice, 100): -1}                    -- deletion of Alice
```

- Weight `+1` = insertion; weight `-1` = deletion
- Zero weights are removed from the map
- Z-Sets support addition and negation
- A full table state and a delta (change) are represented identically

### 4.2 Delta (Δ)

A Z-Set representing changes since the last sync. The engine never sees full table snapshots during maintenance — only deltas.

### 4.3 Incremental View

A view that maintains its result by applying each incoming delta through an incremental algorithm specific to the view's operator type. No full recomputation.

### 4.4 Propagation Order

When a base table changes, all dependent views must be updated in **topological order** through the dependency graph. A view on a view must see its source view's updated state before being updated itself.

---

## 5. Functional Requirements

### FR1 — Table Tracking

**FR1.1** The user must be able to register any base table for change tracking.
**FR1.2** Registering a table loads its current contents into the tracking state (initial snapshot).
**FR1.3** The system must infer and store the table's schema (column names and types) at registration time.
**FR1.4** A table can be unregistered; this drops all views that depend on it.
**FR1.5** The system must expose a function to list all currently tracked tables with their schemas.

### FR2 — View Creation

**FR2.1** Users define views using a standard SQL SELECT statement.
**FR2.2** The SQL is parsed at view creation time, not at query time.
**FR2.3** Views must be named. Names must be unique within the instance.
**FR2.4** A view can reference base tables or other views as sources.
**FR2.5** Circular dependencies between views must be detected and rejected at creation time.
**FR2.6** View creation implicitly tracks any untracked source tables.
**FR2.7** On creation, the view is immediately populated with results from the current table state.

### FR3 — Incremental Maintenance Engine

This is the core. Each operator type requires its own incremental algorithm.

**FR3.1 Filter (WHERE clause)**
Apply the predicate to each row in the delta. Matching rows pass through with their original weights. Time: O(|Δ|).

**FR3.2 Projection (SELECT columns)**
Map each row in the delta through the column selection. Time: O(|Δ|).

**FR3.3 Fused Filter+Project**
As an optimization, these two operators should be combinable into a single pass.

**FR3.4 Aggregation (GROUP BY + aggregate functions)**

The engine must maintain running per-group aggregate state. When a delta arrives:
1. For each `(row, weight)` in the delta, extract the group key
2. Emit a deletion of the current aggregate result for that group
3. Update the running state (sum, count, min, max)
4. Emit an insertion of the new aggregate result

Supported aggregate functions: `SUM`, `COUNT`, `AVG`, `MIN`, `MAX`.

For `MIN` and `MAX`: maintain a sorted multiset per group so that removing the current min/max is O(log n) rather than requiring a full group scan.

`HAVING` clause must be supported as a post-aggregation filter applied to the output Z-set.

**FR3.5 Distinct**
Maintain a per-element count across all insertions and deletions. An element transitions from absent (count ≤ 0) to present (count > 0) and back. Emit `+1` on positive transition, `-1` on negative transition.

**FR3.6 Inner Join**

For two sources A and B with deltas ΔA and ΔB and previous states A_prev and B_prev:

```
Δresult = (ΔA ⋈ B_prev) + (A_prev ⋈ ΔB) + (ΔA ⋈ ΔB)
```

Must support:
- Single-column equi-join
- Multi-column equi-join
- Non-equi predicates (e.g., `a.amount > b.threshold`)

**FR3.7 Set Operations**

| Operation | Z-Set Semantics |
|---|---|
| UNION ALL | Add both Z-sets: w(x) = wA(x) + wB(x) |
| UNION | UNION ALL then DISTINCT |
| INTERSECT | w(x) = min(wA(x), wB(x)), discard ≤ 0 |
| EXCEPT | w(x) = wA(x) - wB(x), discard ≤ 0 |

**FR3.8 Window Functions**

Maintain a per-partition sorted buffer of rows. Supported functions:
- `ROW_NUMBER`, `RANK`, `DENSE_RANK`
- `LAG`, `LEAD` (with configurable offset)
- `SUM`, `AVG`, `COUNT` over `ROWS BETWEEN` frames
- `FIRST_VALUE`, `LAST_VALUE`, `NTH_VALUE`
- `NTILE(n)`

On delta, recompute window values only for affected partitions.

**FR3.9 Recursive Queries (WITH RECURSIVE)**

Support fixed-point iteration:
1. Evaluate the anchor (non-recursive term) to get initial state T₀
2. Iteratively apply the recursive term: Tₙ₊₁ = Tₙ ∪ f(Tₙ)
3. Halt when no new rows are added (fixed point reached)
4. Enforce a maximum iteration limit (default: 1000) to prevent infinite loops

On incremental update: recompute from the anchor delta, re-run fixed-point.

**FR3.10 Non-Recursive CTEs**

CTEs are materialized as intermediate views with a lifecycle tied to their parent view. Multiple CTEs in a single statement are supported.

**FR3.11 Optimizer Passes**

Before creating a view, apply these transformations to the parsed plan:

| Pass | Effect |
|---|---|
| Combine filters | Merge adjacent filter predicates into a single predicate with AND |
| Filter pushdown | Move filters before JOINs to reduce join input size |
| Projection pruning | Drop columns not needed by downstream operators |
| Operator fusion | Merge adjacent filter+project into a single `FilterProject` operator |

### FR4 — Change Data Capture (CDC)

**FR4.1 Manual sync**: The user calls a sync function after DML operations. The engine computes the delta by comparing current table state against last-known state (full-table snapshot diff).

**FR4.2 Automatic sync**: The engine hooks into the database's transaction commit mechanism. On commit, any tables that were modified within that transaction are automatically synced. No user intervention required.

**FR4.3 Auto-sync must be toggleable** at runtime without restarting the server.

**FR4.4 Manual notification** (push-mode CDC): The user can explicitly notify the engine of a specific row insertion or deletion, bypassing the snapshot-diff approach. This is useful for high-performance pipelines that want to avoid scanning the table.

**FR4.5 Sync all**: A single call syncs every tracked table in one operation, with optional parallelism.

### FR5 — Cascading Views & Dependency Management

**FR5.1** A dependency graph must track which views depend on which sources (tables or other views).
**FR5.2** When a source changes, the topological order of dependent views is computed and all are updated in that order.
**FR5.3** Dropping a view must fail if other views depend on it, unless CASCADE is specified.
**FR5.4** `DROP CASCADE` drops the target view and all transitive dependents.
**FR5.5** Users can query the dependency relationships of any view (what it depends on; what depends on it).

### FR6 — Persistence & Recovery

**FR6.1 View definition persistence**: View names and their SQL definitions must be saveable to durable storage (either within the database itself or as a JSON file).

**FR6.2 Load on restart**: On startup, the engine can reload view definitions from storage and re-sync all tables to rebuild view state.

**FR6.3 Replay-based state rebuild**: On recovery, view state is rebuilt by replaying the host database's committed storage through each view's circuit. The host database is the single durable source of truth; the engine must not maintain a parallel durability mechanism. (Rationale: circuit-internal state — aggregate accumulators, join indexes, sort multisets, recursion frontiers — cannot be reconstructed from a serialized view result alone, and replaying separately-logged deltas double-applies data already durable in the host. An earlier checkpoint+WAL design was removed for exactly these failure modes.)

**FR6.4 No resurrection of uncommitted data**: Rows that were not committed to the host database's storage at crash time must not appear in recovered views.

**FR6.5 Crash detection**: A session marker (lock file or equivalent) detects abnormal termination. On restart after a crash, recovery is triggered automatically.

**FR6.6 Committed-state fidelity**: After recovery, every view's state must equal a full recomputation of its SQL over the host database's committed data.

### FR7 — Introspection

**FR7.1** List all tracked tables (name, column count).
**FR7.2** List all views (name, SQL definition, current row count, update version).
**FR7.3** Show dependency graph for a specific view.
**FR7.4** Query a view and return its current contents as a table.

### FR8 — Error Handling

**FR8.1** Errors must include a structured error code with category (parse/validation/runtime/resource/persistence).
**FR8.2** Parse errors must indicate the position in the SQL where the problem occurred.
**FR8.3** Unsupported SQL patterns must be rejected at view creation time, not at sync time.
**FR8.4** All identifiers (table names, view names, file paths) must be validated to prevent SQL injection and path traversal.

---

## 6. Non-Functional Requirements

| Requirement | Specification |
|---|---|
| **Correctness** | View results must be identical to what a full recomputation would produce for any sequence of inserts and deletes |
| **Incremental complexity** | Filter, project, distinct, aggregation: O(|Δ|). Join: O(|Δ| × |other_side|). |
| **Concurrency** | Multiple readers can query views simultaneously. Writers (sync, create, drop) require exclusive access. Use reader-writer locking. |
| **Lock ordering** | If multiple lock tiers exist, they must always be acquired in a fixed order to prevent deadlock. Never acquire a coarser-grained lock after a finer-grained one. |
| **Thread safety** | No global mutable state accessible without a lock. Error accessors must return by value, not by reference. |
| **Memory** | All view state is in-memory. No automatic eviction. The operator must document this clearly. |
| **Identifier security** | Only allow alphanumeric characters and underscores in table and view names |
| **Recursion safety** | Fixed-point iteration must have a configurable upper bound |

---

## 7. Integration Contract (What the Target Database Must Provide)

For any reimplementation, the host database must expose:

| Capability | Used For |
|---|---|
| Parse a SELECT statement into a query plan / AST | Extracting view structure at creation time |
| Execute a SELECT and return rows | Initial table snapshot + delta computation |
| Transaction commit hook | Automatic CDC |
| Persistent storage (tables or files) | View definition persistence |
| Type system (int, float, string, bool, date) | Row representation in Z-sets |
| Extension/plugin API | Registering new functions |

---

## 8. Out of Scope

- **UPDATE statements as deltas**: UPDATE is handled by rewriting as DELETE + INSERT at the CDC layer; the core engine only knows insertions and deletions.
- **Distributed / multi-node**: The engine is single-process. Distributed DBSP is a separate research problem.
- **Automatic spill-to-disk**: View state lives entirely in memory. Memory limits are the operator's responsibility.
- **Subquery decorrelation**: Correlated subqueries in view definitions are not supported.
- **Non-deterministic functions**: Views using `RANDOM()`, `NOW()`, etc. are not supported — they cannot be incrementally maintained.
- **DDL on tracked tables**: Altering a tracked table's schema requires dropping and recreating its dependent views.
- **Multi-instance**: One engine per database instance (singleton model).

---

## 9. Success Metrics

| Metric | Target |
|---|---|
| Aggregate view update after single-row insert | < 1ms |
| Query latency on view (all sizes) | O(result_size), < 10ms for 10K rows |
| Recovery time via replay (small DB, < 10K rows) | < 100ms |
| Correctness | View output == full recomputation for all supported operator types |
| Test coverage | All operator types have unit tests with insert-only, delete-only, and mixed sequences |

---

## 10. References

1. Budiu, M., et al. "DBSP: Automatic Incremental View Maintenance." VLDB 2023.
   https://www.vldb.org/pvldb/vol16/p1601-budiu.pdf

2. Feldera — production DBSP implementation
   https://docs.feldera.com/

3. McSherry, F. "Differential Dataflow"
   https://github.com/TimelyDataflow/differential-dataflow

4. Reference implementation (DuckDB extension)
   https://github.com/[your-repo]/duckDBSP

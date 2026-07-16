# O(Δ) auto-sync capture for UPDATE and DELETE — design findings

Status: accepted (2026-07-15). Implementation plan:
`docs/superpowers/plans/2026-07-15-write-capture.md`.

## Problem

Auto-sync is O(Δ) only for explicit transactions containing only INSERTs
(G2 captured deltas). Every UPDATE, DELETE, or upsert falls back to
scan-and-diff scoped to touched tables (~47ms at 50k rows, ~2.4s per edit
at 1M rows). Hosts issuing plain SQL writes need interactive latency.

The blocker: transaction-local old-row images for UPDATE/DELETE live in
DuckDB version chains, which the pinned v1.5.4 does not expose to
extensions. `LocalStorage` exposes appends only.

## Decision: pre-image capture from statement classification (design 1)

At QueryBegin, for a whitelisted UPDATE/DELETE on a tracked table, run one
internal-connection SELECT that captures the old images *and* — for UPDATE
— computes the new images in the same query by projecting the SET
expressions cast to the column types:

```sql
-- UPDATE t SET c2 = <expr> WHERE <pred>  becomes
SELECT rowid, c1, c2, ..., CAST((<expr>) AS <type-of-c2>) FROM t WHERE (<pred>)
-- DELETE FROM t WHERE <pred>  becomes
SELECT rowid, c1, c2, ... FROM t WHERE (<pred>)
```

Each result row yields (old_row, −1) and, for UPDATE, (new_row, +1) into
the same per-transaction ZSet buffer the G2 INSERT capture uses. Commit
applies the merged per-table delta through `apply_captured_delta` — the
existing ingestion path, no parallel pipeline. Capture cost is one
optimized SELECT with the statement's own predicate: zone-map pruning
makes a single-row UPDATE at 1M rows a few ms, and worst case equals the
statement's own scan cost.

Deviation from the spec's sketch: the spec suggested re-selecting captured
rowids at QueryEnd for post-images. That is unnecessary (SET expressions
evaluate against the old row, exactly like a SELECT projection) and
unsound in the `update_is_del_and_insert` case below. Computing post-images
in the capture query removes the second round-trip and the rowid
dependence from the delta entirely; rowids are kept only for the commit
guard.

### Why not design 2 (OptimizerExtension tee on LogicalUpdate/LogicalDelete)

Rejected for now:

- New extension surface (this repo registers no OptimizerExtension) and
  materially more C++: plan mutation (widening the DML child projection),
  a thread-safe execution-time tee (DML children execute in parallel
  pipelines), and per-operator handling for `update_is_del_and_insert`
  plans whose shape differs (full-row delete+insert vs in-place update).
- Plan rewriting risks correctness of the *user's* statement; design 1
  never touches the user's plan.
- Design 1's guard machinery is needed regardless (the existing COUNT(*)
  guard is UPDATE-blind either way), so design 2 saves little.
- Design 2 remains the escalation path if profiling shows whitelist misses
  dominate real workloads: it is exact, captures what actually executed,
  and tolerates non-determinism. Nothing in design 1 blocks it later.

**Design-2 feasibility spike (2026-07-15, `include/dbsp_plan_tee.hpp` +
`test/integration/test_plan_tee.cpp`): VIABLE on pinned v1.5.4.** Proven:
`OptimizerExtension::Register` fires per statement; a
`LogicalExtensionOperator` injected above a bound `LogicalDelete`'s child
survives ColumnBindingResolver and physical planning; its
`PhysicalOperator` observes the exact deleted rowids at execution with
statement behavior unchanged (autocommit, explicit txn, rollback).
Gotcha: at optimize time the DELETE's rowid expression is a
`BOUND_COLUMN_REF` (binding-based) — `BOUND_REF` indexes exist only after
the resolver — so injection maps bindings to child output positions. The
spike is env-gated (`DBSP_TEE_SPIKE`) and feeds no sync machinery.
**Design-2 phase 1 SHIPPED (same day): the DELETE tee is production.**
`include/dbsp_plan_tee.hpp` widens a tracked-table DELETE's child chain
({FILTER | PROJECTION | left side of any join}* over the target GET) to
carry full old row images, injects the pass-through extension operator,
and streams (rowid, old row) into the connection's `DBSPContextState`
(rowid-deduped: join-shaped USING deletes emit one row per MATCH). Teed
rows are exact — they capture every DELETE shape design 1 declines:
post-write subqueries, parameters, volatile predicates, same-table-twice
transactions. Explicit transactions fold teed rows into the capture
buffer at QueryEnd; autocommit applies them from the commit hook with no
guard (they ARE what executed). Design 1 stays first choice; the tee
arms only when it declined.

Hook-ordering findings from making this production (hard-won, keep):
- Autocommit statements HAVE an active transaction at QueryBegin AND at
  QueryEnd (`TransactionContext::IsAutoCommit()` distinguishes) — but
  their commit hook fires mid-statement, and catalog resolution inside
  that hook FAILS (the old "fold it now" hook path never actually
  resolved; the QueryBegin fold always served autocommit). Consequently:
  autocommit statements fold at QueryBegin, explicit-txn statements fold
  at QueryEnd (so the optimizer tee, which runs between the hooks, can
  mark the statement captured first), and QueryEnd must skip autocommit
  statements entirely — folding there re-counts a finished transaction
  and poisons the NEXT statement's commit into a read-only skip.

Remaining for design-2 phase 2 (UPDATE tee): the same child widening
plus reading SET values from the child projection (already present) and
handling `update_is_del_and_insert` plans whose child carries all
columns already.

### Upstream check

No changeset/CDC extension hook exists in DuckDB through the 1.5.x line.
[Discussion #12408](https://github.com/duckdb/duckdb/discussions/12408)
(CDC support) is open with no shipped API; release notes for 1.4/1.5 add
none. Not coupling to an upgrade. Re-check on the next engine bump.

## Whitelist (capturable shapes)

UPDATE:
- single statement, target is a plain base table, tracked
- no `FROM` clause, no `RETURNING`, no CTEs, no `SET col = DEFAULT`
- SET expressions and WHERE contain no subqueries and no prepared-
  statement parameters
- all functions in SET/WHERE bind to `FunctionStability::CONSISTENT`
  (rejects `random()`, `now()` — `CONSISTENT_WITHIN_QUERY` is volatile
  *across* queries and the capture SELECT is a different query)
- no SET column is part of any index and every SET column type
  `SupportsRegularUpdate()` — otherwise DuckDB executes the update as
  delete+re-append (`update_is_del_and_insert`,
  duckdb `table_catalog_entry.cpp:327-347`) and rowids are not stable for
  the guard
- target table not previously written in this transaction (the internal
  connection sees committed state only)

DELETE:
- single statement, plain tracked base-table target
- no `RETURNING`, no CTEs, no parameters, CONSISTENT functions only,
  target untouched this txn
- missing WHERE (delete-all) is capturable
- `USING` refs and subquery predicates: see below

**Subquery predicates and DELETE USING** (follow-up, same release): a
subquery in WHERE/SET reads OTHER tables, which the capture probe sees at
committed state — exact only when the statement's own view IS committed
state: autocommit, or an explicit transaction before its FIRST write
(`wrote_any` gate; a prior write to any table poisons it). Subquery row
sources are vetted with the INSERT ... SELECT repeatability rules.
`DELETE t USING u WHERE cond` is a semi-join delete and rewrites to
`WHERE EXISTS (SELECT 1 FROM u WHERE cond)` in the capture probe, same
visibility gate. `UPDATE ... FROM` stays out: with multiple FROM matches
per target row the SET result is nondeterministic.

**Upserts** (follow-up, same release): `INSERT ... ON CONFLICT (cols)
DO UPDATE SET` / `DO NOTHING` with an explicit conflict target probes
committed state with a LEFT JOIN from the row source (aliased `excluded`,
so the statement's own `excluded.*` references bind unchanged) to the
target on the conflict columns. NULL probe rowid = insert-part row (+1);
matched rows emit old/new pairs (rowid-verified) for DO UPDATE and
nothing for DO NOTHING. Unqualified target columns in SET are ambiguous
in the probe SELECT and decline via query error; conditional
`DO ... WHERE`, `OR REPLACE`, implicit conflict targets, and SET on
indexed columns fall back. Duplicate conflict keys inside a DO NOTHING
source insert fewer rows than predicted — the COUNT(*) guard catches it.

Everything else — UPDATE...FROM, DELETE USING, multi-statement
strings, Appender writes, unparseable SQL — keeps today's behavior:
poison the transaction's capture, scan-and-diff the touched tables at
commit. If any statement in a transaction is un-capturable the whole
transaction falls back; captured and scanned deltas are never mixed for
one commit.

Autocommit UPDATE/DELETE is captured too: the capture SELECT needs no
transaction internals (QueryBegin runs before the statement, and the
autocommit TransactionCommit hook — which fires mid-statement, post-commit
— runs the guard and applies, exactly where the scoped-scan fallback runs
today).

**Autocommit INSERT** (follow-up, shipped with this feature): the row
source — a VALUES list or a whitelisted deterministic SELECT — is
evaluated by one internal SELECT with the INSERT's own to-column-type
casts, projected into table column order; columns missing from a partial
column list take their declared DEFAULT (or NULL):

```sql
-- INSERT INTO t (b, a) VALUES (e1, e2), ...  becomes
SELECT CAST(v.a AS ...), CAST(v.b AS ...), <default/NULL...>
FROM (VALUES (e1, e2), ...) v(b, a)
-- INSERT INTO t SELECT ... uses the SELECT itself as the source
```

Whitelist: no `BY NAME`, no `DEFAULT VALUES` form, same expression rules
as UPDATE/DELETE. SELECT sources must produce a REPEATABLE row set (they
run twice — capture, then the statement): no LIMIT/SAMPLE (row choice
depends on scan order under parallel scans), no table functions (no
stability metadata), no window functions (tie order), no CTEs. Volatile
column defaults (`nextval()`) fail the bound-plan stability check. The
guard is commit-seq + signed COUNT(*) only — rowids are unknowable before
the insert executes, and predicted rows use the same CAST the INSERT
itself applies. Explicit-transaction INSERTs stay on G2 (exact
LocalStorage scan; capturing both would double-count). Gotcha found
during implementation:
autocommit statements DO have an active transaction at QueryBegin
(`TransactionContext::IsAutoCommit()` distinguishes), and VALUES rows bind
into `LogicalExpressionGet::expressions` — a member that *shadows* the
base-class `expressions` — so the stability walk visits it explicitly.

## Commit guard (replaces count-only guard for write captures)

The existing COUNT(*) guard is count-blind to UPDATE. Three checks, all
O(Δ) or metadata-speed, run at commit before applying:

1. **Commit-sequence conflict check.** `CDCManager::commit_seq_` (atomic)
   bumps whenever any path mutates a tracked baseline (captured apply,
   scan sync, notify, rebuild). A capturing transaction snapshots the seq
   at TransactionBegin (at QueryBegin for autocommit); a different value
   at commit means another transaction's delta landed mid-flight — the
   capture's committed-state SELECT may have diverged from this
   transaction's snapshot. Fall back. Every in-process writer passes
   through these hooks (Appender included, via its commit), and DuckDB's
   file lock excludes other processes.
2. **COUNT(*) guard, signed.** `baseline_weight + Σ(captured weights) ==
   COUNT(*)` per table — unchanged mechanism, now over the merged
   append+write delta (UPDATE contributes 0, DELETE negative).
3. **Rowid re-verification.** Post-commit, via the cached guard
   connection: captured DELETE rowids must be absent
   (`SELECT count(*) ... WHERE rowid IN (...)` = 0); captured UPDATE
   rowids must return exactly the predicted post-image values
   (`SELECT rowid, cols ... WHERE rowid IN (...)`, compared value-by-value).
   Rowid IN-list pruning keeps this O(Δ). This catches volatile functions
   that slipped the stability check, snapshot skew on the captured rows,
   and any post-image mis-prediction.

Any guard failure ⇒ scan-and-diff fallback for the touched tables,
counted in a new `capture_guard_fallbacks_` counter (loud). Guards are
best-effort under concurrency exactly like the existing G2 guard;
scan-and-diff remains the correctness backstop. Correctness never depends
on capture.

Residual risk accepted: an UPDATE row we never captured (predicate
matched a row invisible to the capture SELECT) with no interleaved
tracked-table commit is undetectable by checks 1–3 — but check 1 already
poisons every interleaving that could cause it in-process, and no
out-of-process writer can exist.

## Watermarks, checkpoints, deferred baselines

- `_dbsp_ckpt_meta` watermarks are computed from live tables at
  `dbsp_save` time and verified at load; captured deltas leave committed
  storage as the sole source of truth, so nothing new to maintain.
  Round-trip covered by a test.
- D3c deferred baselines: QueryBegin already materializes all deferred
  baselines before any write statement executes (pre-write storage is the
  only exact moment), so a captured write never races a deferred baseline;
  `apply_captured_delta` additionally calls `prepare_deferred_for_delta`,
  which handles signed deltas. Covered by a test, not assumed.
- `dbsp_changes(view)` semantics untouched (same `propagate_changes`
  path). `InternalQueryGuard` wraps every new capture/guard query.

## Observability

- `captured_delta_syncs_` (existing) now also counts write-captured
  applies; `scan_syncs_` unchanged.
- New `capture_guard_fallbacks_` counts guard-rejected captures.
- `CDCManager::set_write_capture_enabled(bool)` (C++-level, default on)
  lets tests force the scan path for differentials.

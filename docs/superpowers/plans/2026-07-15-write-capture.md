# O(Δ) UPDATE/DELETE Auto-Sync Capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make auto-sync O(Δ) for whitelisted UPDATE/DELETE statements on tracked tables (explicit-txn and autocommit), with a generalized commit guard, never regressing the notify path or correctness.

**Architecture:** At QueryBegin, a whitelisted UPDATE/DELETE on a tracked table runs one internal-connection SELECT that captures old images and (for UPDATE) computes new images by projecting SET expressions cast to column types. Signed deltas buffer in the existing per-txn `TxnCapture` and apply at commit through `apply_captured_delta` after a three-part guard (commit-seq conflict, signed COUNT(*), rowid re-verification). Design rationale: `docs/DESIGN_WRITE_CAPTURE.md`.

**Tech Stack:** C++17 header-only extension code, DuckDB v1.5.4 internals (Parser, ExtractPlan, TableCatalogEntry::GetStorageInfo), Catch2 tests in `test/build_test`.

## Global Constraints

- DuckDB pinned v1.5.4; no upstream upgrade coupling.
- Reuse `apply_captured_delta` — no parallel delta pipeline.
- Whole-txn fallback on any un-capturable statement; never mix captured and scanned deltas in one commit; correctness never depends on capture.
- All internal queries wrapped in `InternalQueryGuard`.
- Perf gate: single-row UPDATE at 1M rows ≤ 50ms (target ~7ms).
- ctest only from `test/build_test`. Build: `cd test/build_test && cmake .. -DENABLE_UBSAN=OFF && make`.
- Notify API path untouched.

---

### Task 1: Write-capture vetting + capture-SQL builder (`dbsp_write_capture.hpp`)

**Files:**
- Create: `include/dbsp_write_capture.hpp`
- Test: `test/unit/test_write_capture.cpp` (add to `test/CMakeLists.txt` beside other unit tests)

**Interfaces:**
- Produces: `dbsp_native::WriteCapturePlan { Kind kind; std::string capture_sql; size_t n_cols; std::vector<size_t> set_col_positions; }` and `std::unique_ptr<WriteCapturePlan> plan_write_capture(duckdb::ClientContext &context, duckdb::SQLStatement &stmt, const duckdb::TableCatalogEntry &entry)` returning nullptr when not capturable.

- [x] **Step 1: Failing unit tests** — shape matrix: plain UPDATE capturable; UPDATE...FROM / RETURNING / CTE / SET DEFAULT / subquery WHERE / param / indexed SET column / LIST column rejected; DELETE plain + no-WHERE capturable; DELETE USING rejected; generated SQL text exact (rowid + quoted cols + CASTs).
- [x] **Step 2: Run, verify fail** (`ctest -R write_capture` from `test/build_test`).
- [x] **Step 3: Implement.** Parse-tree vetting (statement fields per duckdb 1.5.4 headers verified: `UpdateStatement{table,from_table,returning_list,set_info{condition,columns,expressions},cte_map}`, `DeleteStatement{condition,table,using_clauses,returning_list,cte_map}`); recursive expression walk rejecting `ExpressionClass::SUBQUERY`, `PARAMETER`, `DEFAULT`; index-overlap + `SupportsRegularUpdate()` check via `entry.GetStorageInfo(context)`; SQL builder using `quote_table_key`/quoted idents and `CAST((expr) AS <col.Type().ToString()>)`.
- [x] **Step 4: Tests pass.**
- [x] **Step 5: Commit** `feat(capture): UPDATE/DELETE statement vetting + capture-query builder`.

### Task 2: Manager support — commit seq, counters, test knob

**Files:**
- Modify: `include/dbsp_cdc.hpp`

**Interfaces:**
- Produces: `uint64_t CDCManager::commit_seq() const`; seq bumped in `apply_captured_delta`, scan-sync consume path (`scan_syncs_++` site), notify `on_insert`/`on_delete`/`on_update`, `rebuild_all_views`; `uint64_t capture_guard_fallbacks() const` + `void note_capture_guard_fallback()`; `bool write_capture_enabled() const` + `void set_write_capture_enabled(bool)` (default true).

- [x] Steps: failing unit test (seq bumps on notify + scan sync; knob toggles) → implement (`std::atomic<uint64_t> commit_seq_{0}`, `capture_guard_fallbacks_{0}`, `std::atomic<bool> write_capture_enabled_{true}`) → pass → commit `feat(capture): commit-sequence counter + capture observability`.

### Task 3: Context-state wiring — capture, guard, apply

**Files:**
- Modify: `include/dbsp_context_state.hpp` (header comment rewritten too)

**Interfaces:**
- Consumes: Task 1 `plan_write_capture`, Task 2 manager API.
- Produces: `TxnCapture` gains `std::unordered_map<std::string, DuckDBZSet> write_deltas; std::unordered_map<std::string, std::vector<WriteVerify>> verifies; uint64_t seq_snapshot; bool wrote_capture` where `WriteVerify{int64_t rowid; bool is_delete; DuckDBRow expected_new;}`.

- [x] **Failing integration smoke test** (in `test/integration/test_auto_cdc.cpp`): explicit-txn UPDATE and DELETE on tracked table sync views with `captured_delta_syncs` +1 and `scan_syncs` flat; autocommit variants likewise.
- [x] **Implement:**
  - `TransactionBegin`: `capture_.seq_snapshot = manager.commit_seq()`.
  - `QueryBegin` (before `fold_into_txn`): WRITE_KNOWN UPDATE/DELETE + tracked + `write_capture_enabled` + target not in `capture_.touched` → `plan_write_capture`; volatility vetting via `guard_con_->ExtractPlan(capture_sql)` walking bound expressions for `FunctionStability::CONSISTENT`; execute capture SELECT under `InternalQueryGuard`; build signed ZSet (old −1, new +1) + verify list; explicit txn: merge into `capture_`, mark `active`, fold target into `touched` WITHOUT `dirty`; autocommit (no active txn): stash in `stmt_write_` with seq snapshot taken now. Any failure → existing dirty/fallback path.
  - `apply_captured` (commit guard): (1) if `wrote_capture` and `manager.commit_seq() != seq_snapshot` → false; (2) count guard over merged appends+write_deltas signed sum; (3) rowid re-verify via `guard_con_` in 512-rowid IN batches — deletes absent, updates match `expected_new` value-by-value (`DuckDBRow::operator==` NULL-safe); (4) apply ONE merged delta per table via `apply_captured_delta`. Failure → `note_capture_guard_fallback()` + return false (scan fallback).
  - `TransactionCommit` autocommit branch: before fold-and-sync, if `stmt_write_` pending → run same guard/apply (post-commit state readable here, same as today's `sync_tables` fallback site); success skips sync, failure falls through to scoped scan. Clear `stmt_write_` in `TransactionRollback`, `TransactionBegin`, and at each `QueryBegin`.
- [x] Pass → commit `feat(capture): O(Δ) auto-sync capture for UPDATE and DELETE`.

### Task 4: Differential test matrix

**Files:**
- Modify: `test/integration/test_auto_cdc.cpp`

- [x] Matrix (each over aggregate view + join view + chained view; assert view rows identical across (a) captured auto-sync, (b) `set_write_capture_enabled(false)` scan path, (c) notify-API replay; assert intended path via counters): single-row UPDATE; multi-row expression UPDATE (`SET val = val + 1`); UPDATE moving a group-by key; DELETE with subquery WHERE (fallback); DELETE USING (fallback); mixed INSERT+UPDATE+DELETE across different tables (captured) and same table (fallback); rollback discards; upsert (fallback); NULL SET/WHERE; `random()` predicate (fallback); UPDATE on indexed/PK column (fallback); autocommit UPDATE + DELETE (captured).
- [x] dbsp_save/dbsp_load round-trip with captured UPDATE/DELETE history; captured UPDATE immediately after watermark-matched load (D3c deferred interplay).
- [x] Commit `test(capture): differential matrix for write capture`.

### Task 5: Perf gate

**Files:**
- Modify: `test/benchmarks/bench_incremental.cpp`

- [x] `TEST_CASE("Benchmark: single-row UPDATE capture at 1M rows", "[benchmark]")`: 1M-row tracked table + agg view, auto-sync on, time one autocommit UPDATE; `REQUIRE(ms <= 50)`; assert captured counter bumped; print ms. Commit `bench(capture): 1M-row single-UPDATE gate`.

### Task 6: Docs + full suite

**Files:**
- Modify: `docs/API.md` (auto-sync section), `TODO.md` (remove stale version-info blocker lines), `CHANGELOG.md`, `docs/ARCHITECTURE.md` (auto-sync flow + diagram if capture path changes it), header comments in touched files.

- [x] Full suite green from `test/build_test`; docs updated in same/follow-up commit `docs: write-capture design, API, TODO, CHANGELOG`.

## Self-Review Notes

- Spec coverage: capture (T1/T3), guard generalization (T3), counters (T2), watermark/D3c (T4 tests), autocommit (T3/T4), perf (T5), docs+findings (T6 + DESIGN_WRITE_CAPTURE.md). Rejected design 2 justified in findings doc.
- Type consistency: `WriteCapturePlan`/`WriteVerify` names used consistently across T1/T3.
- No placeholders: statement field names verified against pinned duckdb headers; guard batching constant fixed at 512.

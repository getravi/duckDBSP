# Linear-Recursion Signed Deltas Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route retraction-bearing deltas through the incremental fixpoint path for linear UNION ALL recursion, eliminating the ~590ms/commit full-recompute that dominates the wfp sync floor — spec: `docs/superpowers/specs/2026-07-31-linear-recursion-deltas-design.md`.

**Architecture:** All changes in `include/dbsp_plan_translator.hpp` (`PlanRecursiveNode` + the `REC_CTE` build site) plus tests/docs.

**Tech Stack:** C++17, Catch2 integration tests, existing `recompute()` as differential oracle.

## Global Constraints

- Repo rules: docs in the SAME commit (CHANGELOG.md, docs/ARCHITECTURE.md WITH RECURSIVE paragraph, docs/THEORY.md if it covers recursion); `ctest` only from `test/build_test`; `make <target> -j 4`, never bare `-j`; scratch to the session scratchpad.
- Full ctest green at every commit (`test_recursive`, `test_recursive_deletion` are the regression net). Commit trailer:
  Claude-Session: https://claude.ai/code/session_01TGZr1ZRcKV3jmWqoHMck8s
- The signed path must NEVER emit a partial delta: on max-iterations or any structural failure, fall back to `recompute()` within the same `step()` (log under `DBSP_DEBUG_SYNC`).
- UNION (set) recursion and nonlinear UNION ALL behavior must be bit-for-bit unchanged.

---

### Task 1: Linearity detection + signed incremental path + differential tests

**Files:**
- Modify: `include/dbsp_plan_translator.hpp` — `PlanRecursiveNode` (ctor + `step()` at ~3148-3196, `admit`/`iterate` at ~3389-3420 region) and the `REC_CTE` build site (~3970-3998)
- Test: `test/integration/test_recursive_deletion.cpp` (extend; follow its existing harness pattern)
- Docs: `CHANGELOG.md`, `docs/ARCHITECTURE.md`, `docs/THEORY.md` (same commit)

**Interfaces:**
- Produces: `PlanRecursiveNode` ctor gains `bool linear_step` (default false, explicit at the one call site); `step()` fast-routes `union_all_ && linear_step_` deltas (any sign) through the incremental path.
- Consumes: existing `admit`/`iterate`/`accumulated_`/`step_view_` machinery; `PlanOpSpec` tree (`spec.children[1]`) for the linearity count.

- [ ] **Step 1: Write the failing differential test**

In `test/integration/test_recursive_deletion.cpp`, following its existing setup pattern:

```cpp
TEST_CASE("Linear UNION ALL recursion handles retractions incrementally",
          "[integration][recursive][linear-delta]") {
    DuckDBTestHarness db;
    // wfp roll-forward shape: chain per partition over an ordered spine
    db.createTable("net", "p INT, t INT, v DOUBLE",
                   {"(1, 0, 10.0)", "(1, 1, 20.0)", "(1, 2, 30.0)",
                    "(2, 0, 5.0)",  "(2, 1, 5.0)",  "(2, 2, 5.0)"});
    db.exec("SELECT * FROM dbsp_track('net')");
    db.exec("SELECT * FROM dbsp_sync('net')");
    db.exec("CREATE MATERIALIZED VIEW chain AS "
            "WITH RECURSIVE rf AS ("
            "SELECT p, t, v AS acc FROM net WHERE t = 0 "
            "UNION ALL "
            "SELECT n.p, n.t, rf.acc + n.v FROM rf "
            "JOIN net n ON n.p = rf.p AND n.t = rf.t + 1) "
            "SELECT * FROM rf");
    auto expect = [&](std::vector<std::vector<double>> rows) {
        auto got = db.query("SELECT p, t, acc FROM dbsp_query('chain') "
                            "ORDER BY p, t");
        REQUIRE(got->RowCount() == rows.size());
        for (size_t i = 0; i < rows.size(); i++)
            for (size_t j = 0; j < 3; j++)
                REQUIRE(got->GetValue(j, i).GetValue<double>() == rows[i][j]);
    };
    expect({{1,0,10},{1,1,30},{1,2,60},{2,0,5},{2,1,10},{2,2,15}});

    // UPDATE = retraction + insertion; chain must re-carry downstream
    db.exec("UPDATE net SET v = 100.0 WHERE p = 1 AND t = 1");
    db.exec("SELECT * FROM dbsp_sync('net')");
    expect({{1,0,10},{1,1,110},{1,2,140},{2,0,5},{2,1,10},{2,2,15}});

    // Delete a mid-chain link: downstream of (1,1) disappears
    db.exec("DELETE FROM net WHERE p = 1 AND t = 1");
    db.exec("SELECT * FROM dbsp_sync('net')");
    expect({{1,0,10},{2,0,5},{2,1,10},{2,2,15}});

    // Re-insert: chain re-derives
    db.exec("INSERT INTO net VALUES (1, 1, 20.0)");
    db.exec("SELECT * FROM dbsp_sync('net')");
    expect({{1,0,10},{1,1,30},{1,2,60},{2,0,5},{2,1,10},{2,2,15}});
}
```

Correctness is asserted against hand-computed truth here; the same values are what `recompute()` produces today — so this test passes TODAY via the slow path. The failing half is the routing assertion:

```cpp
TEST_CASE("Linear UNION ALL retractions do NOT full-recompute",
          "[integration][recursive][linear-delta]") {
    // Same setup as above, then: capture recompute invocations. Add a
    // test-only counter on PlanRecursiveNode (e.g. `size_t recompute_count_`
    // exposed via a friend/test hook or an env-gated stderr line under
    // DBSP_TIMING) — assert an UPDATE on `net` does not increment it.
}
```

(Concretely: `recompute()` already has a distinctive cost signature; the clean hook is a `static std::atomic<size_t> recompute_invocations` on the node, incremented in `recompute()`, readable from the test via a new `dbsp_recursive_stats()` table function OR — simpler and acceptable — an env-gated stderr line the test harness greps. Pick the smallest mechanism the harness supports; document the choice.)

- [ ] **Step 2: Run — value test passes (slow path), routing test FAILS** (recompute still invoked on retraction).

Run: `cd test/build_test && make test_recursive_deletion -j 4 && ./test_recursive_deletion "[linear-delta]"`

- [ ] **Step 3: Implement linearity detection**

At the `REC_CTE` build site (~3970): walk `*spec.children[1]` (same shape as `count_sources`) counting `Kind::SOURCE` nodes whose `table == sentinel`. `linear = (count == 1)`. Pass to the ctor. Note: `step_view->source_tables()` is deduped — do NOT use it for the count; walk the spec tree.

- [ ] **Step 4: Implement the signed path**

In `step()`: replace the `has_deletion` gate with:

```cpp
if (has_deletion && !(union_all_ && linear_step_)) {
  if (union_all_) recompute(); else dred(anchor_delta, base_deltas);
  has_output_ = !output_.empty();
  return;
}
```

Then generalize the incremental path below it: remove the `w > 0` filter on the seed loop; `admit`'s `union_all_` branch must do signed multiset arithmetic on `accumulated_` (add weight; erase on zero; emit the signed change to frontier + out). Wrap the iteration in the fallback guard: if `iterate` trips `max_iterations_`, discard partial state (restore `accumulated_`/step_view via `recompute()`, which rebuilds from integrated inputs) and log under `DBSP_DEBUG_SYNC`. Verify `step_view_->apply_changes` integrates negative weights into its local join indexes correctly (read `integrate`; it should — weights are added signed — but VERIFY, and note the evidence in your report).

Also confirm `iterate()`'s sentinel push feeds signed frontier weights to the step view unchanged (read it; the sentinel source's `push_borrowed` path is weight-agnostic).

- [ ] **Step 5: Green + full ctest**

Both `[linear-delta]` tests pass; `./test_recursive` and `./test_recursive_deletion` full runs green; full `ctest` green.

- [ ] **Step 6: Docs + commit**

CHANGELOG (the wfp numbers motivating it); ARCHITECTURE WITH RECURSIVE paragraph gains the linear-signed-path sentence; THEORY.md if it discusses recursion. Commit: `feat: signed incremental deltas for linear UNION ALL recursion`.

---

### Task 2: End-to-end wfp validation + benchmark

**Files:** none committed in duckDBSP beyond (if needed) report fixes; NumPad spec-doc addendum committed in the NumPad repo by the controller afterward (NOT this task).

- [ ] **Step 1:** Rebuild loadable extension (`cd build && make -j 4`).
- [ ] **Step 2:** wfp profile re-run (scratchpad, pattern from the controller's earlier probe): fresh spike300 working copy → create views → 8 UPDATE edits with `DBSP_TIMING=1` → confirm `view_step v_closingheadcount` drops from ~590ms to low single-digit ms and no `recompute` signature appears; then `parity.check(con, 'oracle') == []` after reverting the edit (NumPad repo venv, `uv run`).
- [ ] **Step 3:** Full 300-employee benchmark: fresh dump + `uv run python scripts/spike_full_sql/driver.py --db <scratch>/spike300.duckdb --json <scratch>/results300_linear.json`. Record p25/median both scenarios both arms + delta counts (must be unchanged vs the 2026-07-31 fixed-stack run — same logical outputs) + parity all clean.
- [ ] **Step 4:** Report numbers + any surprises to `.superpowers/sdd/2026-07-31-linear-recursion-deltas/task-2-report.md`. Full ctest once more; clean `git status` both repos.

---

## Self-review notes

- Spec coverage: detection (T1S3), signed path + fallback (T1S4), differential/value tests incl. mid-chain delete + re-derive (T1S1), gate tests = existing suites unchanged + routing assertion, benchmark validation (T2). Non-goals honored (no checkpoint/state change, DRed untouched).
- The riskiest unknown is flagged as a verify-in-code step, not assumed: signed-weight integration in the step view's local indexes and the sentinel push path.
- Placeholder check: the routing-assertion mechanism offers two concrete implementations and demands the choice be documented — deliberate latitude, not a TBD.

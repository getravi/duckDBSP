# Incremental Recursive Deletion (DRed) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `WITH RECURSIVE ... UNION` view maintenance incremental under deletions via the Delete-Rederive (DRed) algorithm, replacing the current full fixed-point recompute.

**Architecture:** `PlanRecursiveNode::step()` currently routes any negative-weight input to `recompute()` (O(iterations × relation)). Add a `dred()` method for set-semantics (`!union_all_`) deletions: an incremental overdelete pass (retraction fixpoint that over-approximates) followed by a rederive pass (re-admits over-deleted rows that still have alternative support). `recompute()` stays as the UNION ALL path and as the test oracle. The `step_view_` sub-circuit (linear join+project) keeps its integrated base state throughout; the node maintains the invariant `accumulated_` set == `step_view_` sentinel arrangement.

**Tech Stack:** C++17, DuckDB 1.5.4 static, Catch2 v2, CMake. All work in `include/dbsp_plan_translator.hpp` (header-only) + a new integration test file.

## Global Constraints

- DuckDB engine pinned v1.5.4 — do not change.
- Header-only extension: implementation lives in `include/dbsp_plan_translator.hpp`.
- Set-semantics (`UNION`) recursion only. `UNION ALL` (`union_all_ == true`) keeps `recompute()` unchanged.
- `recompute()` MUST remain in the file (UNION ALL path + differential oracle).
- Build/test only from `test/build_test` (project artifact-hygiene rule).
- Recursive step term is linear (join/project/filter, no aggregate/distinct) — guaranteed by SQL's recursive-CTE restrictions; the algorithm relies on it.
- Assume `DuckDBZSet` API: `insert(row, w)`, `get(row) -> int64_t`, `empty()`, `size()`, `clear()`, range-for yields `const auto &[row, w]`.

---

## File Structure

- `include/dbsp_plan_translator.hpp` — add `PlanRecursiveNode::dred(...)`, re-route the deletion branch in `step()`. No other class changes.
- `test/integration/test_recursive_deletion.cpp` — **new** differential test file (harness-driven; oracle = direct DuckDB recursive query).
- `test/CMakeLists.txt` — register the new integration test.
- `TODO.md`, `docs/THEORY.md` (or nearest), `include/dbsp_plan_translator.hpp` header comment — doc updates.

---

## Task 1: Differential test harness for recursive deletion (safety net)

These tests pass against the **current** `recompute()` path — they exist to guard the refactor in Task 2. Write them first; they must stay green throughout.

**Files:**
- Create: `test/integration/test_recursive_deletion.cpp`
- Modify: `test/CMakeLists.txt` (after line 90, the existing `recursive_integration` registration)
- Test build dir: `test/build_test`

**Interfaces:**
- Consumes: `dbsp_test::DuckDBTestHarness` (`exec`, `query`, `getViewRows`) from `test/test_helpers.hpp`.
- Produces: a reusable free helper `assertViewMatchesOracle(harness, view_name, recursive_sql, base_table)` used by all deletion tests and by Task 1's property test.

- [ ] **Step 1: Write the failing test file**

Create `test/integration/test_recursive_deletion.cpp`:

```cpp
// Differential tests for incremental recursive-view DELETION (DRed).
// Oracle: the same WITH RECURSIVE query run directly against the base table
// (non-incremental) must equal the incrementally-maintained view after every
// mutation.

#include "../test_helpers.hpp"
#include <algorithm>
#include <random>

using namespace dbsp_test;
using namespace duckdb;

namespace {

// Normalize a result set to a sorted multiset of stringified rows.
std::vector<std::string> sortedRows(const std::vector<std::vector<Value>> &rows) {
  std::vector<std::string> out;
  out.reserve(rows.size());
  for (const auto &r : rows) {
    std::string s;
    for (const auto &v : r) {
      s += v.IsNull() ? std::string("NULL") : v.ToString();
      s += '|';
    }
    out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> queryRows(DuckDBTestHarness &h, const std::string &sql) {
  auto result = h.query(sql);
  REQUIRE_FALSE(result->HasError());
  std::vector<std::vector<Value>> rows;
  for (size_t i = 0; i < result->RowCount(); i++) {
    std::vector<Value> row;
    for (size_t j = 0; j < result->ColumnCount(); j++)
      row.push_back(result->GetValue(j, i));
    rows.push_back(std::move(row));
  }
  return sortedRows(rows);
}

// Assert the incremental view equals the oracle recursive query.
// `oracle_sql` is a self-contained WITH RECURSIVE ... SELECT run on base tables.
void assertViewMatchesOracle(DuckDBTestHarness &h, const std::string &view_name,
                             const std::string &oracle_sql) {
  auto view = sortedRows(h.getViewRows(view_name));
  auto oracle = queryRows(h, oracle_sql);
  INFO("view rows=" << view.size() << " oracle rows=" << oracle.size());
  REQUIRE(view == oracle);
}

const char *kTcOracle =
    "WITH RECURSIVE tc AS ("
    "  SELECT src, dst FROM edges "
    "  UNION "
    "  SELECT tc.src, edges.dst FROM tc JOIN edges ON tc.dst = edges.src"
    ") SELECT * FROM tc";

// Create the transitive-closure view over an `edges(src,dst)` table.
void makeTcView(DuckDBTestHarness &h) {
  auto r = h.query(std::string("SELECT * FROM dbsp_create_view('tc', '") +
                   "WITH RECURSIVE tc AS ("
                   "  SELECT src, dst FROM edges "
                   "  UNION "
                   "  SELECT tc.src, edges.dst FROM tc JOIN edges ON tc.dst = edges.src"
                   ") SELECT * FROM tc')");
  REQUIRE_FALSE(r->HasError());
}

} // namespace

TEST_CASE("Recursive deletion: edge removal kills reachability",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  h.exec("INSERT INTO edges VALUES (1,2),(2,3),(3,4)");
  makeTcView(h);
  assertViewMatchesOracle(h, "tc", kTcOracle); // 6 rows

  h.exec("DELETE FROM edges WHERE src=2 AND dst=3");
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle); // now {(1,2),(3,4)}
}

TEST_CASE("Recursive deletion: alternate path keeps derived rows",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  // 1->2->4 and 1->3->4 : (1,4) has two supporting paths.
  h.exec("INSERT INTO edges VALUES (1,2),(2,4),(1,3),(3,4)");
  makeTcView(h);
  assertViewMatchesOracle(h, "tc", kTcOracle);

  // Remove one path; (1,4) must survive on the other (rederive).
  h.exec("DELETE FROM edges WHERE src=2 AND dst=4");
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle);
}

TEST_CASE("Recursive deletion: cycle edge removal",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  h.exec("INSERT INTO edges VALUES (1,2),(2,3),(3,1),(3,4)"); // cycle 1-2-3
  makeTcView(h);
  assertViewMatchesOracle(h, "tc", kTcOracle);

  h.exec("DELETE FROM edges WHERE src=3 AND dst=1"); // break the cycle
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle);
}

TEST_CASE("Recursive deletion: insert then delete round-trip",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  h.exec("INSERT INTO edges VALUES (1,2),(2,3)");
  makeTcView(h);
  assertViewMatchesOracle(h, "tc", kTcOracle);

  h.exec("INSERT INTO edges VALUES (3,4)");
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle);

  h.exec("DELETE FROM edges WHERE src=3 AND dst=4");
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle); // back to 3-row state
}

TEST_CASE("Recursive deletion: randomized differential (DRed == oracle)",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  makeTcView(h);

  std::mt19937 rng(1234);
  std::uniform_int_distribution<int> node(1, 7);
  std::set<std::pair<int, int>> present;

  for (int round = 0; round < 200; round++) {
    int s = node(rng), d = node(rng);
    bool insert = (rng() & 1);
    if (insert && !present.count({s, d})) {
      h.exec("INSERT INTO edges VALUES (" + std::to_string(s) + "," +
             std::to_string(d) + ")");
      present.insert({s, d});
    } else if (!insert && present.count({s, d})) {
      h.exec("DELETE FROM edges WHERE src=" + std::to_string(s) +
             " AND dst=" + std::to_string(d));
      present.erase({s, d});
    } else {
      continue; // no-op round
    }
    h.exec("SELECT * FROM dbsp_sync('edges')");
    assertViewMatchesOracle(h, "tc", kTcOracle);
  }
}
```

- [ ] **Step 2: Register the test in CMake**

In `test/CMakeLists.txt`, add immediately after line 90 (`recursive_integration`):

```cmake
add_dbsp_integration_test(recursive_deletion integration/test_recursive_deletion.cpp)
```

- [ ] **Step 3: Build and run — expect PASS (current recompute is correct)**

```bash
cd test/build_test && cmake .. >/dev/null && make test_recursive_deletion 2>&1 | tail -5 && ./test_recursive_deletion
```
Expected: all sections PASS. (Current `recompute()` already handles deletions correctly; these tests are the refactor's safety net.) If any FAIL, stop — the oracle or view setup is wrong, fix before Task 2.

- [ ] **Step 4: Commit**

```bash
git add test/integration/test_recursive_deletion.cpp test/CMakeLists.txt
git commit -m "test: differential recursive-deletion suite (DRed safety net)"
```

---

## Task 2: DRed replaces recompute on the UNION deletion path

**Files:**
- Modify: `include/dbsp_plan_translator.hpp` — `PlanRecursiveNode::step()` deletion branch (~line 2858) and add `dred(...)` method (after `recompute()`, ~line 2928).
- Test: `test/integration/test_recursive_deletion.cpp` (unchanged; must stay green).

**Interfaces:**
- Consumes: existing members `accumulated_`, `step_view_` (`apply_changes(src, delta)` / `get_delta()`), `sentinel_`, `output_`, `max_iterations_`; existing `anchor_()` result and `base_deltas` local built in `step()`.
- Produces: `void dred(const DuckDBZSet &anchor_delta, const std::vector<std::pair<std::string, const DuckDBZSet *>> &base_deltas)` — mutates `accumulated_` and fills `output_` with the signed change.

- [ ] **Step 1: Re-route the deletion branch to `dred` for set semantics**

In `step()`, replace the existing deletion block:

```cpp
    if (has_deletion) {
      recompute();
      has_output_ = !output_.empty();
      return;
    }
```

with:

```cpp
    if (has_deletion) {
      if (union_all_) {
        recompute(); // multiplicity semantics: full recompute (see design)
      } else {
        dred(anchor_delta, base_deltas);
      }
      has_output_ = !output_.empty();
      return;
    }
```

- [ ] **Step 2: Add the `dred` method**

Insert immediately after the closing brace of `recompute()` (before `iterate`):

```cpp
  // Delete-Rederive for set-semantics (UNION) recursion. Overdelete
  // over-approximates the retraction; rederive restores rows that still
  // have alternative support. Maintains the invariant
  //   accumulated_ (set) == step_view_ sentinel arrangement
  // by feeding every accumulated_ change to the sentinel source. output_
  // is built incrementally (no full diff). See docs spec 2026-07-07.
  void dred(const DuckDBZSet &anchor_delta,
            const std::vector<std::pair<std::string, const DuckDBZSet *>>
                &base_deltas) {
    // --- Seed: split inputs into retractions (drive overdelete) and
    //     insertions (seed rederive). ---
    DuckDBZSet frontier;    // rows being retracted this round (weight 1 each)
    DuckDBZSet insert_seed; // positive rows to (re)admit during rederive
    auto retract = [&](const DuckDBRow &row) {
      if (accumulated_.get(row) != 0) {
        accumulated_.insert(row, -1); // 1 + (-1) = 0 → removed
        output_.insert(row, -1);
        frontier.insert(row, 1);
      }
    };
    auto classify = [&](const DuckDBZSet &z) {
      for (const auto &[row, w] : z) {
        if (w < 0)
          retract(row);
        else if (w > 0)
          insert_seed.insert(row, 1);
      }
    };
    classify(anchor_delta);
    for (const auto &[table, d] : base_deltas) {
      step_view_->apply_changes(table, *d); // base delta already integrated
      classify(step_view_->get_delta());    // derived reactions
    }

    // --- Overdelete: iterate the retraction frontier through the step. ---
    size_t iter = 0;
    while (!frontier.empty() && iter++ < max_iterations_) {
      DuckDBZSet neg;
      for (const auto &[row, w] : frontier)
        neg.insert(row, -w); // retract frontier from sentinel arrangement
      step_view_->apply_changes(sentinel_, neg);
      DuckDBZSet next;
      for (const auto &[row, w] : step_view_->get_delta()) {
        if (w < 0 && accumulated_.get(row) != 0) {
          accumulated_.insert(row, -1);
          output_.insert(row, -1);
          next.insert(row, 1);
        }
      }
      frontier = std::move(next);
    }

    // --- Rederive: recompute the step's one-step image over survivors, then
    //     iterate; re-admit any over-deleted row that reappears. ---
    // Clear the sentinel side (survivors → empty), then re-present survivors
    // so the resulting get_delta() is the full image I(survivors). Base
    // arrangements are untouched, so this is a linear round-trip.
    DuckDBZSet survivors = accumulated_;
    {
      DuckDBZSet clear;
      for (const auto &[row, w] : survivors)
        clear.insert(row, -1);
      step_view_->apply_changes(sentinel_, clear); // sentinel → empty (discard)
    }
    DuckDBZSet fwd;
    step_view_->apply_changes(sentinel_, survivors); // sentinel → survivors
    for (const auto &[row, w] : step_view_->get_delta()) {
      if (w > 0 && accumulated_.get(row) == 0) {
        accumulated_.insert(row, 1);
        output_.insert(row, 1);
        fwd.insert(row, 1);
      }
    }
    // Also admit directly-seeded insertions absent from accumulated_.
    for (const auto &[row, w] : insert_seed) {
      if (accumulated_.get(row) == 0) {
        accumulated_.insert(row, 1);
        output_.insert(row, 1);
        fwd.insert(row, 1);
      }
    }
    // Deeper rederivations: rederived rows are now part of the recursive
    // relation and may rederive further over-deleted rows.
    iter = 0;
    while (!fwd.empty() && iter++ < max_iterations_) {
      step_view_->apply_changes(sentinel_, fwd);
      DuckDBZSet next;
      for (const auto &[row, w] : step_view_->get_delta()) {
        if (w > 0 && accumulated_.get(row) == 0) {
          accumulated_.insert(row, 1);
          output_.insert(row, 1);
          next.insert(row, 1);
        }
      }
      fwd = std::move(next);
    }
  }
```

- [ ] **Step 3: Build and run the deletion suite — expect PASS**

```bash
cd test/build_test && make test_recursive_deletion 2>&1 | tail -5 && ./test_recursive_deletion
```
Expected: all sections PASS, including the randomized differential. If the randomized test fails, capture the seed round and reduce it to a targeted case before changing the algorithm (systematic-debugging).

- [ ] **Step 4: Run the full recursive suite (no regression on inserts / UNION ALL)**

```bash
cd test/build_test && make test_recursive recursive_integration 2>&1 | tail -3 && ./test_recursive && ./recursive_integration
```
Expected: PASS (insert-only path and UNION ALL recompute path untouched).

- [ ] **Step 5: Commit**

```bash
git add include/dbsp_plan_translator.hpp
git commit -m "feat: incremental recursive deletion via DRed (UNION recursion)"
```

---

## Task 3: Full suite, sanitizers, and documentation

**Files:**
- Modify: `TODO.md` (remove the "Deletions through recursive views trigger a full fixed-point recompute" limitation; note UNION ALL still recomputes).
- Modify: `include/dbsp_plan_translator.hpp` — the `PlanRecursiveNode` header comment (~line 2808-2819) to describe DRed.
- Modify: `docs/THEORY.md` if it documents recursive maintenance; otherwise skip.

- [ ] **Step 1: Run the whole test suite**

```bash
cd test/build_test && make 2>&1 | tail -3 && ctest --output-on-failure 2>&1 | tail -20
```
Expected: all tests pass.

- [ ] **Step 2: ASAN pass on the new suite**

```bash
cd test && cmake -S . -B build_asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SANITIZER=address -DENABLE_UBSAN=OFF >/dev/null 2>&1 && cmake --build build_asan --target recursive_deletion 2>&1 | tail -3 && ASAN_OPTIONS=detect_leaks=0 ./build_asan/recursive_deletion
```
Expected: all PASS, zero ASAN reports. (If the sanitizer flag name differs, check an existing `build_asan` invocation in the repo history; the memory notes reference `test/build_asan`.)

- [ ] **Step 3: Update the class header comment**

Replace the "Deltas containing a deletion fall back to a full fixed-point recompute..." paragraph (~lines 2814-2819) with:

```cpp
// Deltas containing a deletion take the DRed (Delete-Rederive) path for
// UNION (set-semantics) recursion: an incremental overdelete fixpoint
// over-approximates the retraction, then a rederive fixpoint re-admits rows
// that still have alternative support (cycles included). Overdelete is
// O(affected subgraph); rederive costs one image pass over the surviving
// relation — still cheaper than the full recompute it replaces. UNION ALL
// (multiplicity) recursion keeps recompute(): weighted deletion in a cycle
// is ill-defined. recompute() is retained for that path and as the
// differential-test oracle.
```

- [ ] **Step 4: Update TODO.md**

In the "Architectural" section, remove the bullet:
> - Deletions through recursive views trigger a full fixed-point recompute (correct but non-incremental).

Add under a "Performance" or "Not supported" note:
> - UNION ALL recursive deletion still triggers a full fixed-point recompute (multiplicity-in-cycles is ill-defined; UNION recursion is incremental via DRed).

- [ ] **Step 5: Commit**

```bash
git add TODO.md include/dbsp_plan_translator.hpp docs/
git commit -m "docs: DRed recursive deletion — update TODO, class comment, theory"
```

---

## Self-Review Notes

- **Spec coverage:** overdelete (Task 2 Step 2 loop 1) ✓; rederive (loop 2+3) ✓; UNION-only, UNION ALL recompute (Task 2 Step 1) ✓; property test DRed≡oracle (Task 1) ✓; targeted cases — kill-reachability, alternate-path, cycle, round-trip (Task 1) ✓; multi-table base deletion — covered by the join-with-`edges` step (edges is the base table, deletions hit it) ✓; `recompute()` retained ✓; docs (Task 3) ✓.
- **Oracle validity:** `kTcOracle` is a self-contained CTE on `edges`, independent of the incremental engine — a true external oracle.
- **Invariant maintenance:** every `accumulated_` mutation is mirrored to the sentinel source (retract loop feeds `neg`; rederive feeds `fwd`; the clear/re-present round-trip is weight-neutral on the sentinel). This keeps `accumulated_ == step_view_ sentinel arrangement`.
- **Linearity dependency:** the clear-then-re-present rederive trick assumes the step term is linear (no distinct/aggregate inside). SQL forbids aggregates/DISTINCT in a recursive term, so this holds; noted in Global Constraints.
- **Type consistency:** `dred(const DuckDBZSet&, const std::vector<std::pair<std::string, const DuckDBZSet*>>&)` matches the `base_deltas` local already built in `step()` (`std::vector<std::pair<std::string, const DuckDBZSet *>>`, lines 2844-2856) and `anchor_delta` (`const DuckDBZSet&`, line 2838). ✓
```

# Incremental Window-Function Maintenance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Maintain the SQL window shapes NumPad emits (LAG/LEAD/SHIFT, ROLLING, CUMSUM/YTD/QTD/MTD, FILLFORWARD) in O(affected rows) instead of the current O(partition) full re-render, emitting only changed output rows.

**Architecture:** `NativeWindowView::apply_changes` currently, per affected partition, retracts the whole partition output and re-renders every row (`include/dbsp_window_view.hpp:158–430`). We refactor rendering into a per-row helper backed by a per-index output cache, add a "fast-eligible" gate (all windows are offset / ROWS-frame-agg / fillforward — no RANK/RANGE/holistic), and for eligible partitions compute a small **affected-row set** per delta and re-emit only those rows. Ineligible partitions and any declined edge fall back to today's full re-render. Correctness is pinned by a differential test (incremental == full) and a throughput bench gates the O(affected) property in ctest.

**Tech Stack:** C++17, DuckDB static lib, Catch2 (`third_party/catch/catch.hpp`), CMake + ctest from `test/build_test`.

## Global Constraints

- **Bit-for-bit parity:** every fast path must produce the exact output Z-set the full-partition renderer produces (NULL handling, ROWS-frame boundaries, reset buckets). Verified by the differential test, not assumed.
- **Fallback is sacred:** anything not fast-pathed (RANK/DENSE_RANK/ROW_NUMBER/NTILE, holistic, RANGE/GROUPS frames, structural edges a path declines) uses the existing full re-render. Correctness never depends on the fast path.
- **Scope:** ROWS frames only; the four NumPad shapes. NumPad emits no RANGE/rank/holistic.
- **Discipline:** TDD, one behavior per commit. Full suite + ASAN + TSAN stay green (`test/build_test`, `test/build_asan`, `test/build_tsan`).
- **Build/test dir:** run ctest only from `test/build_test` (repo CLAUDE.md hygiene).

---

## File Structure

- **Modify** `include/dbsp_window_view.hpp` — refactor render into `render_row` + per-index cache; add fast-eligibility gate and per-shape affected-set logic.
- **Create** `test/unit/test_window_incremental.cpp` — differential (incremental == full) tests per shape + structural edits.
- **Create** `test/benchmarks/bench_window.cpp` — window-delta throughput (O(affected) not O(partition)).
- **Modify** `test/CMakeLists.txt` — register the new unit test and bench; add a `window_incremental` ctest and a `bench_window` executable; add `bench_window "[window_bench]"` to the ctest gate.
- **Modify** `docs/DESIGN_DATA_PLANE.md` — record incremental window maintenance as shipped (new section), once Phase 1 lands.

---

## Task 1: Differential harness + baseline invariant test

**Files:**
- Create: `test/unit/test_window_incremental.cpp`
- Modify: `test/CMakeLists.txt` (register `window_incremental`)

**Interfaces:**
- Consumes: `dbsp_native::NativeWindowView`, `WindowDef`, `DuckDBRow`, `DuckDBZSet`, `NativeSortView::SortColumn`, `TableSchema` (all in `include/dbsp_window_view.hpp` / `dbsp_duckdb_types.hpp`).
- Produces: helper `zset_equal(const DuckDBZSet&, const DuckDBZSet&)` and `apply_incremental_vs_full(windows, schema, deltas)` used by all later tasks.

- [ ] **Step 1: Write the failing test**

```cpp
// test/unit/test_window_incremental.cpp
#include "../test_helpers.hpp"
#include "catch.hpp"
#include "dbsp_window_view.hpp"

using namespace dbsp_native;
using duckdb::Value;

namespace {
// Z-set equality: same (row, weight) multiset.
bool zset_equal(const DuckDBZSet &a, const DuckDBZSet &b) {
  if (a.size() != b.size()) return false;
  for (const auto &[row, w] : a) {
    bool found = false;
    for (const auto &[row2, w2] : b)
      if (w == w2 && row == row2) { found = true; break; }
    if (!found) return false;
  }
  return true;
}

// One (part_col, order_col, arg_col) source row.
DuckDBRow src(int part, int ord, Value arg) {
  DuckDBRow r;
  r.columns = {Value::INTEGER(part), Value::INTEGER(ord), arg};
  return r;
}

// A LAG(arg,1) OVER (PARTITION BY col0 ORDER BY col1) window def.
std::vector<NativeWindowView::WindowDef> lag1_def() {
  NativeWindowView::WindowDef w;
  w.function = "LAG";
  w.partition_indices = {0};
  w.sort_columns = {{1, true, true}}; // col_idx=1, ascending, nulls_first
  w.arg_column_idx = 2;
  w.offset = 1;
  return {w};
}

TableSchema src_schema() {
  TableSchema s;
  s.table_name = "t";
  s.columns = {{"p", duckdb::LogicalType::INTEGER},
               {"o", duckdb::LogicalType::INTEGER},
               {"v", duckdb::LogicalType::INTEGER}};
  return s;
}

// Build a view, feed deltas one-at-a-time, return its result Z-set.
DuckDBZSet run_incremental(std::vector<NativeWindowView::WindowDef> w,
                           const std::vector<DuckDBZSet> &deltas) {
  NativeWindowView v("w_inc", "", "t", src_schema(), src_schema(), w);
  for (const auto &d : deltas) v.apply_changes("t", d);
  return v.get_result();
}

// Build a fresh view, feed the net accumulated state in ONE apply (full render).
DuckDBZSet run_full(std::vector<NativeWindowView::WindowDef> w,
                    const std::vector<DuckDBZSet> &deltas) {
  DuckDBZSet net;
  for (const auto &d : deltas)
    for (const auto &[row, wt] : d) net.insert(row, wt);
  NativeWindowView v("w_full", "", "t", src_schema(), src_schema(), w);
  v.apply_changes("t", net);
  return v.get_result();
}
} // namespace

TEST_CASE("window incremental == full: LAG baseline", "[window][incremental]") {
  std::vector<DuckDBZSet> deltas;
  DuckDBZSet d0; // initial partition of 5 rows
  for (int o = 0; o < 5; o++) d0.insert(src(1, o, Value::INTEGER(o * 10)), 1);
  deltas.push_back(d0);
  DuckDBZSet d1; // update ord=2's value: retract old, insert new
  d1.insert(src(1, 2, Value::INTEGER(20)), -1);
  d1.insert(src(1, 2, Value::INTEGER(999)), 1);
  deltas.push_back(d1);

  REQUIRE(zset_equal(run_incremental(lag1_def(), deltas),
                     run_full(lag1_def(), deltas)));
}
```

- [ ] **Step 2: Register the test and build**

Add to `test/CMakeLists.txt` after the other `add_dbsp_test(...)` lines (near line 70):

```cmake
add_dbsp_test(window_incremental unit/test_window_incremental.cpp)
```

Run: `cd test/build_test && cmake . >/dev/null && cmake --build . --target test_window_incremental 2>&1 | tail -2`
Expected: builds clean.

- [ ] **Step 3: Run the test**

Run: `cd test/build_test && ./test_window_incremental "[window][incremental]"`
Expected: **PASS** (current full-render is already correct for this case — this test documents the invariant that guards the refactor in Task 2).

- [ ] **Step 4: Commit**

```bash
git add test/unit/test_window_incremental.cpp test/CMakeLists.txt
git commit -m "test(window): differential harness (incremental == full) + LAG baseline"
```

---

## Task 2: Refactor to per-row render + output cache (behavior-preserving)

Splits the per-row rendering out of the mega-loop and replaces the per-partition output Z-set with a per-index cache, so later tasks can re-emit individual rows. Behavior is unchanged (still renders all rows); the Task 1 differential test must stay green.

**Files:**
- Modify: `include/dbsp_window_view.hpp:81` (change `partition_outputs_` type), `:158–430` (refactor loop)
- Test: `test/unit/test_window_incremental.cpp` (add a multi-window + structural-insert case)

**Interfaces:**
- Produces: `DuckDBRow render_row(const std::vector<DuckDBRow>& rows, size_t idx, const std::vector<size_t>& peer_start, const std::vector<size_t>& peer_end) const;` — renders one output row (source columns + all window columns) for `rows[idx]`. Extracted verbatim from the current per-row body (`:227–420`).
- Produces: `bool all_windows_fast_eligible() const;` — true iff every `WindowDef` is offset (LAG/LEAD), ROWS-frame aggregate (SUM/COUNT/AVG/MIN/MAX with start/end in {UNBOUNDED_PRECEDING, CURRENT_ROW_ROWS, EXPR_PRECEDING_ROWS, EXPR_FOLLOWING_ROWS}), or LAST_VALUE; false if any RANK/DENSE_RANK/ROW_NUMBER/NTILE/NTH_VALUE/FIRST_VALUE or any CURRENT_ROW_RANGE/CURRENT_ROW_GROUPS boundary appears. (Task 2 does not yet branch on it; it is defined here for Tasks 3–6.)

- [ ] **Step 1: Add a structural + multi-window differential test (still passes on full render)**

```cpp
TEST_CASE("window incremental == full: structural insert mid-partition",
          "[window][incremental]") {
  std::vector<DuckDBZSet> deltas;
  DuckDBZSet d0;
  for (int o = 0; o < 4; o++) d0.insert(src(1, o, Value::INTEGER(o)), 1);
  deltas.push_back(d0);
  DuckDBZSet d1; // insert a NEW ordinal in the middle (structural)
  d1.insert(src(1, 2 /*dup order slot but distinct value*/, Value::INTEGER(77)), 1);
  deltas.push_back(d1);
  REQUIRE(zset_equal(run_incremental(lag1_def(), deltas),
                     run_full(lag1_def(), deltas)));
}
```

- [ ] **Step 2: Run — expect PASS (documents behavior before refactor)**

Run: `cd test/build_test && cmake --build . --target test_window_incremental >/dev/null && ./test_window_incremental "[window][incremental]"`
Expected: PASS.

- [ ] **Step 3: Change `partition_outputs_` to a per-index cache**

In `include/dbsp_window_view.hpp:81`, replace:
```cpp
  std::map<PartitionKey, DuckDBZSet> partition_outputs_;
```
with:
```cpp
  // Rendered output row per partition, in ordered-row order (index-aligned
  // with the sorted partition). Enables per-row retract/re-emit.
  std::map<PartitionKey, std::vector<DuckDBRow>> partition_outputs_;
```

- [ ] **Step 4: Extract `render_row` and `all_windows_fast_eligible`**

Add as private methods (before `public:` at `:88`). Move the body of the current per-row loop (`:227–420`, everything that builds `out_row`) into `render_row`, taking `row_idx`, `partition_rows`, `peer_start`, `peer_end`. Compute `row_number`/`rank`/`dense_rank` inside from `row_idx` and `peer_start` (rank = `peer_start[row_idx] + 1`; dense_rank requires the sweep — for Task 2 keep the sweep in the caller and pass rank/dense_rank in, OR restrict `render_row` to the non-ranking columns and keep rank handling in the full path only; since ranking functions are fallback-only per the eligibility gate, `render_row` may assume no rank window when used by fast paths). Concretely:

```cpp
  // Render the full output row (source cols + each window col) for one index.
  // rank/dense_rank use peer_start; only meaningful on the full path.
  DuckDBRow render_row(const std::vector<DuckDBRow> &rows, size_t row_idx,
                       const std::vector<size_t> &peer_start,
                       const std::vector<size_t> &peer_end) const {
    DuckDBRow out_row = rows[row_idx];
    const int64_t row_number = (int64_t)row_idx + 1;
    const int64_t rank = (int64_t)peer_start[row_idx] + 1;
    // dense_rank: count distinct peer-groups up to row_idx
    int64_t dense_rank = 1;
    for (size_t j = 1; j <= row_idx; j++)
      if (peer_start[j] == j) dense_rank++;
    for (const auto &win : windows_) {
      // ... the exact per-function body currently at :244–419, using
      // `rows` for `partition_rows`, `row_idx`, `row_number`, `rank`,
      // `dense_rank`. Copy it verbatim; do not change semantics.
    }
    return out_row;
  }

  bool all_windows_fast_eligible() const {
    for (const auto &w : windows_) {
      const bool offset = (w.function == "LAG" || w.function == "LEAD");
      const bool fill = (w.function == "LAST_VALUE");
      const bool agg = (w.function == "SUM" || w.function == "COUNT" ||
                        w.function == "AVG" || w.function == "MIN" ||
                        w.function == "MAX");
      const bool rows_frame =
          (w.start == duckdb::WindowBoundary::UNBOUNDED_PRECEDING ||
           w.start == duckdb::WindowBoundary::CURRENT_ROW_ROWS ||
           w.start == duckdb::WindowBoundary::EXPR_PRECEDING_ROWS ||
           w.start == duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS) &&
          (w.end == duckdb::WindowBoundary::UNBOUNDED_PRECEDING ||
           w.end == duckdb::WindowBoundary::UNBOUNDED_FOLLOWING ||
           w.end == duckdb::WindowBoundary::CURRENT_ROW_ROWS ||
           w.end == duckdb::WindowBoundary::EXPR_PRECEDING_ROWS ||
           w.end == duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS);
      if (!(offset || fill || (agg && rows_frame))) return false;
    }
    return true;
  }
```

- [ ] **Step 5: Rewrite the render section (`:158–430`) to use the cache + helper**

Replace the affected-partition loop so that, per affected partition, it: (a) retracts each cached output row (`for (auto& r : partition_outputs_[key]) { delta_.insert(r,-1); result_.insert(r,-1); }`), (b) rebuilds `partition_rows` + `peer_start`/`peer_end` (keep the existing peer code `:187–219`), (c) clears the cache vector, (d) loops every index calling `render_row`, inserting into `delta_`/`result_` and `partition_outputs_[key].push_back(out_row)`. This preserves behavior (renders all rows) while routing through the new structures.

- [ ] **Step 6: Build + run the whole differential + existing window suites**

Run: `cd test/build_test && cmake --build . --target test_window_incremental test_window_functions >/dev/null && ./test_window_incremental && ./test_window_functions`
Expected: all PASS (behavior unchanged).

- [ ] **Step 7: Commit**

```bash
git add include/dbsp_window_view.hpp test/unit/test_window_incremental.cpp
git commit -m "refactor(window): per-row render + per-index output cache (no behavior change)"
```

---

## Task 3: Offset fast path (LAG / LEAD / SHIFT)

**Files:**
- Modify: `include/dbsp_window_view.hpp` (`apply_changes` fast branch)
- Test: `test/unit/test_window_incremental.cpp`

**Interfaces:**
- Consumes: `render_row`, `all_windows_fast_eligible`, per-index `partition_outputs_`.
- Produces: private `void emit_affected(const PartitionKey& key, const std::vector<size_t>& affected);` — for each index in `affected` (sorted, unique), retract `partition_outputs_[key][idx]`, render new via `render_row`, insert new, update the cache slot. Used by Tasks 3–6.

- [ ] **Step 1: Write failing tests (value update + structural insert, single & multi-offset)**

```cpp
TEST_CASE("window fast LAG/LEAD == full", "[window][incremental]") {
  auto defs = []{
    NativeWindowView::WindowDef lag; lag.function="LAG"; lag.partition_indices={0};
    lag.sort_columns={{1,true,true}}; lag.arg_column_idx=2; lag.offset=2;
    NativeWindowView::WindowDef lead; lead.function="LEAD"; lead.partition_indices={0};
    lead.sort_columns={{1,true,true}}; lead.arg_column_idx=2; lead.offset=1;
    return std::vector<NativeWindowView::WindowDef>{lag, lead};
  }();
  std::vector<DuckDBZSet> deltas;
  DuckDBZSet d0;
  for (int o=0;o<8;o++) d0.insert(src(1,o,Value::INTEGER(o)),1);
  deltas.push_back(d0);
  DuckDBZSet upd; // value update mid-partition
  upd.insert(src(1,4,Value::INTEGER(4)),-1);
  upd.insert(src(1,4,Value::INTEGER(400)),1);
  deltas.push_back(upd);
  REQUIRE(zset_equal(run_incremental(defs,deltas), run_full(defs,deltas)));
}
```

- [ ] **Step 2: Run — expect FAIL only if fast path is wrong; PASS now (still full render)**

Run: `cd test/build_test && cmake --build . --target test_window_incremental >/dev/null && ./test_window_incremental "[window][incremental]"`
Expected: PASS (Task 2 still renders all — this test locks the target before the fast branch exists). Keep it; it becomes the guard once the branch lands.

- [ ] **Step 3: Add the fast branch to `apply_changes`**

After updating the multiset for a partition, when `all_windows_fast_eligible()` is true and no structural size change forces a full pass, compute affected indices. Add `emit_affected` and, for offset windows, the affected set for a value update at ordered index `p` is `{p}` (the row itself, whose source cols changed) plus each `p + offset` (LEAD readers) and `p - offset` (LAG readers) within bounds, across all offset windows. For a structural insert/delete at `p`, affected = `[p - maxOffset, p + maxOffset]` clamped, plus `p`. Implement by locating each changed row's ordered index in `partition_rows` (binary search via the comparator), unioning the offset neighborhoods, and calling `emit_affected`. Structural size change (insert/delete) also shifts positions for offset readers within `maxOffset` — the neighborhood clamp covers it.

```cpp
  void emit_affected(const PartitionKey &key,
                     const std::vector<DuckDBRow> &rows,
                     const std::vector<size_t> &peer_start,
                     const std::vector<size_t> &peer_end,
                     std::vector<size_t> affected) {
    std::sort(affected.begin(), affected.end());
    affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
    auto &cache = partition_outputs_[key];
    for (size_t idx : affected) {
      if (idx >= rows.size()) continue;
      // retract old cached row
      delta_.insert(cache[idx], -1); result_.insert(cache[idx], -1);
      DuckDBRow nr = render_row(rows, idx, peer_start, peer_end);
      delta_.insert(nr, 1); result_.insert(nr, 1);
      cache[idx] = nr;
    }
  }
```

Guard: the fast branch only runs when partition size is unchanged (pure value update) OR the size change is handled by rebuilding the cache to the new length and mapping affected indices; if that mapping is ambiguous, fall back to the full pass for that partition (always correct).

- [ ] **Step 4: Run — expect PASS**

Run: `cd test/build_test && ./test_window_incremental "[window][incremental]"`
Expected: PASS (fast path matches full).

- [ ] **Step 5: Commit**

```bash
git add include/dbsp_window_view.hpp test/unit/test_window_incremental.cpp
git commit -m "feat(window): O(1) offset fast path (LAG/LEAD/SHIFT) with full fallback"
```

---

## Task 4: Rolling bounded-frame fast path (ROWS n PRECEDING)

**Files:** Modify `include/dbsp_window_view.hpp`; Test `test/unit/test_window_incremental.cpp`

**Interfaces:** Consumes `emit_affected`, `render_row`. No new public interface.

- [ ] **Step 1: Failing test — ROLLING_SUM/AVG over ROWS n-1 PRECEDING**

```cpp
TEST_CASE("window fast ROLLING == full", "[window][incremental]") {
  NativeWindowView::WindowDef w; w.function="SUM"; w.partition_indices={0};
  w.sort_columns={{1,true,true}}; w.arg_column_idx=2;
  w.start=duckdb::WindowBoundary::EXPR_PRECEDING_ROWS; w.start_offset=2; // 3-row frame
  w.end=duckdb::WindowBoundary::CURRENT_ROW_ROWS;
  std::vector<NativeWindowView::WindowDef> defs{w};
  std::vector<DuckDBZSet> deltas;
  DuckDBZSet d0; for(int o=0;o<10;o++) d0.insert(src(1,o,Value::INTEGER(o)),1);
  deltas.push_back(d0);
  DuckDBZSet upd; upd.insert(src(1,5,Value::INTEGER(5)),-1);
  upd.insert(src(1,5,Value::INTEGER(500)),1); deltas.push_back(upd);
  REQUIRE(zset_equal(run_incremental(defs,deltas), run_full(defs,deltas)));
}
```

- [ ] **Step 2: Run — expect PASS via full render (locks target)**

Run: `cd test/build_test && cmake --build . --target test_window_incremental >/dev/null && ./test_window_incremental "[window][incremental]"`
Expected: PASS.

- [ ] **Step 3: Add rolling affected-set**

For a ROWS aggregate window with `start_offset = s` (rows preceding) and end at CURRENT_ROW, a value update at ordered index `p` affects every row whose frame includes `p`: indices `[p, p + s]` clamped to `[0, size-1]` (rows at `p..p+s` have `p` within their `s`-preceding frame). For `EXPR_FOLLOWING_ROWS` end offset `e`, extend to `[p - e, p + s]`. Union across all rolling windows using the per-window `start_offset`/`end_offset`, then `emit_affected`. (Full re-eval of each affected row via `render_row` is O(frame) per row × O(frame) rows = O(frame^2) worst case but frame is small and constant; acceptable — no running state needed for Phase 1.)

- [ ] **Step 4: Run — expect PASS**

Run: `cd test/build_test && ./test_window_incremental "[window][incremental]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/dbsp_window_view.hpp test/unit/test_window_incremental.cpp
git commit -m "feat(window): O(frame) rolling bounded-frame fast path"
```

---

## Task 5: Running-sum fast path (CUMSUM / YTD / QTD / MTD)

Running sums are `UNBOUNDED_PRECEDING .. CURRENT_ROW_ROWS` (partition = the reset bucket, so YTD/QTD/MTD partition on the fiscal bucket column and reduce to CUMSUM within it).

**Files:** Modify `include/dbsp_window_view.hpp`; Test `test/unit/test_window_incremental.cpp`

**Interfaces:** Consumes `emit_affected`, `render_row`.

- [ ] **Step 1: Failing test — SUM OVER (UNBOUNDED PRECEDING .. CURRENT ROW)**

```cpp
TEST_CASE("window fast CUMSUM == full", "[window][incremental]") {
  NativeWindowView::WindowDef w; w.function="SUM"; w.partition_indices={0};
  w.sort_columns={{1,true,true}}; w.arg_column_idx=2;
  w.start=duckdb::WindowBoundary::UNBOUNDED_PRECEDING;
  w.end=duckdb::WindowBoundary::CURRENT_ROW_ROWS;
  std::vector<NativeWindowView::WindowDef> defs{w};
  std::vector<DuckDBZSet> deltas;
  DuckDBZSet d0; for(int o=0;o<10;o++) d0.insert(src(1,o,Value::INTEGER(o)),1);
  deltas.push_back(d0);
  DuckDBZSet upd; upd.insert(src(1,3,Value::INTEGER(3)),-1);
  upd.insert(src(1,3,Value::INTEGER(300)),1); deltas.push_back(upd);
  REQUIRE(zset_equal(run_incremental(defs,deltas), run_full(defs,deltas)));
}
```

- [ ] **Step 2: Run — expect PASS via full render**

Run: `cd test/build_test && cmake --build . --target test_window_incremental >/dev/null && ./test_window_incremental "[window][incremental]"`
Expected: PASS.

- [ ] **Step 3: Add running-sum affected-set**

For an UNBOUNDED_PRECEDING..CURRENT_ROW aggregate, a value update at index `p` affects the suffix `[p, size-1]`. Call `emit_affected` with `affected = {p, p+1, ..., size-1}`. Per-row re-eval via `render_row` for a running sum is O(row_idx) inside the frame loop, making the suffix O(suffix^2) — replace with an incremental accumulator: compute the frame value at `p` once from `partition_outputs_[key][p-1]`'s cached window column (the prior running total), then for each `i` in the suffix set `running += rows[i].arg` and emit; this makes the suffix O(suffix). Implement a specialized suffix emitter for pure UNBOUNDED_PRECEDING..CURRENT SUM/COUNT/AVG that reads the previous cached running value and walks forward, instead of calling the generic `render_row` (which rescans the frame). MIN/MAX with unbounded-preceding are monotone only for inserts; on a value decrease they still need a suffix rescan — for MIN/MAX running windows, fall back to `render_row` per suffix row (still O(suffix) rows, O(frame) each — acceptable and correct).

- [ ] **Step 4: Run — expect PASS**

Run: `cd test/build_test && ./test_window_incremental "[window][incremental]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/dbsp_window_view.hpp test/unit/test_window_incremental.cpp
git commit -m "feat(window): O(suffix) running-sum fast path (CUMSUM/YTD/QTD/MTD)"
```

---

## Task 6: Fillforward fast path (LAST_VALUE IGNORE NULLS) — also closes a correctness gap

**Note:** the current renderer's `LAST_VALUE` (`:275–295`) takes the frame-end value directly and does **not** implement `IGNORE NULLS`, which NumPad's FILLFORWARD requires. So the differential baseline is itself wrong for null-bearing input. This task fixes both the full renderer and adds the fast path, validated against DuckDB's actual semantics.

**Files:** Modify `include/dbsp_window_view.hpp`; Test `test/unit/test_window_incremental.cpp`

**Interfaces:** Consumes `emit_affected`, `render_row`.

- [ ] **Step 1: Failing test — LAST_VALUE IGNORE NULLS carries last non-null forward**

```cpp
TEST_CASE("window fillforward (LAST_VALUE ignore nulls) correctness",
          "[window][incremental]") {
  NativeWindowView::WindowDef w; w.function="LAST_VALUE"; w.partition_indices={0};
  w.sort_columns={{1,true,true}}; w.arg_column_idx=2;
  w.start=duckdb::WindowBoundary::UNBOUNDED_PRECEDING;
  w.end=duckdb::WindowBoundary::CURRENT_ROW_ROWS;
  std::vector<NativeWindowView::WindowDef> defs{w};
  DuckDBZSet d0;
  d0.insert(src(1,0,Value::INTEGER(5)),1);
  d0.insert(src(1,1,Value(duckdb::LogicalType::INTEGER)),1); // NULL
  d0.insert(src(1,2,Value(duckdb::LogicalType::INTEGER)),1); // NULL
  d0.insert(src(1,3,Value::INTEGER(9)),1);
  NativeWindowView v("ff","","t",src_schema(),src_schema(),defs);
  v.apply_changes("t", d0);
  // Expect fillforward: 5,5,5,9 — assert the window column of ord=2 is 5.
  bool found=false;
  for (const auto &[row,wt] : v.get_result())
    if (wt>0 && row.columns[1].GetValue<int32_t>()==2)
      { REQUIRE(row.columns.back().GetValue<int32_t>()==5); found=true; }
  REQUIRE(found);
}
```

- [ ] **Step 2: Run — expect FAIL (current LAST_VALUE ignores nulls incorrectly)**

Run: `cd test/build_test && cmake --build . --target test_window_incremental >/dev/null && ./test_window_incremental "[window][incremental]"`
Expected: FAIL — current code returns NULL at ord=2, not 5.

- [ ] **Step 3: Fix LAST_VALUE ignore-nulls in `render_row`, add fast affected-set**

In `render_row`'s LAST_VALUE branch, scan backward from the frame end to the first non-null arg value (that is fillforward: last non-null up to current row). For the fast path, a value update at index `p` affects the forward run `[p, q]` where `q` is the index just before the next non-null after `p` (rows that were carrying `p`'s value forward, or that now change because `p`'s null/non-null status changed). Compute the run by walking forward from `p` until a non-null arg is hit; `emit_affected` over `[p, q]`. Because fillforward reads backward, changing `p` can only affect rows from `p` up to the next non-null; the walk is O(run).

- [ ] **Step 4: Run — expect PASS + differential green**

Run: `cd test/build_test && ./test_window_incremental`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add include/dbsp_window_view.hpp test/unit/test_window_incremental.cpp
git commit -m "feat(window): fillforward fast path + fix LAST_VALUE IGNORE NULLS"
```

---

## Task 7: Throughput bench + ctest gate + docs

**Files:**
- Create: `test/benchmarks/bench_window.cpp`
- Modify: `test/CMakeLists.txt` (add `bench_window` executable + `window_bench` ctest)
- Modify: `docs/DESIGN_DATA_PLANE.md`

**Interfaces:** none (bench + docs).

- [ ] **Step 1: Write the throughput bench (asserts O(affected), not O(partition))**

```cpp
// test/benchmarks/bench_window.cpp
#include "../test_helpers.hpp"
#include "catch.hpp"
#include "dbsp_window_view.hpp"
#include <chrono>
using namespace dbsp_native; using duckdb::Value;
using namespace std::chrono;

static double us_single_update(int partition_size) {
  NativeWindowView::WindowDef w; w.function="LAG"; w.partition_indices={0};
  w.sort_columns={{1,true,true}}; w.arg_column_idx=2; w.offset=1;
  NativeWindowView v("b","","t",{}, {}, {w});
  DuckDBZSet init;
  for (int o=0;o<partition_size;o++)
    init.insert([&]{DuckDBRow r; r.columns={Value::INTEGER(1),Value::INTEGER(o),
      Value::INTEGER(o)}; return r;}(),1);
  v.apply_changes("t", init);
  DuckDBRow oldr; oldr.columns={Value::INTEGER(1),Value::INTEGER(partition_size/2),
    Value::INTEGER(partition_size/2)};
  DuckDBRow newr; newr.columns={Value::INTEGER(1),Value::INTEGER(partition_size/2),
    Value::INTEGER(-1)};
  DuckDBZSet upd; upd.insert(oldr,-1); upd.insert(newr,1);
  auto t0=high_resolution_clock::now();
  v.apply_changes("t", upd);
  return duration_cast<nanoseconds>(high_resolution_clock::now()-t0).count()/1000.0;
}

TEST_CASE("bench: window single-row update is O(affected), not O(partition)",
          "[window_bench]") {
  double small = us_single_update(1000);
  double big   = us_single_update(100000);
  std::cout << "[bench] window LAG single update: 1k="<<small
            <<"us 100k="<<big<<"us ratio="<<(big/small)<<"\n";
  // O(partition) would be ~100x; O(affected) stays near-flat. Allow 10x slack.
  REQUIRE(big < small * 10.0);
}
```

- [ ] **Step 2: Register bench + gate in `test/CMakeLists.txt`**

After the other benchmark `add_executable` blocks (near line 137):
```cmake
add_executable(bench_window
    benchmarks/bench_window.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/dbsp_extension.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/dbsp_recovery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/dbsp_crash_marker.cpp)
target_link_libraries(bench_window catch2_main duckdb_static
                      duckdb_generated_extension_loader
                      core_functions_extension parquet_extension)
add_test(NAME window_bench COMMAND bench_window "[window_bench]")
```

- [ ] **Step 3: Build + run the gate**

Run: `cd test/build_test && cmake . >/dev/null && cmake --build . --target bench_window test_window_incremental >/dev/null && ctest -R "window_bench|window_incremental" --output-on-failure`
Expected: both PASS; bench prints a ratio well under 10x (near-flat).

- [ ] **Step 4: Full regression (suite + sanitizers)**

Run: `cd test/build_test && ctest --output-on-failure 2>&1 | tail -15`
Expected: 100% pass. (Then rebuild `test/build_asan` and `test/build_tsan` and run their window tests; expected clean.)

- [ ] **Step 5: Document in `docs/DESIGN_DATA_PLANE.md`**

Add a section: "Incremental window maintenance (SHIPPED): NativeWindowView emits only affected rows per delta — offset O(1), rolling O(frame), running-sum O(suffix), fillforward O(run) — full-partition renderer kept as fallback for RANK/RANGE/holistic. Differential (incremental==full) test + window_bench gate. Closes the O(partition)-per-edit exposure for NumPad's time-intelligence SQL." Cross-reference this plan and the spec.

- [ ] **Step 6: Commit**

```bash
git add test/benchmarks/bench_window.cpp test/CMakeLists.txt docs/DESIGN_DATA_PLANE.md
git commit -m "test+docs(window): O(affected) throughput gate + data-plane doc"
```

---

## Self-Review

**Spec coverage:** offset (Task 3), rolling (Task 4), running-sum (Task 5), fillforward (Task 6) — all four spec shapes covered. Fallback (Task 2 eligibility gate + all fast branches guard-and-fall-back). Differential test (Tasks 1–6). Throughput gate (Task 7). Bit-for-bit parity (differential per shape). All spec sections map to tasks.

**Placeholder scan:** `render_row` Step (Task 2 Step 4) says "copy the per-function body verbatim" rather than reprinting ~180 lines already in the file at `:244–419` — this is a deliberate move-in-place of existing code, not a placeholder; the exact source range is cited. All other steps contain concrete code.

**Type consistency:** `WindowDef` fields (`function`, `partition_indices`, `sort_columns`, `arg_column_idx`, `offset`, `start`/`end`, `start_offset`/`end_offset`), `partition_outputs_` (now `map<PartitionKey, vector<DuckDBRow>>`), `render_row`/`emit_affected`/`all_windows_fast_eligible` signatures are consistent across Tasks 2–7. `NativeSortView::SortColumn{column_idx, ascending, nulls_first}` used consistently in test defs.

**Known risk flagged:** Task 6 fixes a pre-existing LAST_VALUE IGNORE NULLS bug; its differential baseline uses an explicit expected value (not self-comparison) because the full renderer was also wrong.

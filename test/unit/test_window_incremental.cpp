// Differential tests: incremental window maintenance must equal a full
// from-scratch render. Feeds a view deltas one-at-a-time (incremental path)
// and compares to a fresh view fed the net accumulated state in one apply
// (full-render path). See docs/superpowers/specs/2026-07-18-incremental-
// window-maintenance-design.md.
#include "../test_helpers.hpp"
#include "catch.hpp"
#include "dbsp_window_view.hpp"

using namespace dbsp_native;
using duckdb::Value;

namespace {

// Z-set equality: same (row, weight) multiset.
bool zset_equal(const DuckDBZSet &a, const DuckDBZSet &b) {
  if (a.size() != b.size())
    return false;
  for (const auto &[row, w] : a) {
    bool found = false;
    for (const auto &[row2, w2] : b)
      if (w == w2 && row == row2) {
        found = true;
        break;
      }
    if (!found)
      return false;
  }
  return true;
}

// One (part_col, order_col, arg_col) source row.
DuckDBRow src(int part, int ord, Value arg) {
  DuckDBRow r;
  r.columns = {Value::INTEGER(part), Value::INTEGER(ord), arg};
  return r;
}

// LAG(arg,1) OVER (PARTITION BY col0 ORDER BY col1).
std::vector<NativeWindowView::WindowDef> lag1_def() {
  NativeWindowView::WindowDef w;
  w.function = "LAG";
  w.partition_indices = {0};
  w.sort_columns = {{1, true, true}}; // column_idx=1, ascending, nulls_first
  w.arg_column_idx = 2;
  w.offset = 1;
  return {w};
}

// Build a view, feed deltas one-at-a-time, return its result Z-set.
DuckDBZSet run_incremental(std::vector<NativeWindowView::WindowDef> w,
                           const std::vector<DuckDBZSet> &deltas) {
  NativeWindowView v("w_inc", "", "t", TableSchema{}, TableSchema{}, w);
  for (const auto &d : deltas)
    v.apply_changes("t", d);
  return v.get_result();
}

// Build a fresh view, feed the net accumulated state in ONE apply.
DuckDBZSet run_full(std::vector<NativeWindowView::WindowDef> w,
                    const std::vector<DuckDBZSet> &deltas) {
  DuckDBZSet net;
  for (const auto &d : deltas)
    for (const auto &[row, wt] : d)
      net.insert(row, wt);
  NativeWindowView v("w_full", "", "t", TableSchema{}, TableSchema{}, w);
  v.apply_changes("t", net);
  return v.get_result();
}

} // namespace

TEST_CASE("window incremental == full: LAG baseline", "[window][incremental]") {
  std::vector<DuckDBZSet> deltas;
  DuckDBZSet d0; // initial partition of 5 rows
  for (int o = 0; o < 5; o++)
    d0.insert(src(1, o, Value::INTEGER(o * 10)), 1);
  deltas.push_back(d0);
  DuckDBZSet d1; // update ord=2's value: retract old, insert new
  d1.insert(src(1, 2, Value::INTEGER(20)), -1);
  d1.insert(src(1, 2, Value::INTEGER(999)), 1);
  deltas.push_back(d1);

  REQUIRE(zset_equal(run_incremental(lag1_def(), deltas),
                     run_full(lag1_def(), deltas)));
}

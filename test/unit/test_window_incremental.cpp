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

TEST_CASE("window fast LAG/LEAD value update == full", "[window][incremental]") {
  std::vector<NativeWindowView::WindowDef> defs;
  {
    NativeWindowView::WindowDef lag;
    lag.function = "LAG";
    lag.partition_indices = {0};
    lag.sort_columns = {{1, true, true}};
    lag.arg_column_idx = 2;
    lag.offset = 2;
    NativeWindowView::WindowDef lead;
    lead.function = "LEAD";
    lead.partition_indices = {0};
    lead.sort_columns = {{1, true, true}};
    lead.arg_column_idx = 2;
    lead.offset = 1;
    defs = {lag, lead};
  }
  std::vector<DuckDBZSet> deltas;
  DuckDBZSet d0;
  for (int o = 0; o < 8; o++)
    d0.insert(src(1, o, Value::INTEGER(o)), 1);
  deltas.push_back(d0);
  DuckDBZSet upd; // value update mid-partition (size unchanged -> fast path)
  upd.insert(src(1, 4, Value::INTEGER(4)), -1);
  upd.insert(src(1, 4, Value::INTEGER(400)), 1);
  deltas.push_back(upd);
  REQUIRE(zset_equal(run_incremental(defs, deltas), run_full(defs, deltas)));
}

TEST_CASE("window incremental == full: structural insert (fallback)",
          "[window][incremental]") {
  std::vector<DuckDBZSet> deltas;
  DuckDBZSet d0;
  for (int o = 0; o < 5; o++)
    d0.insert(src(1, o, Value::INTEGER(o)), 1);
  deltas.push_back(d0);
  DuckDBZSet ins; // new ordinal -> size changes -> full re-render path
  ins.insert(src(1, 5, Value::INTEGER(50)), 1);
  deltas.push_back(ins);
  REQUIRE(zset_equal(run_incremental(lag1_def(), deltas),
                     run_full(lag1_def(), deltas)));
}

TEST_CASE("window fast ROLLING (ROWS 2 preceding) == full",
          "[window][incremental]") {
  NativeWindowView::WindowDef w;
  w.function = "SUM";
  w.partition_indices = {0};
  w.sort_columns = {{1, true, true}};
  w.arg_column_idx = 2;
  w.start = duckdb::WindowBoundary::EXPR_PRECEDING_ROWS;
  w.start_offset = 2; // 3-row trailing frame
  w.end = duckdb::WindowBoundary::CURRENT_ROW_ROWS;
  std::vector<NativeWindowView::WindowDef> defs{w};
  std::vector<DuckDBZSet> deltas;
  DuckDBZSet d0;
  for (int o = 0; o < 10; o++)
    d0.insert(src(1, o, Value::INTEGER(o)), 1);
  deltas.push_back(d0);
  DuckDBZSet upd;
  upd.insert(src(1, 5, Value::INTEGER(5)), -1);
  upd.insert(src(1, 5, Value::INTEGER(500)), 1);
  deltas.push_back(upd);
  REQUIRE(zset_equal(run_incremental(defs, deltas), run_full(defs, deltas)));
}

TEST_CASE("window fast CUMSUM (unbounded preceding) == full",
          "[window][incremental]") {
  NativeWindowView::WindowDef w;
  w.function = "SUM";
  w.partition_indices = {0};
  w.sort_columns = {{1, true, true}};
  w.arg_column_idx = 2;
  w.start = duckdb::WindowBoundary::UNBOUNDED_PRECEDING;
  w.end = duckdb::WindowBoundary::CURRENT_ROW_ROWS;
  std::vector<NativeWindowView::WindowDef> defs{w};
  std::vector<DuckDBZSet> deltas;
  DuckDBZSet d0;
  for (int o = 0; o < 10; o++)
    d0.insert(src(1, o, Value::INTEGER(o)), 1);
  deltas.push_back(d0);
  DuckDBZSet upd; // edit at o=3 -> suffix [3..9] re-emits
  upd.insert(src(1, 3, Value::INTEGER(3)), -1);
  upd.insert(src(1, 3, Value::INTEGER(300)), 1);
  deltas.push_back(upd);
  REQUIRE(zset_equal(run_incremental(defs, deltas), run_full(defs, deltas)));
}

TEST_CASE("window fillforward (LAST_VALUE IGNORE NULLS) correctness",
          "[window][incremental]") {
  NativeWindowView::WindowDef w;
  w.function = "LAST_VALUE";
  w.partition_indices = {0};
  w.sort_columns = {{1, true, true}};
  w.arg_column_idx = 2;
  w.start = duckdb::WindowBoundary::UNBOUNDED_PRECEDING;
  w.end = duckdb::WindowBoundary::CURRENT_ROW_ROWS;
  std::vector<NativeWindowView::WindowDef> defs{w};
  DuckDBZSet d0; // values: 5, NULL, NULL, 9 -> fillforward 5,5,5,9
  d0.insert(src(1, 0, Value::INTEGER(5)), 1);
  d0.insert(src(1, 1, Value(duckdb::LogicalType::INTEGER)), 1);
  d0.insert(src(1, 2, Value(duckdb::LogicalType::INTEGER)), 1);
  d0.insert(src(1, 3, Value::INTEGER(9)), 1);
  NativeWindowView v("ff", "", "t", TableSchema{}, TableSchema{}, defs);
  v.apply_changes("t", d0);
  bool found = false; // ord=2 must carry forward the last non-null (5)
  for (const auto &[row, wt] : v.get_result())
    if (wt > 0 && row.columns[1].GetValue<int32_t>() == 2) {
      REQUIRE(row.columns.back().GetValue<int32_t>() == 5);
      found = true;
    }
  REQUIRE(found);
}

TEST_CASE("window fast fillforward value update == full",
          "[window][incremental]") {
  NativeWindowView::WindowDef w;
  w.function = "LAST_VALUE";
  w.partition_indices = {0};
  w.sort_columns = {{1, true, true}};
  w.arg_column_idx = 2;
  w.start = duckdb::WindowBoundary::UNBOUNDED_PRECEDING;
  w.end = duckdb::WindowBoundary::CURRENT_ROW_ROWS;
  std::vector<NativeWindowView::WindowDef> defs{w};
  std::vector<DuckDBZSet> deltas;
  DuckDBZSet d0; // 5, NULL, 7, NULL, 9
  d0.insert(src(1, 0, Value::INTEGER(5)), 1);
  d0.insert(src(1, 1, Value(duckdb::LogicalType::INTEGER)), 1);
  d0.insert(src(1, 2, Value::INTEGER(7)), 1);
  d0.insert(src(1, 3, Value(duckdb::LogicalType::INTEGER)), 1);
  d0.insert(src(1, 4, Value::INTEGER(9)), 1);
  deltas.push_back(d0);
  DuckDBZSet upd; // ord=2: 7 -> NULL (extends the run carrying 5 forward)
  upd.insert(src(1, 2, Value::INTEGER(7)), -1);
  upd.insert(src(1, 2, Value(duckdb::LogicalType::INTEGER)), 1);
  deltas.push_back(upd);
  REQUIRE(zset_equal(run_incremental(defs, deltas), run_full(defs, deltas)));
}

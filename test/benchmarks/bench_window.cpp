// Window-delta throughput: a single-row update must cost O(affected rows),
// not O(partition). With the incremental fast path a value update touches a
// couple of rows regardless of partition size; the old full-partition
// re-render was linear in the partition. Gate: 100x-larger partition must not
// be ~100x slower.
#include "../test_helpers.hpp"
#include "catch.hpp"
#include "dbsp_window_view.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

using namespace dbsp_native;
using duckdb::Value;
using namespace std::chrono;

namespace {
DuckDBRow row3(int p, int o, int v) {
  DuckDBRow r;
  r.columns = {Value::INTEGER(p), Value::INTEGER(o), Value::INTEGER(v)};
  return r;
}

// Median per-update cost (us) for a value update at the partition middle.
// Warms up, then times many real in-place updates so tiny per-update times
// aren't swamped by measurement noise.
double us_single_update(int partition_size) {
  NativeWindowView::WindowDef w;
  w.function = "LAG";
  w.partition_indices = {0};
  w.sort_columns = {{1, true, true}};
  w.arg_column_idx = 2;
  w.offset = 1;
  NativeWindowView v("b", "", "t", TableSchema{}, TableSchema{}, {w});

  DuckDBZSet init; // one partition, partition_size ordered rows
  for (int o = 0; o < partition_size; o++)
    init.insert(row3(1, o, o), 1);
  v.apply_changes("t", init); // full render (setup, untimed)

  const int mid = partition_size / 2;
  int cur = mid; // current value at ordinal `mid`
  auto do_update = [&](int next) {
    DuckDBZSet upd; // same sort key -> in-place overwrite -> fast path
    upd.insert(row3(1, mid, cur), -1);
    upd.insert(row3(1, mid, next), 1);
    v.apply_changes("t", upd);
    cur = next;
  };

  do_update(-1); // warmup
  const int N = 300;
  std::vector<double> times;
  times.reserve(N);
  for (int i = 0; i < N; i++) {
    int next = (i & 1) ? -2 : -3;
    auto t0 = high_resolution_clock::now();
    do_update(next);
    times.push_back(
        duration_cast<nanoseconds>(high_resolution_clock::now() - t0).count() /
        1000.0);
  }
  std::sort(times.begin(), times.end());
  return times[times.size() / 2]; // median
}
} // namespace

TEST_CASE("bench: window single-row update is O(affected), not O(partition)",
          "[window_bench]") {
  double small = us_single_update(1000);
  double big = us_single_update(100000);
  std::cout << "[bench] window LAG single update: 1k=" << small
            << "us 100k=" << big << "us ratio=" << (big / small) << "\n";
  // O(partition) would be ~100x; O(affected) stays near-flat. Allow 10x slack.
  REQUIRE(big < small * 10.0);
}

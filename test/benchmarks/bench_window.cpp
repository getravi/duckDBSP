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

// Partition-state footprint and teardown cost — the two axes the packed-row
// storage targets. Reports accounted window bytes, checkpoint serialize time,
// and destruction time for a view holding many mid-size partitions (the wfp
// shape: one partition per entity, ordered periods within).
TEST_CASE("bench: window state footprint / serialize / teardown",
          "[window_bench]") {
  constexpr int kParts = 500, kRows = 400; // 200k rows total
  auto v = std::make_unique<NativeWindowView>(
      "b", "", "t", TableSchema{}, TableSchema{}, [] {
        NativeWindowView::WindowDef w;
        w.function = "LAG";
        w.partition_indices = {0};
        w.sort_columns = {{1, true, true}};
        w.arg_column_idx = 2;
        w.offset = 1;
        return std::vector<NativeWindowView::WindowDef>{w};
      }());
  {
    // Scoped so the input delta dies before teardown is timed — in
    // production the commit's delta Z-set is long gone by close, so the
    // partition stores are the last owners of their rows.
    DuckDBZSet init;
    for (int p = 0; p < kParts; p++)
      for (int o = 0; o < kRows; o++)
        init.insert(row3(p, o, o), 1);
    v->apply_changes("t", init);
    v->drop_delta();
  }

  StateBytes sb;
  StateAccounting acct;
  v->account_state(sb, acct);

  auto t0 = high_resolution_clock::now();
  std::vector<uint8_t> blob;
  v->serialize_circuit_node_state(blob);
  double ser_ms =
      duration_cast<microseconds>(high_resolution_clock::now() - t0).count() /
      1000.0;

  NativeWindowView restored("b", "", "t", TableSchema{}, TableSchema{}, [] {
    NativeWindowView::WindowDef w;
    w.function = "LAG";
    w.partition_indices = {0};
    w.sort_columns = {{1, true, true}};
    w.arg_column_idx = 2;
    w.offset = 1;
    return std::vector<NativeWindowView::WindowDef>{w};
  }());
  t0 = high_resolution_clock::now();
  REQUIRE(restored.restore_circuit_node_state(blob.data(), blob.size()));
  double restore_ms =
      duration_cast<microseconds>(high_resolution_clock::now() - t0).count() /
      1000.0;
  REQUIRE(restored.get_result().size() == v->get_result().size());

  t0 = high_resolution_clock::now();
  v.reset(); // destroy the view: partition state + caches + result
  double destroy_ms =
      duration_cast<microseconds>(high_resolution_clock::now() - t0).count() /
      1000.0;

  std::cout << "[bench] window state 200k rows: window_bytes=" << sb.window
            << " blob_bytes=" << blob.size() << " serialize_ms=" << ser_ms
            << " restore_ms=" << restore_ms << " destroy_ms=" << destroy_ms
            << "\n";
  REQUIRE(sb.window > 0);
  REQUIRE(!blob.empty());
}

// Planner-path expression evaluation benchmarks (pre-Phase-D).
//
// Question under test: how expensive is RowExprEval (a 1-row DataChunk
// through ExpressionExecutor per expression per row) on the hot
// apply_changes path, measured against a hand-written lambda doing the
// same work? The answer decides whether Phase D needs vectorized
// expression evaluation.
//
// Method: push one identical pre-built delta through each view via
// apply_changes and time it. No SQL in the timed region; correctness is
// asserted on result sizes, throughput is printed, never asserted
// (machine-dependent).

#include "../test_helpers.hpp"
#include "catch.hpp"

#include <chrono>
#include <iostream>

using namespace dbsp_test;
using namespace std::chrono;

namespace {

constexpr int kRows = 100000;

DuckDBZSet build_delta() {
  DuckDBZSet delta;
  for (int i = 0; i < kRows; i++) {
    DuckDBRow row;
    row.columns = {duckdb::Value::INTEGER(i),
                   duckdb::Value::INTEGER(i % 100),
                   duckdb::Value(std::string(1, 'a' + (i % 3)))};
    delta.insert(row, 1);
  }
  return delta;
}

double rows_per_sec(double us) { return kRows / (us / 1e6); }

template <typename Fn> double measure_us(Fn &&fn) {
  auto start = high_resolution_clock::now();
  fn();
  auto end = high_resolution_clock::now();
  return static_cast<double>(
      duration_cast<microseconds>(end - start).count());
}

std::unique_ptr<dbsp_native::NativeMaterializedView>
translate_view(DuckDBTestHarness &db, const std::string &name,
               const std::string &sql) {
  dbsp_native::InternalQueryGuard guard;
  auto result =
      dbsp_native::PlanTranslator::translate(*db.conn().context, name, sql);
  INFO("translate error: " << result.error);
  REQUIRE(result.view != nullptr);
  return std::move(result.view);
}

} // namespace

TEST_CASE("Benchmark: RowExprEval vs hand-written lambda on filter path",
          "[benchmark][planner_eval]") {
  DuckDBTestHarness db;
  db.exec("CREATE TABLE t (id INT, val INT, tag VARCHAR)");

  const DuckDBZSet delta = build_delta();
  const std::string sql = "SELECT id, val, tag FROM t WHERE val > 50";
  const size_t expected = 49000; // val in [0,99], 51..99 pass, 1000 each

  // (a) planner view, IR optimizer ON (FILTER_MAP fused: 1 predicate +
  //     3 projection RowExprEvals per surviving row)
  auto planner_fused = translate_view(db, "bench_fused", sql);
  double fused_us = measure_us(
      [&]() { planner_fused->apply_changes("t", delta); });
  REQUIRE(planner_fused->get_result().size() == expected);

  // (b) planner view, IR optimizer OFF (separate filter + map nodes)
  dbsp_native::g_plan_ir_optimize = false;
  auto planner_raw = translate_view(db, "bench_raw", sql);
  dbsp_native::g_plan_ir_optimize = true;
  double raw_us =
      measure_us([&]() { planner_raw->apply_changes("t", delta); });
  REQUIRE(planner_raw->get_result().size() == expected);

  // (c) hand-written lambda baseline: same predicate, no projection copy
  //     needed (identity), zero ExpressionExecutor involvement
  dbsp_native::TableSchema schema;
  schema.table_name = "t";
  schema.columns = {{"id", duckdb::LogicalType::INTEGER},
                    {"val", duckdb::LogicalType::INTEGER},
                    {"tag", duckdb::LogicalType::VARCHAR}};
  dbsp_native::NativeFilterView hand("bench_hand", sql, "t", schema,
                                     [](const dbsp_native::DuckDBRow &row) {
                                       const auto &v = row.columns[1];
                                       return !v.IsNull() &&
                                              v.GetValue<int32_t>() > 50;
                                     });
  double hand_us = measure_us([&]() { hand.apply_changes("t", delta); });
  REQUIRE(hand.get_result().size() == expected);

  std::cout << "[bench] filter " << kRows << " rows:\n"
            << "  planner fused   : " << fused_us << " us ("
            << rows_per_sec(fused_us) << " rows/s)\n"
            << "  planner unfused : " << raw_us << " us ("
            << rows_per_sec(raw_us) << " rows/s)\n"
            << "  hand lambda     : " << hand_us << " us ("
            << rows_per_sec(hand_us) << " rows/s)\n"
            << "  eval overhead   : " << (fused_us / hand_us)
            << "x vs hand lambda; fusion saves "
            << (1.0 - fused_us / raw_us) * 100.0 << "%\n";
}

TEST_CASE("Benchmark: planner aggregate throughput",
          "[benchmark][planner_eval]") {
  DuckDBTestHarness db;
  db.exec("CREATE TABLE t (id INT, val INT, tag VARCHAR)");

  const DuckDBZSet delta = build_delta();
  auto agg = translate_view(db, "bench_agg",
                            "SELECT tag, SUM(val), COUNT(*) FROM t GROUP BY tag");
  double agg_us = measure_us([&]() { agg->apply_changes("t", delta); });
  REQUIRE(agg->get_result().size() == 3); // tags a, b, c

  std::cout << "[bench] aggregate " << kRows << " rows: " << agg_us
            << " us (" << rows_per_sec(agg_us) << " rows/s)\n";
}

TEST_CASE("Benchmark: planner join delta throughput",
          "[benchmark][planner_eval]") {
  DuckDBTestHarness db;
  db.exec("CREATE TABLE t (id INT, val INT, tag VARCHAR)");
  db.exec("CREATE TABLE u (id INT, val INT, tag VARCHAR)");

  auto join = translate_view(
      db, "bench_join",
      "SELECT t.id, u.val FROM t JOIN u ON t.id = u.id");

  // Preload one side, then time the other side's delta (u-rows arrive
  // against a populated t index)
  const DuckDBZSet delta = build_delta();
  join->apply_changes("t", delta);
  double join_us = measure_us([&]() { join->apply_changes("u", delta); });
  REQUIRE(join->get_result().size() == kRows);

  std::cout << "[bench] join delta " << kRows << " rows vs " << kRows
            << "-row index: " << join_us << " us ("
            << rows_per_sec(join_us) << " rows/s)\n";
}


TEST_CASE("Benchmark: cascaded view sync cost", "[benchmark][cascade_bench]") {
  // 50k-row base, 3-level chain (filter -> aggregate -> sort). Time 20
  // single-row syncs: with reset+recompute cascades this is O(base size)
  // per sync; with delta propagation it must be O(delta).
  DuckDBTestHarness db;
  db.exec("CREATE TABLE big (id INT, val INT, tag VARCHAR)");
  db.exec("INSERT INTO big SELECT range, range % 100, chr(CAST(97 + range % 3 AS INT)) "
          "FROM range(50000)");
  db.exec("SELECT * FROM dbsp_track('big')");
  db.exec("SELECT * FROM dbsp_sync('big')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  db.exec("SELECT * FROM dbsp_create_view('cb_f', "
          "'SELECT id, val, tag FROM big WHERE val > 10')");
  db.exec("SELECT * FROM dbsp_create_view('cb_a', "
          "'SELECT tag, COUNT(*) AS n, SUM(val) AS s FROM cb_f GROUP BY tag')");
  db.exec("SELECT * FROM dbsp_create_view('cb_s', "
          "'SELECT tag, s FROM cb_a ORDER BY s DESC')");

  double total_us = measure_us([&]() {
    for (int i = 0; i < 20; i++) {
      db.exec("INSERT INTO big VALUES (" + std::to_string(100000 + i) +
              ", 42, 'a')");
      db.exec("SELECT * FROM dbsp_sync('big')");
    }
  });
  auto rows = db.query("SELECT * FROM dbsp_query('cb_s')");
  REQUIRE_FALSE(rows->HasError());
  REQUIRE(rows->RowCount() == 3);

  // Isolate cascade cost from the scan-and-diff sync cost: on_insert
  // feeds a single-row delta straight into propagate_changes (no table
  // scan). 1000 rows through the whole 3-level chain.
  auto &manager = dbsp_native::get_cdc_manager();
  double prop_us = measure_us([&]() {
    for (int i = 0; i < 1000; i++) {
      dbsp_native::DuckDBRow row;
      row.columns = {duckdb::Value::INTEGER(300000 + i),
                     duckdb::Value::INTEGER(42), duckdb::Value("a")};
      manager.on_insert("big", row);
    }
  });

  std::cout << "[bench] 3-level chain, full sync (scan-dominated): "
            << (total_us / 20.0) << " us/row; propagate-only (on_insert): "
            << (prop_us / 1000.0) << " us/row through the whole chain\n";
}

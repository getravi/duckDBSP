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

  // G2 captured-delta path: explicit-txn single-row inserts with auto-sync.
  // Realign the tracked baseline first — the on_insert block above injected
  // rows into manager state that are not in the DuckDB table, and the
  // commit guard would (correctly) reject the mismatch.
  db.exec("SELECT * FROM dbsp_sync('big')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");
  auto &mgr2 = dbsp_native::get_cdc_manager();
  const uint64_t cap_before = mgr2.captured_delta_syncs();
  double cap_us = measure_us([&]() {
    for (int i = 0; i < 20; i++) {
      db.exec("BEGIN");
      db.exec("INSERT INTO big VALUES (" + std::to_string(400000 + i) +
              ", 42, 'a')");
      db.exec("COMMIT");
    }
  });
  db.exec("SELECT * FROM dbsp_auto_sync(false)");
  const uint64_t captured = mgr2.captured_delta_syncs() - cap_before;

  // Floor: identical txn shape with auto-sync off (pure DuckDB cost)
  double floor_us = measure_us([&]() {
    for (int i = 0; i < 20; i++) {
      db.exec("BEGIN");
      db.exec("INSERT INTO big VALUES (" + std::to_string(500000 + i) +
              ", 42, 'a')");
      db.exec("COMMIT");
    }
  });

  std::cout << "[bench] 3-level chain, full sync (scan-dominated): "
            << (total_us / 20.0) << " us/row; propagate-only (on_insert): "
            << (prop_us / 1000.0)
            << " us/row; captured-delta commit (explicit txn): "
            << (cap_us / 20.0) << " us/commit (" << captured
            << "/20 via fast path; bare txn floor "
            << (floor_us / 20.0) << " us)\n";
}

// I1: N views probing the same table — shared arrangement means ONE index
// update per delta instead of N. Contrast identical views (1 arrangement)
// against distinct-projection views (N arrangements, the pre-I1 cost shape).
TEST_CASE("Benchmark: shared vs private join arrangements",
          "[benchmark][arrangement_bench]") {
  constexpr int kViews = 8;
  constexpr int kBase = 20000;
  constexpr int kDelta = 2000;
  auto &mgr = dbsp_native::get_cdc_manager();

  auto run = [&](bool shared, bool parallel = false) -> double {
    mgr.set_parallel_sync(parallel);
    DuckDBTestHarness db;
    db.exec("CREATE TABLE l (id INT, val INT)");
    db.exec("CREATE TABLE r (id INT, a INT, b INT, c INT, d INT, e INT, "
            "f INT, g INT, h INT)");
    db.exec("INSERT INTO r SELECT i, i, i, i, i, i, i, i, i FROM "
            "range(" + std::to_string(kBase) + ") s(i)");
    db.exec("INSERT INTO l SELECT i, i FROM range(100) s(i)");
    db.exec("SELECT * FROM dbsp_track('l')");
    db.exec("SELECT * FROM dbsp_track('r')");
    db.exec("SELECT * FROM dbsp_sync('l')");
    db.exec("SELECT * FROM dbsp_sync('r')");

    const char cols[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    for (int v = 0; v < kViews; v++) {
      // shared: all views need the same r columns → one fingerprint;
      // private: each view projects a different column → N fingerprints
      std::string rcol(1, shared ? 'a' : cols[v]);
      std::string sql = "SELECT l.id, l.val, r." + rcol +
                        " FROM l JOIN r ON l.id = r.id";
      db.exec("SELECT * FROM dbsp_create_view('bv" + std::to_string(v) +
              "', '" + sql + "')");
    }
    // Both join sides share since I1b: identical views hold l + r
    // arrangements (2); distinct projections split only the r side
    // (kViews r-side + 1 shared l-side)
    const size_t arrs = mgr.shared_arrangement_count();
    REQUIRE(arrs == (shared ? 2u : static_cast<size_t>(kViews) + 1));

    db.exec("INSERT INTO r SELECT i, i, i, i, i, i, i, i, i FROM range(" +
            std::to_string(kBase) + ", " +
            std::to_string(kBase + kDelta) + ") s(i)");
    auto t0 = high_resolution_clock::now();
    db.exec("SELECT * FROM dbsp_sync('r')");
    auto t1 = high_resolution_clock::now();
    return static_cast<double>(
        duration_cast<microseconds>(t1 - t0).count());
  };

  double shared_us = run(true);
  double private_us = run(false);
  double parallel_us = run(true, /*parallel=*/true);
  mgr.set_parallel_sync(false);
  std::cout << "[bench] " << kViews << " views, " << kDelta
            << "-row delta into " << kBase << "-row probe side: shared (2 "
            "arrangements) " << shared_us << " us; private (" << kViews
            << "+1 arrangements) " << private_us << " us; shared+parallel "
            << parallel_us << " us\n";
}

#include <sys/resource.h>

// K1: baseline spill — RAM footprint and sync cost, disk vs RAM baselines
TEST_CASE("Benchmark: spilled vs RAM baselines", "[benchmark][spill_bench]") {
  constexpr int kRowsBig = 200000;
  auto &mgr = dbsp_native::get_cdc_manager();

  auto rss_mb = []() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);
  };

  auto run = [&](bool spill) {
    DuckDBTestHarness db;
    if (spill) {
      db.exec("SELECT * FROM dbsp_spill(true)");
    }
    db.exec("CREATE TABLE big (id INT, val INT, s VARCHAR)");
    db.exec("INSERT INTO big SELECT i, i % 1000, 'row_' || i FROM range(" +
            std::to_string(kRowsBig) + ") t(i)");
    db.exec("SELECT * FROM dbsp_track('big')");
    auto t0 = high_resolution_clock::now();
    db.exec("SELECT * FROM dbsp_sync('big')");
    auto t1 = high_resolution_clock::now();
    db.exec("SELECT * FROM dbsp_create_view('bv_spill', "
            "'SELECT val, COUNT(*) FROM big GROUP BY val')");
    // Incremental round on the existing baseline
    db.exec("INSERT INTO big SELECT i, i % 1000, 'row_' || i FROM range(" +
            std::to_string(kRowsBig) + ", " +
            std::to_string(kRowsBig + 1000) + ") t(i)");
    auto t2 = high_resolution_clock::now();
    db.exec("SELECT * FROM dbsp_sync('big')");
    auto t3 = high_resolution_clock::now();
    double first_ms =
        duration_cast<microseconds>(t1 - t0).count() / 1000.0;
    double incr_ms =
        duration_cast<microseconds>(t3 - t2).count() / 1000.0;
    std::cout << "[bench] " << (spill ? "spill" : "ram  ") << " baseline: "
              << kRowsBig << "-row first sync " << first_ms
              << " ms; +1000-row resync " << incr_ms << " ms; maxrss "
              << rss_mb() << " MB\n";
    if (spill) {
      db.exec("SELECT * FROM dbsp_spill(false)");
    }
  };

  // Spill FIRST: maxrss is monotone, so the RAM run inflating it later
  // cannot mask the spill run's footprint
  run(true);
  run(false);
}

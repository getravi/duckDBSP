// Perf gate for O(Δ) UPDATE/DELETE auto-sync capture
// (docs/DESIGN_WRITE_CAPTURE.md): a single-row autocommit UPDATE against
// a 1M-row tracked table with a live aggregate view must complete in
// <= 50ms end-to-end (capture SELECT + commit guard + delta propagation).
// The scan-and-diff path this replaces took ~2.4s at this scale.

#include "../test_helpers.hpp"
#include "catch.hpp"
#include <chrono>
#include <iostream>

using namespace dbsp_test;
using namespace std::chrono;

TEST_CASE("Benchmark: single-row UPDATE capture at 1M rows", "[benchmark]") {
  DuckDBTestHarness db;

  db.exec("CREATE TABLE big (id INTEGER, grp INTEGER, val DOUBLE)");
  db.exec("INSERT INTO big SELECT i, i % 1000, (i % 997) * 1.5 "
          "FROM range(1000000) t(i)");
  db.exec("SELECT * FROM dbsp_track('big')");
  db.exec("SELECT * FROM dbsp_sync('big')");
  db.exec("SELECT * FROM dbsp_create_view('big_agg', "
          "'SELECT grp, SUM(val) AS s, COUNT(*) AS n FROM big GROUP BY grp')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  auto &manager = db.manager();
  const uint64_t caps = manager.captured_delta_syncs();
  const uint64_t scans = manager.scan_syncs();

  // Warm-up edit (first statement pays one-time costs: guard connection,
  // prepared count) — still asserted captured
  db.exec("UPDATE big SET val = 1.0 WHERE id = 424242");

  auto start = high_resolution_clock::now();
  db.exec("UPDATE big SET val = 2.5 WHERE id = 123456");
  auto end = high_resolution_clock::now();
  const double ms = duration_cast<microseconds>(end - start).count() / 1000.0;

  std::cout << "[Benchmark] 1M-row single-row UPDATE via capture: " << ms
            << " ms\n";

  // Both edits must have taken the captured path, no scans
  REQUIRE(manager.captured_delta_syncs() == caps + 2);
  REQUIRE(manager.scan_syncs() == scans);
  // View reflects the edit
  auto res = db.query("SELECT s FROM dbsp_query('big_agg') "
                      "WHERE grp = 123456 % 1000");
  REQUIRE_FALSE(res->HasError());
  // Hard perf requirement from the task spec (scan-diff took ~2.4s here)
  REQUIRE(ms <= 50.0);

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("Benchmark: single-row DELETE capture at 1M rows", "[benchmark]") {
  DuckDBTestHarness db;

  db.exec("CREATE TABLE bigd (id INTEGER, grp INTEGER, val DOUBLE)");
  db.exec("INSERT INTO bigd SELECT i, i % 1000, i * 0.5 "
          "FROM range(1000000) t(i)");
  db.exec("SELECT * FROM dbsp_track('bigd')");
  db.exec("SELECT * FROM dbsp_sync('bigd')");
  db.exec("SELECT * FROM dbsp_create_view('bigd_agg', "
          "'SELECT grp, COUNT(*) AS n FROM bigd GROUP BY grp')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  auto &manager = db.manager();
  const uint64_t caps = manager.captured_delta_syncs();

  db.exec("DELETE FROM bigd WHERE id = 777"); // warm-up
  auto start = high_resolution_clock::now();
  db.exec("DELETE FROM bigd WHERE id = 555555");
  auto end = high_resolution_clock::now();
  const double ms = duration_cast<microseconds>(end - start).count() / 1000.0;

  std::cout << "[Benchmark] 1M-row single-row DELETE via capture: " << ms
            << " ms\n";
  REQUIRE(manager.captured_delta_syncs() == caps + 2);
  REQUIRE(ms <= 50.0);

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

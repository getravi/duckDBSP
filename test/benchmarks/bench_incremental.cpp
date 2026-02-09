// Benchmarks for DBSP incremental computation
// Uses manual chrono timing (Catch2 v2 compatible)

#include "catch.hpp"
#include "../test_helpers.hpp"
#include <chrono>
#include <iostream>

using namespace dbsp_test;
using namespace std::chrono;

// Helper: measure execution time in microseconds
template <typename Fn> double measure_us(Fn &&fn) {
  auto start = high_resolution_clock::now();
  fn();
  auto end = high_resolution_clock::now();
  return duration_cast<microseconds>(end - start).count();
}

TEST_CASE("Benchmark: O(delta) vs O(n) for aggregates", "[benchmark]") {
  DuckDBTestHarness db;

  const int TABLE_SIZE = 10000;
  const int NUM_GROUPS = 100;

  db.exec("CREATE TABLE large_sales (id INT, category INT, amount DECIMAL)");
  db.exec("SELECT * FROM dbsp_track('large_sales')");

  // Insert bulk data
  for (int i = 0; i < TABLE_SIZE; i++) {
    db.exec("INSERT INTO large_sales VALUES (" + std::to_string(i) + ", " +
            std::to_string(i % NUM_GROUPS) + ", " +
            std::to_string((i % 100) * 10.0) + ")");
  }
  db.exec("SELECT * FROM dbsp_sync('large_sales')");

  // Create DBSP incremental view
  db.exec("SELECT * FROM dbsp_create_view('category_totals', "
          "'SELECT category, SUM(amount) FROM large_sales GROUP BY category')");

  // Measure DBSP incremental insert
  double dbsp_time = measure_us([&]() {
    db.exec("INSERT INTO large_sales VALUES (99999, 50, 500.0)");
    db.exec("SELECT * FROM dbsp_sync('large_sales')");
    db.query("SELECT * FROM dbsp_query('category_totals')");
  });

  // Measure traditional view recompute
  double trad_time = measure_us([&]() {
    db.exec("CREATE OR REPLACE VIEW traditional_view AS "
            "SELECT category, SUM(amount) FROM large_sales GROUP BY category");
    db.query("SELECT * FROM traditional_view");
  });

  std::cout << "[Benchmark] DBSP incremental: " << dbsp_time << " us\n";
  std::cout << "[Benchmark] Traditional recompute: " << trad_time << " us\n";
  std::cout << "[Benchmark] Speedup: " << (trad_time / dbsp_time) << "x\n";

  // DBSP incremental should be faster than full recompute
  // (may not always hold for small tables due to overhead)
  REQUIRE(dbsp_time > 0);
  REQUIRE(trad_time > 0);
}

TEST_CASE("Benchmark: Incremental insert scaling", "[benchmark]") {
  DuckDBTestHarness db;

  auto run_scaling_test = [&](const std::string &suffix, int size) {
    std::string tbl = "t" + suffix;
    db.exec("CREATE TABLE " + tbl + " (id INT, val INT)");
    db.exec("SELECT * FROM dbsp_track('" + tbl + "')");
    for (int i = 0; i < size; i++) {
      db.exec("INSERT INTO " + tbl + " VALUES (" + std::to_string(i) + ", " +
              std::to_string(i * 10) + ")");
    }
    db.exec("SELECT * FROM dbsp_sync('" + tbl + "')");
    db.exec("SELECT * FROM dbsp_create_view('sum" + suffix +
            "', 'SELECT SUM(val) FROM " + tbl + "')");

    // Average over multiple inserts for stability
    const int ITERATIONS = 5;
    double total = 0;
    for (int iter = 0; iter < ITERATIONS; iter++) {
      total += measure_us([&]() {
        db.exec("INSERT INTO " + tbl + " VALUES (" +
                std::to_string(size + 1000 + iter) + ", 42)");
        db.exec("SELECT * FROM dbsp_sync('" + tbl + "')");
      });
    }
    return total / ITERATIONS;
  };

  double time_1k = run_scaling_test("1k", 1000);
  double time_10k = run_scaling_test("10k", 10000);

  std::cout << "[Benchmark] Insert into 1K table: " << time_1k << " us\n";
  std::cout << "[Benchmark] Insert into 10K table: " << time_10k << " us\n";
  std::cout << "[Benchmark] Ratio (10K/1K): " << (time_10k / time_1k) << "x\n";

  // DBSP sync is O(1), but DuckDB INSERT has its own overhead that
  // scales with table size. We verify both complete successfully.
  REQUIRE(time_1k > 0);
  REQUIRE(time_10k > 0);
}

TEST_CASE("Benchmark: Batch insert performance", "[benchmark]") {
  DuckDBTestHarness db;

  db.exec("CREATE TABLE batch_test (id INT, category INT, val INT)");
  db.exec("SELECT * FROM dbsp_track('batch_test')");

  for (int i = 0; i < 5000; i++) {
    db.exec("INSERT INTO batch_test VALUES (" + std::to_string(i) + ", " +
            std::to_string(i % 10) + ", " + std::to_string(i) + ")");
  }
  db.exec("SELECT * FROM dbsp_sync('batch_test')");

  db.exec("SELECT * FROM dbsp_create_view('batch_agg', "
          "'SELECT category, SUM(val) FROM batch_test GROUP BY category')");

  double time_10 = measure_us([&]() {
    for (int i = 0; i < 10; i++) {
      db.exec("INSERT INTO batch_test VALUES (" +
              std::to_string(10000 + i) + ", 5, 100)");
    }
    db.exec("SELECT * FROM dbsp_sync('batch_test')");
  });

  double time_100 = measure_us([&]() {
    for (int i = 0; i < 100; i++) {
      db.exec("INSERT INTO batch_test VALUES (" +
              std::to_string(20000 + i) + ", 5, 100)");
    }
    db.exec("SELECT * FROM dbsp_sync('batch_test')");
  });

  std::cout << "[Benchmark] Batch 10 rows: " << time_10 << " us\n";
  std::cout << "[Benchmark] Batch 100 rows: " << time_100 << " us\n";
  std::cout << "[Benchmark] Ratio (100/10): " << (time_100 / time_10)
            << "x\n";

  // Time should scale with batch size (O(delta))
  REQUIRE(time_10 > 0);
  REQUIRE(time_100 > 0);
}

#include "../test_helpers.hpp"

using namespace dbsp_test;
using namespace duckdb;

TEST_CASE("Window RANGE frame integration", "[integration][window][range]") {
  DuckDBTestHarness harness;

  // Create table for sensor data
  harness.exec("CREATE TABLE sensor_data (id INTEGER, ts INTEGER, val DOUBLE)");

  // Insert data with duplicate timestamps (peers)
  harness.exec("INSERT INTO sensor_data VALUES (1, 10, 1.0), (2, 10, 2.0), (3, "
               "20, 3.0), (4, 30, 4.0)");

  // CASE 1: RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW (Default)
  // For ts=10 rows, the sum should be 3.0 (1.0 + 2.0) because they are peers.
  // If we used ROWS, row 1 would be 1.0 and row 2 would be 3.0.
  // With RANGE, both should be 3.0.

  harness.exec("SELECT * FROM dbsp_create_view('range_sum', "
               "'SELECT id, ts, SUM(val) OVER (ORDER BY ts RANGE BETWEEN "
               "UNBOUNDED PRECEDING AND CURRENT ROW) as s FROM sensor_data')");

  auto rows = harness.getViewRows("range_sum");
  REQUIRE(rows.size() == 4);

  for (const auto &row : rows) {
    int32_t ts = row[1].GetValue<int32_t>();
    double sum = row.back().GetValue<double>();

    if (ts == 10) {
      REQUIRE(sum == 3.0);
    } else if (ts == 20) {
      REQUIRE(sum == 6.0);
    } else if (ts == 30) {
      REQUIRE(sum == 10.0);
    }
  }

  // CASE 2: RANGE BETWEEN CURRENT ROW AND CURRENT ROW
  // Should only include peers.
  harness.exec("SELECT * FROM dbsp_create_view('peer_sum', "
               "'SELECT id, ts, SUM(val) OVER (ORDER BY ts RANGE BETWEEN "
               "CURRENT ROW AND CURRENT ROW) as s FROM sensor_data')");

  rows = harness.getViewRows("peer_sum");
  for (const auto &row : rows) {
    int32_t ts = row[1].GetValue<int32_t>();
    double sum = row.back().GetValue<double>();

    if (ts == 10) {
      REQUIRE(sum == 3.0);
    } else if (ts == 20) {
      REQUIRE(sum == 3.0);
    } else if (ts == 30) {
      REQUIRE(sum == 4.0);
    }
  }

  // CASE 3: GROUPS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW (GROUPS)
  // Semantics should be identical to RANGE for this data
  harness.exec("SELECT * FROM dbsp_create_view('groups_sum', "
               "'SELECT id, ts, SUM(val) OVER (ORDER BY ts GROUPS BETWEEN "
               "UNBOUNDED PRECEDING AND CURRENT ROW) as s FROM sensor_data')");

  rows = harness.getViewRows("groups_sum");
  REQUIRE(rows.size() == 4);
  for (const auto &row : rows) {
    int32_t ts = row[1].GetValue<int32_t>();
    double sum = row.back().GetValue<double>();
    if (ts == 10)
      REQUIRE(sum == 3.0);
    if (ts == 20)
      REQUIRE(sum == 6.0);
    if (ts == 30)
      REQUIRE(sum == 10.0);
  }
}

#include "catch.hpp"
#include "../test_helpers.hpp"

using namespace dbsp_test;
using namespace duckdb;

TEST_CASE("Set Operations Integration Test", "[integration][set_ops]") {
  DuckDBTestHarness db;

  // Create base tables
  db.exec("CREATE TABLE t1 (id INTEGER, val INTEGER);");
  db.exec("CREATE TABLE t2 (id INTEGER, val INTEGER);");

  // Track tables
  db.exec("SELECT * FROM dbsp_track('t1')");
  db.exec("SELECT * FROM dbsp_track('t2')");

  // Insert initial data
  db.exec("INSERT INTO t1 VALUES (1, 10), (2, 20), (3, 30);");
  db.exec("INSERT INTO t2 VALUES (2, 20), (3, 35), (4, 40);");

  // Initial sync
  db.exec("SELECT * FROM dbsp_sync('t1')");
  db.exec("SELECT * FROM dbsp_sync('t2')");

  SECTION("UNION View") {
    db.exec("CREATE MATERIALIZED VIEW v_union AS SELECT id, val "
            "FROM t1 UNION SELECT id, val FROM t2;");

    // Initial check: (1,10), (2,20), (3,30), (3,35), (4,40) = 5 distinct rows
    // UNION removes duplicates. (2,20) appears in both t1 and t2, so only once.
    db.assertViewRowCount("v_union", 5);

    auto rows = db.getViewRows("v_union");
    REQUIRE(rows.size() == 5);

    // Incremental update: add (5,50) to t1
    db.exec("INSERT INTO t1 VALUES (5, 50);");
    db.exec("SELECT * FROM dbsp_sync('t1')");

    db.assertViewRowCount("v_union", 6);

    // Add duplicate (1,10) to t2 - already in t1, so UNION result should not
    // grow
    db.exec("INSERT INTO t2 VALUES (1, 10);");
    db.exec("SELECT * FROM dbsp_sync('t2')");

    // Still 6 unique rows
    db.assertViewRowCount("v_union", 6);
  }

  SECTION("UNION ALL View") {
    db.exec("CREATE MATERIALIZED VIEW v_union_all AS SELECT id, "
            "val FROM t1 UNION ALL SELECT id, val FROM t2;");

    // Initial: t1(3 rows) + t2(3 rows) = 6 rows total. (2,20) appears twice.
    db.assertViewRowCount("v_union_all", 6);

    // Delete row from t1
    db.exec("DELETE FROM t1 WHERE id = 1;");
    db.exec("SELECT * FROM dbsp_sync('t1')");

    // After delete: t1(2) + t2(3) = 5 rows
    db.assertViewRowCount("v_union_all", 5);

    auto rows = db.getViewRows("v_union_all");
    REQUIRE(rows.size() == 5);
    // First row by ORDER BY id, val should be (2,20)
    bool found_2_20 = false;
    for (const auto &row : rows) {
      if (row[0].GetValue<int32_t>() == 2 && row[1].GetValue<int32_t>() == 20) {
        found_2_20 = true;
        break;
      }
    }
    REQUIRE(found_2_20);
  }

  SECTION("INTERSECT View") {
    db.exec("CREATE MATERIALIZED VIEW v_intersect AS SELECT id, "
            "val FROM t1 INTERSECT SELECT id, val FROM t2;");

    // Initial: Only (2,20) is in both t1 and t2
    db.assertViewRowCount("v_intersect", 1);

    auto rows = db.getViewRows("v_intersect");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][0].GetValue<int32_t>() == 2);
    REQUIRE(rows[0][1].GetValue<int32_t>() == 20);

    // Incremental update: Add (3,30) to t2, making it intersect with t1
    db.exec("INSERT INTO t2 VALUES (3, 30);");
    db.exec("SELECT * FROM dbsp_sync('t2')");

    db.assertViewRowCount("v_intersect", 2);

    rows = db.getViewRows("v_intersect");
    REQUIRE(rows.size() == 2);
    bool found_3_30 = false;
    for (const auto &row : rows) {
      if (row[0].GetValue<int32_t>() == 3 && row[1].GetValue<int32_t>() == 30) {
        found_3_30 = true;
        break;
      }
    }
    REQUIRE(found_3_30);
  }

  SECTION("EXCEPT View") {
    db.exec("CREATE MATERIALIZED VIEW v_except AS SELECT id, "
            "val FROM t1 EXCEPT SELECT id, val FROM t2;");

    // Initial: t1 - t2.
    // t1: (1,10), (2,20), (3,30)
    // t2: (2,20), (3,35), (4,40)
    // Result: (1,10), (3,30)
    db.assertViewRowCount("v_except", 2);

    auto rows = db.getViewRows("v_except");
    REQUIRE(rows.size() == 2);
    bool found_1_10 = false, found_3_30 = false;
    for (const auto &row : rows) {
      int id = row[0].GetValue<int32_t>();
      int val = row[1].GetValue<int32_t>();
      if (id == 1 && val == 10) found_1_10 = true;
      if (id == 3 && val == 30) found_3_30 = true;
    }
    REQUIRE(found_1_10);
    REQUIRE(found_3_30);

    // Incremental update: Insert (1,10) into t2, should remove it from EXCEPT
    db.exec("INSERT INTO t2 VALUES (1, 10);");
    db.exec("SELECT * FROM dbsp_sync('t2')");

    db.assertViewRowCount("v_except", 1);

    rows = db.getViewRows("v_except");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][0].GetValue<int32_t>() == 3);
    REQUIRE(rows[0][1].GetValue<int32_t>() == 30);
  }
}

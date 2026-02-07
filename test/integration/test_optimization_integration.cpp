#define CATCH_CONFIG_MAIN
#include "../test_helpers.hpp"
#include "catch.hpp"

using namespace dbsp_test;

TEST_CASE("Optimization Integration", "[optimization]") {
  DuckDBTestHarness db;

  SECTION("Filter Pushdown through JOIN") {
    // Create tables
    db.exec("CREATE TABLE t1 (id INTEGER, val1 INTEGER)");
    db.exec("CREATE TABLE t2 (id INTEGER, val2 INTEGER)");

    db.exec("INSERT INTO t1 VALUES (1, 100), (2, 200), (3, 300)");
    db.exec("INSERT INTO t2 VALUES (1, 10), (2, 20), (3, 30)");

    // Track tables
    db.exec("SELECT * FROM dbsp_track('t1')");
    db.exec("SELECT * FROM dbsp_sync('t1')");
    db.exec("SELECT * FROM dbsp_track('t2')");
    db.exec("SELECT * FROM dbsp_sync('t2')");

    // Create a view with filters that can be pushed down
    // Filter on t1.val1 > 150 (should be pushed to left)
    // Filter on t2.val2 < 25 (should be pushed to right)
    std::string view_sql = "SELECT t1.id, t1.val1, t2.val2 "
                           "FROM t1 JOIN t2 ON t1.id = t2.id "
                           "WHERE t1.val1 > 150 AND t2.val2 < 25";

    db.exec("SELECT * FROM dbsp_create_view('v_optimized', '" + view_sql +
            "')");

    // Verify results
    // t1 rows > 150: (2, 200), (3, 300)
    // t2 rows < 25: (1, 10), (2, 20)
    // Join on id:
    // id 2: t1 matches (200 > 150), t2 matches (20 < 25). Result: (2, 200, 20)
    // id 1: t1 fails (100 !> 150)
    // id 3: t2 fails (30 !< 25)

    auto result =
        db.query("SELECT * FROM dbsp_query('v_optimized') ORDER BY id");
    REQUIRE(!result->HasError());
    REQUIRE(result->RowCount() == 1);
    REQUIRE(result->GetValue(0, 0).GetValue<int>() == 2);
    REQUIRE(result->GetValue(1, 0).GetValue<int>() == 200);
    REQUIRE(result->GetValue(2, 0).GetValue<int>() == 20);

    // Drop view
    db.exec("SELECT dbsp_drop('v_optimized')");
  }

  SECTION("Projection Pruning") {
    // Create table with many columns
    db.exec("CREATE TABLE wide_table (id INTEGER, c1 INTEGER, c2 INTEGER, c3 "
            "INTEGER, c4 INTEGER)");
    db.exec("INSERT INTO wide_table VALUES (1, 10, 20, 30, 40)");

    db.exec("SELECT * FROM dbsp_track('wide_table')");
    db.exec("SELECT * FROM dbsp_sync('wide_table')");

    // Create view selecting only a subset
    // Optimizer should identify that c3 and c4 are not needed
    db.exec("SELECT * FROM dbsp_create_view('v_pruned', 'SELECT id, c1 FROM "
            "wide_table WHERE c2 > 10')");

    auto result = db.query("SELECT * FROM dbsp_query('v_pruned')");
    REQUIRE(!result->HasError());
    REQUIRE(result->RowCount() == 1);
    REQUIRE(result->GetValue(0, 0).GetValue<int>() == 1);
    REQUIRE(result->GetValue(1, 0).GetValue<int>() == 10);

    // Verify modifications propagate correctly (even if pruned)
    db.exec(
        "INSERT INTO wide_table VALUES (2, 11, 21, 31, 41)"); // Should appear
    db.exec("INSERT INTO wide_table VALUES (3, 12, 5, 32, 42)"); // Should be
                                                                 // filtered out
                                                                 // (c2 <= 10)
    db.exec("SELECT * FROM dbsp_sync('wide_table')");

    // Check result again
    auto result2 = db.query("SELECT * FROM dbsp_query('v_pruned') ORDER BY id");
    REQUIRE(!result2->HasError());
    REQUIRE(result2->RowCount() == 2);

    REQUIRE(result2->GetValue(0, 1).GetValue<int>() == 2);
    REQUIRE(result2->GetValue(1, 1).GetValue<int>() == 11);
  }
}

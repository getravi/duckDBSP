#include "../test_helpers.hpp"
#include <catch.hpp>

using namespace dbsp;

TEST_CASE("Set Operations Integration Test", "[integration][set_ops]") {
  DbspTestHelper helper(true);

  // Create base tables
  REQUIRE(helper.execute("CREATE TABLE t1 (id INTEGER, val INTEGER);"));
  REQUIRE(helper.execute("CREATE TABLE t2 (id INTEGER, val INTEGER);"));

  // Insert initial data
  REQUIRE(helper.execute("INSERT INTO t1 VALUES (1, 10), (2, 20), (3, 30);"));
  REQUIRE(helper.execute("INSERT INTO t2 VALUES (2, 20), (3, 35), (4, 40);"));

  SECTION("UNION View") {
    REQUIRE(helper.execute("CREATE MATERIALIZED VIEW v_union AS SELECT id, val "
                           "FROM t1 UNION SELECT id, val FROM t2;"));

    // Initial check: (1,10), (2,20), (3,30), (3,35), (4,40)
    // Note: UNION (distinct) removes duplicates. (2,20) is in both.
    auto result = helper.query("SELECT * FROM v_union ORDER BY id, val;");
    REQUIRE(result.size() == 5);
    CHECK(result[0] == "1, 10");
    CHECK(result[1] == "2, 20");
    CHECK(result[2] == "3, 30");
    CHECK(result[3] == "3, 35");
    CHECK(result[4] == "4, 40");

    // Incremental update
    REQUIRE(helper.execute("INSERT INTO t1 VALUES (5, 50);"));
    REQUIRE(helper.execute(
        "INSERT INTO t2 VALUES (1, 10);")); // Already in t1, so should not
                                            // appear again in UNION result

    result = helper.query("SELECT * FROM v_union ORDER BY id, val;");
    REQUIRE(result.size() == 6);
    CHECK(result[5] == "5, 50");
  }

  SECTION("UNION ALL View") {
    REQUIRE(helper.execute("CREATE MATERIALIZED VIEW v_union_all AS SELECT id, "
                           "val FROM t1 UNION ALL SELECT id, val FROM t2;"));

    // Initial check: t1(3) + t2(3) = 6 rows. (2,20) appears twice.
    auto result = helper.query("SELECT * FROM v_union_all ORDER BY id, val;");
    REQUIRE(result.size() == 6);
    // (1,10), (2,20), (2,20), (3,30), (3,35), (4,40)

    // Incremental update
    REQUIRE(helper.execute("DELETE FROM t1 WHERE id = 1;"));

    result = helper.query("SELECT * FROM v_union_all ORDER BY id, val;");
    REQUIRE(result.size() == 5);
    CHECK(result[0] == "2, 20");
  }

  SECTION("INTERSECT View") {
    REQUIRE(helper.execute("CREATE MATERIALIZED VIEW v_intersect AS SELECT id, "
                           "val FROM t1 INTERSECT SELECT id, val FROM t2;"));

    // Initial check: Only (2,20) is in both
    auto result = helper.query("SELECT * FROM v_intersect ORDER BY id, val;");
    REQUIRE(result.size() == 1);
    CHECK(result[0] == "2, 20");

    // Incremental update: Add (3,30) to t2, making it intersect with t1
    REQUIRE(helper.execute("INSERT INTO t2 VALUES (3, 30);"));

    result = helper.query("SELECT * FROM v_intersect ORDER BY id, val;");
    REQUIRE(result.size() == 2);
    CHECK(result[1] == "3, 30");
  }

  SECTION("EXCEPT View") {
    REQUIRE(helper.execute("CREATE MATERIALIZED VIEW v_except AS SELECT id, "
                           "val FROM t1 EXCEPT SELECT id, val FROM t2;"));

    // Initial check: t1 - t2.
    // t1: (1,10), (2,20), (3,30)
    // t2: (2,20), (3,35), (4,40)
    // Result: (1,10), (3,30)
    auto result = helper.query("SELECT * FROM v_except ORDER BY id, val;");
    REQUIRE(result.size() == 2);
    CHECK(result[0] == "1, 10");
    CHECK(result[1] == "3, 30");

    // Incremental update: Insert (1,10) into t2, should remove it from EXCEPT
    // result
    REQUIRE(helper.execute("INSERT INTO t2 VALUES (1, 10);"));

    result = helper.query("SELECT * FROM v_except ORDER BY id, val;");
    REQUIRE(result.size() == 1);
    CHECK(result[0] == "3, 30");
  }
}

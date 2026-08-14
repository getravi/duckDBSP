// Regression: CASE + later column refs silently wrong (BatchEvaluator
// stale result-vector reference).
//
// DuckDB's CASE executor has all-one-branch fast paths that make the
// result vector a REFERENCE to an input chunk column. BatchEvaluator
// reused each expression's result vector across execute() calls, so a
// later batch that took the mixed-branch path (FillSwitch) wrote through
// the stale pointer straight into the shared input chunk — corrupting the
// branch's source column for every expression evaluated AFTER the CASE in
// that batch. Reported by NumPad as "CASE across a self-join is silently
// wrong"; the self-join merely produced the priming batch sequence
// (an all-one-branch batch, then a mixed one).
//
// Python twin (compares against stock DuckDB): test/python/test_self_join_case.py

#include "catch.hpp"
#include "../test_helpers.hpp"

using namespace dbsp_test;

TEST_CASE("CASE then plain ref across a self-join stays correct",
          "[integration][case_batch_eval]") {
    DuckDBTestHarness db;

    db.createTable("base", "d0 INT, v DOUBLE",
                   {"(1, 10.0)", "(2, 20.0)", "(3, 30.0)"});
    db.createTable("tb", "d0 INT, s DOUBLE, e DOUBLE",
                   {"(1, 100.0, 1000.0)", "(2, -200.0, 2000.0)"});
    db.exec("SELECT * FROM dbsp_track('base')");
    db.exec("SELECT * FROM dbsp_track('tb')");
    db.exec("SELECT * FROM dbsp_sync('base')");
    db.exec("SELECT * FROM dbsp_sync('tb')");

    db.exec("SELECT * FROM dbsp_create_view('sj', "
            "'SELECT base.d0, "
            " CASE WHEN a1.s > 0 THEN a1.s ELSE a2.e END AS c, "
            " a1.s AS p1, a2.e AS p2 "
            " FROM base "
            " LEFT JOIN tb a1 ON a1.d0 = base.d0 "
            " LEFT JOIN tb a2 ON a2.d0 = base.d0')");

    auto rows = db.getViewRows("sj");
    REQUIRE(rows.size() == 3);
    for (const auto &row : rows) {
        const int32_t d0 = row[0].GetValue<int32_t>();
        if (d0 == 1) {
            // Used to come back as p2 == 100.0 (the CASE's value):
            // the CASE clobbered a2.e in the shared chunk.
            REQUIRE(row[1].GetValue<double>() == 100.0);  // c   = a1.s
            REQUIRE(row[2].GetValue<double>() == 100.0);  // p1  = a1.s
            REQUIRE(row[3].GetValue<double>() == 1000.0); // p2  = a2.e
        } else if (d0 == 2) {
            REQUIRE(row[1].GetValue<double>() == 2000.0);
            REQUIRE(row[2].GetValue<double>() == -200.0);
            REQUIRE(row[3].GetValue<double>() == 2000.0);
        } else {
            REQUIRE(d0 == 3); // unmatched: all pads NULL
            REQUIRE(row[1].IsNull());
            REQUIRE(row[2].IsNull());
            REQUIRE(row[3].IsNull());
        }
    }
}

TEST_CASE("staged commits: all-one-branch batch then mixed batch",
          "[integration][case_batch_eval]") {
    DuckDBTestHarness db;

    // No join needed: the first commit's rows all take the ELSE branch
    // (primes the stale reference); the second commit is mixed (used to
    // write the CASE results into the chunk's `e` column).
    db.createTable("tb2", "d0 INT, s DOUBLE, e DOUBLE", {});
    db.exec("SELECT * FROM dbsp_track('tb2')");
    db.exec("SELECT * FROM dbsp_sync('tb2')");
    db.exec("SELECT * FROM dbsp_create_view('staged', "
            "'SELECT d0, CASE WHEN s > 0 THEN s ELSE e END AS c, e AS p2 "
            " FROM tb2')");

    db.exec("INSERT INTO tb2 VALUES (2, -200.0, 2000.0)"); // all-ELSE
    db.exec("SELECT * FROM dbsp_sync('tb2')");
    db.exec("INSERT INTO tb2 VALUES (1, 100.0, 1000.0), (4, -1.0, 4000.0)");
    db.exec("SELECT * FROM dbsp_sync('tb2')"); // mixed

    auto rows = db.getViewRows("staged");
    REQUIRE(rows.size() == 3);
    for (const auto &row : rows) {
        const int32_t d0 = row[0].GetValue<int32_t>();
        const double c = row[1].GetValue<double>();
        const double p2 = row[2].GetValue<double>();
        if (d0 == 1) {
            REQUIRE(c == 100.0);
            REQUIRE(p2 == 1000.0); // used to come back as 100.0
        } else if (d0 == 2) {
            REQUIRE(c == 2000.0);
            REQUIRE(p2 == 2000.0);
        } else {
            REQUIRE(d0 == 4);
            REQUIRE(c == 4000.0);
            REQUIRE(p2 == 4000.0);
        }
    }
}

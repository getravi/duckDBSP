// D2 plan tee (include/dbsp_plan_tee.hpp): DELETE shapes the design-1
// pre-image capture declines are served by teeing the rows the plan
// actually processed. Each scenario checks the view against direct SQL
// and asserts via counters that NO scan ran.

#include "../test_helpers.hpp"
#include "catch.hpp"

using namespace dbsp_test;

namespace {

struct TeeFixture {
  DuckDBTestHarness db;

  TeeFixture() {
    db.createTable("tt", "id INT, grp INT, val INT",
                   {"(1, 1, 10)", "(2, 1, 20)", "(3, 2, 30)", "(4, 2, 40)"});
    db.createTable("tu", "id INT, tag VARCHAR", {"(1, 'x')", "(3, 'y')"});
    db.exec("SELECT * FROM dbsp_track('tt')");
    db.exec("SELECT * FROM dbsp_track('tu')");
    db.exec("SELECT * FROM dbsp_sync('tt')");
    db.exec("SELECT * FROM dbsp_sync('tu')");
    db.exec("SELECT * FROM dbsp_create_view('tv_agg', 'SELECT grp, "
            "SUM(val) AS s, COUNT(*) AS n FROM tt GROUP BY grp')");
    db.exec("SELECT * FROM dbsp_auto_sync(true)");
  }

  void finish() { db.exec("SELECT * FROM dbsp_auto_sync(false)"); }

  void check() {
    auto expected = db.query("SELECT * FROM (SELECT grp, SUM(val) AS s, "
                             "COUNT(*) AS n FROM tt GROUP BY grp) "
                             "ORDER BY ALL");
    auto actual = db.query("SELECT * FROM dbsp_query('tv_agg') ORDER BY ALL");
    REQUIRE_FALSE(expected->HasError());
    REQUIRE_FALSE(actual->HasError());
    REQUIRE(actual->RowCount() == expected->RowCount());
    for (size_t r = 0; r < expected->RowCount(); r++) {
      for (size_t c = 0; c < expected->ColumnCount(); c++) {
        REQUIRE(actual->GetValue(c, r).ToString() ==
                expected->GetValue(c, r).ToString());
      }
    }
  }
};

} // namespace

TEST_CASE("plan tee: same-table-twice transaction stays O(delta)",
          "[integration][plan_tee]") {
  TeeFixture fx;
  auto &m = fx.db.manager();
  // design 1 declines the DELETE (table already written this txn);
  // the tee catches it — no scan at commit
  const uint64_t caps = m.captured_delta_syncs();
  const uint64_t scans = m.scan_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("INSERT INTO tt VALUES (9, 1, 90)");
  fx.db.exec("DELETE FROM tt WHERE id = 1");
  fx.db.exec("COMMIT");
  REQUIRE(m.captured_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check();
  fx.finish();
}

TEST_CASE("plan tee: post-write subquery DELETE stays O(delta)",
          "[integration][plan_tee]") {
  TeeFixture fx;
  auto &m = fx.db.manager();
  // the INSERT into tu poisons design 1's committed-state view for the
  // subquery; the tee sees the executed rows regardless
  const uint64_t caps = m.captured_delta_syncs();
  const uint64_t scans = m.scan_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("INSERT INTO tu VALUES (4, 'z')");
  fx.db.exec("DELETE FROM tt WHERE id IN (SELECT id FROM tu)");
  fx.db.exec("COMMIT");
  // one captured apply per table: the G2 INSERT (tu) + the teed DELETE (tt)
  REQUIRE(m.captured_delta_syncs() == caps + 2);
  REQUIRE(m.scan_syncs() == scans);
  fx.check();
  // the subquery must have seen the txn-local INSERT: id 4 deleted too
  auto res = fx.db.query("SELECT COUNT(*) FROM tt");
  REQUIRE(res->GetValue(0, 0).GetValue<int64_t>() == 1); // only id 2 left
  fx.finish();
}

TEST_CASE("plan tee: rollback discards teed rows",
          "[integration][plan_tee]") {
  TeeFixture fx;
  auto &m = fx.db.manager();
  const uint64_t caps = m.captured_delta_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("INSERT INTO tt VALUES (9, 1, 90)"); // poison design-1 DELETE
  fx.db.exec("DELETE FROM tt WHERE grp = 1");
  fx.db.exec("ROLLBACK");
  REQUIRE(m.captured_delta_syncs() == caps);
  fx.check(); // views match the unchanged table
  fx.finish();
}

TEST_CASE("plan tee: zero-match DELETE after write skips the scan",
          "[integration][plan_tee]") {
  TeeFixture fx;
  auto &m = fx.db.manager();
  const uint64_t scans = m.scan_syncs();
  const uint64_t caps = m.captured_delta_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("INSERT INTO tt VALUES (9, 3, 90)");
  fx.db.exec("DELETE FROM tt WHERE id = 777"); // matches nothing
  fx.db.exec("COMMIT");
  // teed empty delta merges with the captured INSERT: still fast path
  REQUIRE(m.captured_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check();
  fx.finish();
}

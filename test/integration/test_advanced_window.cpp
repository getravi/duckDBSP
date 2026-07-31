#include "../test_helpers.hpp"

using namespace dbsp_test;
using namespace duckdb;

namespace {

// Compare a materialized view's content against stock DuckDB's direct
// answer for the same SQL. Both sides sorted; values compared as strings
// (matches the planner-frontend differential harness convention).
void requireViewMatchesQuery(DuckDBTestHarness &db, const std::string &view,
                             const std::string &sql) {
  auto expected = db.query("SELECT * FROM (" + sql + ") ORDER BY ALL");
  auto actual =
      db.query("SELECT * FROM dbsp_query('" + view + "') ORDER BY ALL");
  INFO("expected error: " << (expected->HasError() ? expected->GetError()
                                                   : "none"));
  INFO("actual error: " << (actual->HasError() ? actual->GetError()
                                               : "none"));
  REQUIRE_FALSE(expected->HasError());
  REQUIRE_FALSE(actual->HasError());
  REQUIRE(actual->ColumnCount() == expected->ColumnCount());
  REQUIRE(actual->RowCount() == expected->RowCount());
  for (size_t r = 0; r < expected->RowCount(); r++) {
    for (size_t c = 0; c < expected->ColumnCount(); c++) {
      INFO("row " << r << " col " << c);
      REQUIRE(actual->GetValue(c, r).ToString() ==
              expected->GetValue(c, r).ToString());
    }
  }
}

} // namespace

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

// The binder wraps frame-bound and LAG/LEAD-offset literals in a BOUND_CAST
// shell (e.g. `11 PRECEDING` arrives as CAST(11 AS BIGINT)); constant_int
// used to require a bare BOUND_CONSTANT and rejected these as "non-constant"
// even though the window machinery (start_offset/end_offset) already
// supports bounded ROWS frames. This is the first real-SQL exercise of that
// machinery, so the assertions run twice: once after the initial load and
// once after mutations that (a) change a value inside an existing frame and
// (b) grow the partition so old values fall OUTSIDE later frames (bounded,
// not cumulative) -- both compared exactly against stock DuckDB.
TEST_CASE("Window constant bounded ROWS frame (AVG) accepted and correct",
          "[integration][window][constant-frame]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER, v DOUBLE)");
  for (int i = 0; i < 20; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", " +
                 std::to_string(i) + ".0)");
  }
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  const std::string sql =
      "SELECT grp, tidx, AVG(v) OVER (PARTITION BY grp ORDER BY tidx ROWS "
      "BETWEEN 11 PRECEDING AND CURRENT ROW) AS avgv FROM t";
  auto create = harness.query(
      "SELECT * FROM dbsp_create_view('w_frame', '" + sql + "')");
  INFO("create error: " << (create->HasError() ? create->GetError() : "none"));
  REQUIRE_FALSE(create->HasError()); // must NOT be DBSP-E110

  requireViewMatchesQuery(harness, "w_frame", sql);

  // (a) mid-window value change: tidx=10 falls inside the trailing 12-row
  // frame for tidx=10..19.
  harness.exec("UPDATE t SET v = 999.0 WHERE tidx = 10");
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_frame", sql);

  // (b) a value leaving the window: grow the partition so the frame for
  // later rows no longer reaches back to the early rows (bounded ROWS,
  // not UNBOUNDED PRECEDING) -- exercises the structural full-render path.
  for (int i = 20; i < 26; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", " +
                 std::to_string(i) + ".0)");
  }
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_frame", sql);
}

// LAG(v, 12): a constant, non-default offset. Same constant_int gate as the
// frame-bound test above.
TEST_CASE("Window LAG with constant non-default offset accepted and correct",
          "[integration][window][constant-frame]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER, v DOUBLE)");
  for (int i = 0; i < 20; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", " +
                 std::to_string(i) + ".0)");
  }
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  const std::string sql = "SELECT grp, tidx, LAG(v, 12) OVER (PARTITION BY "
                          "grp ORDER BY tidx) AS lagv FROM t";
  auto create = harness.query(
      "SELECT * FROM dbsp_create_view('w_lag12', '" + sql + "')");
  INFO("create error: " << (create->HasError() ? create->GetError() : "none"));
  REQUIRE_FALSE(create->HasError()); // must NOT be "non-constant offset"

  requireViewMatchesQuery(harness, "w_lag12", sql);

  // mid-window value change: tidx=3 is read by tidx=15's LAG(v,12).
  harness.exec("UPDATE t SET v = 999.0 WHERE tidx = 3");
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_lag12", sql);

  // structural growth (full-render path)
  for (int i = 20; i < 26; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", " +
                 std::to_string(i) + ".0)");
  }
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_lag12", sql);
}

// LAG(v) with the implicit default offset (1) must keep working -- guards
// against the fix regressing the already-working bare-offset case.
TEST_CASE("Window LAG default offset still works",
          "[integration][window][constant-frame]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER, v DOUBLE)");
  for (int i = 0; i < 8; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", " +
                 std::to_string(i) + ".0)");
  }
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  const std::string sql =
      "SELECT grp, tidx, LAG(v) OVER (PARTITION BY grp ORDER BY tidx) AS "
      "lagv FROM t";
  harness.exec("SELECT * FROM dbsp_create_view('w_lag1', '" + sql + "')");
  requireViewMatchesQuery(harness, "w_lag1", sql);

  harness.exec("UPDATE t SET v = 999.0 WHERE tidx = 3");
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_lag1", sql);
}

// NTILE's bucket count and NTH_VALUE's N reach the same BOUND_CAST-wrapped
// literal shape as the frame bounds / LAG-LEAD offsets above, but must stay
// gated: a differential check during this fix (not committed, see the
// task-1 report) found NTILE's bucket-boundary math already diverges from
// stock DuckDB for uneven partition sizes -- pre-existing and out of this
// task's scope. constant_int()'s BOUND_CAST unwrap is deliberately NOT used
// for these two call sites (they keep bare_constant_int) so this stays a
// loud "unsupported" instead of a silently wrong result. This test guards
// that scope boundary.
TEST_CASE("NTILE and NTH_VALUE constant args stay gated (scope boundary)",
          "[integration][window][constant-frame]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER, v DOUBLE)");
  for (int i = 0; i < 10; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", " +
                 std::to_string(i) + ".0)");
  }
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  auto ntile = harness.query(
      "SELECT * FROM dbsp_create_view('w_ntile', "
      "'SELECT grp, tidx, NTILE(4) OVER (PARTITION BY grp ORDER BY tidx) "
      "AS b FROM t')");
  REQUIRE(ntile->HasError());
  REQUIRE(ntile->GetError().find("non-constant bucket count") !=
         std::string::npos);

  auto nth_value = harness.query(
      "SELECT * FROM dbsp_create_view('w_nth', "
      "'SELECT grp, tidx, NTH_VALUE(v, 3) OVER (PARTITION BY grp ORDER BY "
      "tidx ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS n "
      "FROM t')");
  REQUIRE(nth_value->HasError());
  REQUIRE(nth_value->GetError().find("non-constant N") != std::string::npos);
}

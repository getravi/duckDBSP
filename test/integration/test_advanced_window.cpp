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

// Fast-path EXCLUSION boundary: a pure value UPDATE (no row count change)
// stays on NativeWindowView's fast re-emit path (emit_affected/
// affected_indices), not the structural full-render fallback. The AVG-frame
// test above only ever updates a row close enough to the partition's end
// that every later row is within reach of the 12-row trailing frame -- it
// can never prove affected_indices correctly EXCLUDES rows whose frame
// doesn't reach the edited row. Use a partition at least 2x the frame width
// (24 rows, frame width 12) and edit the FIRST row: rows 0..11 must pick up
// the new value (it's within their trailing frame), rows 12..23 must not
// (row 0 is outside frame [r-11, r] once r >= 12) -- their AVG must stay
// exactly what it was, verified by an exact-value diff against stock
// DuckDB's own recompute over the whole partition, not just the changed
// rows.
TEST_CASE("Window bounded frame fast-path excludes rows outside its reach",
          "[integration][window][constant-frame]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER, v DOUBLE)");
  for (int i = 0; i < 24; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", " +
                 std::to_string(i) + ".0)");
  }
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  const std::string sql =
      "SELECT grp, tidx, AVG(v) OVER (PARTITION BY grp ORDER BY tidx ROWS "
      "BETWEEN 11 PRECEDING AND CURRENT ROW) AS avgv FROM t";
  auto create = harness.query(
      "SELECT * FROM dbsp_create_view('w_excl', '" + sql + "')");
  REQUIRE_FALSE(create->HasError());
  requireViewMatchesQuery(harness, "w_excl", sql);

  // Pure value update at tidx=0 (start of partition): row count unchanged
  // -> fast path. Rows 0..11 include tidx=0 in their frame and must change;
  // rows 12..23 must not.
  harness.exec("UPDATE t SET v = 999.0 WHERE tidx = 0");
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_excl", sql);
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

// NTH_VALUE's N reaches the same BOUND_CAST-wrapped literal shape as the
// frame bounds / LAG-LEAD offsets and NTILE's bucket count (all of which
// now accept constants -- see the differential tests below for NTILE), but
// must stay gated: reading NTH_VALUE's render logic
// (NativeWindowView, both call sites in dbsp_window_view.hpp) found it
// always indexes the N-th row of the whole partition, ignoring the
// window's frame bounds -- diverges from stock DuckDB's frame-relative
// NTH_VALUE whenever the frame is narrower than the full partition.
// constant_int()'s BOUND_CAST unwrap is deliberately NOT used for this
// call site (it keeps bare_constant_int) so this stays a loud
// "unsupported" instead of a silently wrong result. This test guards that
// scope boundary.
// All-NULL-frame aggregate semantics: stock DuckDB returns NULL for
// SUM/AVG/MIN/MAX when every value in the frame is NULL (COUNT returns 0,
// which is already a real number, not NULL). duckDBSP's window aggregate
// accumulator folded "no non-null values seen" through as sum=0/count=0 and
// then emitted 0.0 for SUM/AVG instead of NULL -- silently wrong wherever a
// partition (or a prefix of one) is genuinely all-NULL, e.g. wfp's
// zero-employee org/lvl combos feeding Rolling12MAvgHeadcount. grp=1 is
// all-NULL throughout (every bounded frame in it is all-NULL); grp=2 is a
// non-null control that must keep matching stock exactly (regression guard
// for the normal case the existing AVG-frame tests already cover).
TEST_CASE("Window bounded ROWS frame: all-NULL partition returns NULL not 0",
          "[integration][window][null-frame]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER, v DOUBLE)");
  for (int i = 0; i < 10; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", NULL)");
    harness.exec("INSERT INTO t VALUES (2, " + std::to_string(i) + ", " +
                 std::to_string(i) + ".0)");
  }
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  const std::string sql =
      "SELECT grp, tidx, "
      "SUM(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN 3 PRECEDING "
      "AND CURRENT ROW) AS sum_b, "
      "AVG(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN 3 PRECEDING "
      "AND CURRENT ROW) AS avg_b, "
      "MIN(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN 3 PRECEDING "
      "AND CURRENT ROW) AS min_b, "
      "MAX(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN 3 PRECEDING "
      "AND CURRENT ROW) AS max_b, "
      "COUNT(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN 3 PRECEDING "
      "AND CURRENT ROW) AS cnt_b "
      "FROM t";
  auto create = harness.query(
      "SELECT * FROM dbsp_create_view('w_nullframe', '" + sql + "')");
  INFO("create error: " << (create->HasError() ? create->GetError() : "none"));
  REQUIRE_FALSE(create->HasError());

  requireViewMatchesQuery(harness, "w_nullframe", sql);
}

// Same defect class on the UNBOUNDED PRECEDING (cumulative) frame, which
// reuses the identical aggregate-accumulation code path -- the report that
// found this bug flagged the unbounded path as untested and possibly
// affected too (it predates today's bounded-frame fix). grp=1 is all-NULL
// throughout (cumulative SUM/AVG must stay NULL at every row). grp=2 has a
// NULL prefix (tidx 0..2) followed by non-null values -- the cumulative
// frame is all-NULL for tidx 0..2 and must flip to non-NULL exactly at
// tidx=3, the transition stock DuckDB's own running accumulator defines.
TEST_CASE("Window UNBOUNDED PRECEDING frame: all-NULL prefix returns NULL "
          "not 0",
          "[integration][window][null-frame]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER, v DOUBLE)");
  for (int i = 0; i < 8; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", NULL)");
    std::string v2 = (i < 3) ? "NULL" : (std::to_string(i) + ".0");
    harness.exec("INSERT INTO t VALUES (2, " + std::to_string(i) + ", " + v2 +
                 ")");
  }
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  const std::string sql =
      "SELECT grp, tidx, "
      "SUM(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN UNBOUNDED "
      "PRECEDING AND CURRENT ROW) AS sum_c, "
      "AVG(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN UNBOUNDED "
      "PRECEDING AND CURRENT ROW) AS avg_c, "
      "MIN(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN UNBOUNDED "
      "PRECEDING AND CURRENT ROW) AS min_c, "
      "MAX(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN UNBOUNDED "
      "PRECEDING AND CURRENT ROW) AS max_c, "
      "COUNT(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN UNBOUNDED "
      "PRECEDING AND CURRENT ROW) AS cnt_c "
      "FROM t";
  auto create = harness.query(
      "SELECT * FROM dbsp_create_view('w_nullcum', '" + sql + "')");
  INFO("create error: " << (create->HasError() ? create->GetError() : "none"));
  REQUIRE_FALSE(create->HasError());

  requireViewMatchesQuery(harness, "w_nullcum", sql);
}

// Post-edit: a pure value UPDATE (row count unchanged) that turns a
// previously-populated bounded frame all-NULL, and the reverse edit that
// turns it back. Row count is unchanged both times, so this stays on
// NativeWindowView's fast re-emit path (emit_affected/affected_indices) --
// render_row is the function the fix touches, and this is the only test in
// the file that exercises the fix through that path rather than the
// structural full-render path used at initial load.
TEST_CASE("Window bounded ROWS frame: edit into and out of an all-NULL "
          "frame (fast path)",
          "[integration][window][null-frame]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER, v DOUBLE)");
  for (int i = 0; i < 8; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", " +
                 std::to_string(i) + ".0)");
  }
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  const std::string sql =
      "SELECT grp, tidx, "
      "SUM(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN 2 PRECEDING "
      "AND CURRENT ROW) AS sum_b, "
      "AVG(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN 2 PRECEDING "
      "AND CURRENT ROW) AS avg_b, "
      "MIN(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN 2 PRECEDING "
      "AND CURRENT ROW) AS min_b, "
      "MAX(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN 2 PRECEDING "
      "AND CURRENT ROW) AS max_b, "
      "COUNT(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN 2 PRECEDING "
      "AND CURRENT ROW) AS cnt_b "
      "FROM t";
  auto create = harness.query(
      "SELECT * FROM dbsp_create_view('w_nulledit', '" + sql + "')");
  REQUIRE_FALSE(create->HasError());
  requireViewMatchesQuery(harness, "w_nulledit", sql);

  // Blank tidx 0,1,2: tidx=2's trailing 3-row frame (0,1,2) becomes
  // all-NULL. Row count unchanged -> fast path.
  harness.exec("UPDATE t SET v = NULL WHERE tidx IN (0, 1, 2)");
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_nulledit", sql);

  // Reverse: un-blank tidx=1 so tidx=2's frame has one non-null value again
  // -- the all-NULL frame must go back to a real number, not stay stuck at
  // whatever the fix emits for the NULL case.
  harness.exec("UPDATE t SET v = 99.0 WHERE tidx = 1");
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_nulledit", sql);
}

// Same edit-path defect on the UNBOUNDED PRECEDING (cumulative) frame.
TEST_CASE("Window UNBOUNDED PRECEDING frame: edit into and out of an "
          "all-NULL frame (fast path)",
          "[integration][window][null-frame]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER, v DOUBLE)");
  for (int i = 0; i < 8; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", " +
                 std::to_string(i) + ".0)");
  }
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  const std::string sql =
      "SELECT grp, tidx, "
      "SUM(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN UNBOUNDED "
      "PRECEDING AND CURRENT ROW) AS sum_c, "
      "AVG(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN UNBOUNDED "
      "PRECEDING AND CURRENT ROW) AS avg_c, "
      "MIN(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN UNBOUNDED "
      "PRECEDING AND CURRENT ROW) AS min_c, "
      "MAX(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN UNBOUNDED "
      "PRECEDING AND CURRENT ROW) AS max_c, "
      "COUNT(v) OVER (PARTITION BY grp ORDER BY tidx ROWS BETWEEN UNBOUNDED "
      "PRECEDING AND CURRENT ROW) AS cnt_c "
      "FROM t";
  auto create = harness.query(
      "SELECT * FROM dbsp_create_view('w_nullcumedit', '" + sql + "')");
  REQUIRE_FALSE(create->HasError());
  requireViewMatchesQuery(harness, "w_nullcumedit", sql);

  // Blank tidx 0,1,2: the cumulative frame is all-NULL for those rows (and
  // only those rows -- tidx=3 onward still has non-null values entering the
  // running frame). Row count unchanged -> fast path.
  harness.exec("UPDATE t SET v = NULL WHERE tidx IN (0, 1, 2)");
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_nullcumedit", sql);

  // Reverse: un-blank tidx=0 so the cumulative frame has a real value from
  // the start again.
  harness.exec("UPDATE t SET v = 99.0 WHERE tidx = 0");
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_nullcumedit", sql);
}

TEST_CASE("NTH_VALUE constant N stays gated (scope boundary)",
          "[integration][window][constant-frame]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER, v DOUBLE)");
  for (int i = 0; i < 10; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ", " +
                 std::to_string(i) + ".0)");
  }
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  auto nth_value = harness.query(
      "SELECT * FROM dbsp_create_view('w_nth', "
      "'SELECT grp, tidx, NTH_VALUE(v, 3) OVER (PARTITION BY grp ORDER BY "
      "tidx ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS n "
      "FROM t')");
  REQUIRE(nth_value->HasError());
  REQUIRE(nth_value->GetError().find("non-constant N") != std::string::npos);
}

// NTILE(n) differential vs stock DuckDB, covering every bucket-boundary
// regime the fixed algorithm (dbsp_window_view.hpp) has to get right, all
// in one partitioned query so a single dbsp_query/stock comparison covers
// them: grp=1 (9 rows, NTILE(3)) is the p % n == 0 case (3 rows/bucket
// evenly); grp=2 (8 rows, NTILE(3)) is p % n != 0 (buckets of 3, 3, 2 --
// the first p%n buckets get the extra row); grp=3 (2 rows, NTILE(3)) is
// n > p (more buckets than rows -- stock assigns buckets 1..p one row each
// and buckets p+1..n never appear); grp=4 (1 row, NTILE(3)) is a
// single-row partition, also n > p. The pre-fix formula
// (floor(row_idx*n/p)+1) only diverges from stock in the n > p cases
// (grp=3, grp=4), which is why the old gated-scope test above only needed
// two groups; this test's grp=3/grp=4 are the ones that actually exercise
// the bug this fix addresses.
TEST_CASE("NTILE bucket assignment differential vs stock DuckDB",
          "[integration][window][ntile]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER)");
  for (int i = 0; i < 9; i++) {
    harness.exec("INSERT INTO t VALUES (1, " + std::to_string(i) + ")");
  }
  for (int i = 0; i < 8; i++) {
    harness.exec("INSERT INTO t VALUES (2, " + std::to_string(i) + ")");
  }
  for (int i = 0; i < 2; i++) {
    harness.exec("INSERT INTO t VALUES (3, " + std::to_string(i) + ")");
  }
  harness.exec("INSERT INTO t VALUES (4, 0)");
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  const std::string sql = "SELECT grp, tidx, NTILE(3) OVER (PARTITION BY "
                          "grp ORDER BY tidx) AS b FROM t";
  auto create =
      harness.query("SELECT * FROM dbsp_create_view('w_ntile', '" + sql + "')");
  INFO("create error: " << (create->HasError() ? create->GetError() : "none"));
  REQUIRE_FALSE(create->HasError());

  requireViewMatchesQuery(harness, "w_ntile", sql);
}

// Post-edit re-render: NTILE has no fast-path rule (affected_indices()
// only knows LAG/LEAD/SUM/COUNT/AVG/MIN/MAX), so every edit to an
// NTILE-bearing partition -- structural or not -- goes through the
// full-partition re-render loop in apply_changes() (the second of the two
// call sites fixed by this change), not render_row()'s fast-path copy.
// This walks one partition's row count p through the n > p / p == n /
// p % n != 0 regimes via INSERT and DELETE, re-checking against stock
// after each edit.
TEST_CASE("NTILE bucket assignment differential vs stock after "
          "INSERT/DELETE change partition size",
          "[integration][window][ntile]") {
  DuckDBTestHarness harness;
  harness.exec("CREATE TABLE t (grp INTEGER, tidx INTEGER)");
  // Start at p=2, n=5 (n > p): buckets 1,2 get one row each, 3-5 empty.
  harness.exec("INSERT INTO t VALUES (1, 0), (1, 1)");
  harness.exec("SELECT * FROM dbsp_track('t')");
  harness.exec("SELECT * FROM dbsp_sync('t')");

  const std::string sql =
      "SELECT grp, tidx, NTILE(5) OVER (PARTITION BY grp ORDER BY tidx) AS "
      "b FROM t";
  auto create =
      harness.query("SELECT * FROM dbsp_create_view('w_ntile_edit', '" +
                    sql + "')");
  REQUIRE_FALSE(create->HasError());
  requireViewMatchesQuery(harness, "w_ntile_edit", sql);

  // INSERT to p=5, n=5 (p == n): every bucket gets exactly one row.
  harness.exec("INSERT INTO t VALUES (1, 2), (1, 3), (1, 4)");
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_ntile_edit", sql);

  // DELETE back to p=3, n=5 (n > p again, different remainder than the
  // p=2 start): buckets 1-3 get one row each, 4-5 empty.
  harness.exec("DELETE FROM t WHERE tidx IN (3, 4)");
  harness.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(harness, "w_ntile_edit", sql);
}

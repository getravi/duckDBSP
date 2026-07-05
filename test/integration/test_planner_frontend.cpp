// Phase B1: planner frontend differential tests
//
// Views created with dbsp_use_planner(true) translate through DuckDB's
// binder/planner (dbsp_plan_translator.hpp). Differential harness: after
// every delta batch, the incrementally maintained view result must equal
// DuckDB's own answer for the view SQL, across randomized insert/delete
// sequences. Unsupported plans must fall back to the bespoke parser.

#include "../test_helpers.hpp"
#include "catch.hpp"

#include <random>

using namespace dbsp_test;

namespace {

// Compare view content against DuckDB's direct answer for the view SQL.
// Both sides sorted; values compared as strings.
void requireViewMatchesQuery(DuckDBTestHarness &db, const std::string &view,
                             const std::string &sql) {
  auto expected = db.query("SELECT * FROM (" + sql + ") ORDER BY ALL");
  auto actual =
      db.query("SELECT * FROM dbsp_query('" + view + "') ORDER BY ALL");
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

// Randomized insert/delete rounds against table `t`, syncing and diffing
// the view after every round.
void runDifferential(DuckDBTestHarness &db, const std::string &view,
                     const std::string &sql, unsigned seed) {
  std::mt19937 rng(seed);
  int next_id = 1000;
  std::vector<int> live;

  for (int round = 0; round < 15; round++) {
    int inserts = static_cast<int>(rng() % 5) + 1;
    for (int i = 0; i < inserts; i++) {
      int id = next_id++;
      int val = static_cast<int>(rng() % 100);
      char tag = static_cast<char>('a' + rng() % 3);
      db.exec("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
              std::to_string(val) + ", '" + std::string(1, tag) + "')");
      live.push_back(id);
    }
    int deletes = live.empty() ? 0 : static_cast<int>(rng() % 3);
    for (int i = 0; i < deletes && !live.empty(); i++) {
      size_t idx = rng() % live.size();
      db.exec("DELETE FROM t WHERE id = " + std::to_string(live[idx]));
      live.erase(live.begin() + static_cast<long>(idx));
    }
    db.exec("SELECT * FROM dbsp_sync('t')");
    requireViewMatchesQuery(db, view, sql);
  }
}

void setupTable(DuckDBTestHarness &db) {
  db.createTable("t", "id INT, val INT, tag VARCHAR",
                 {"(1, 10, 'a')", "(2, 60, 'b')", "(3, 90, 'a')"});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_sync('t')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");
}

} // namespace

TEST_CASE("dbsp_use_planner toggles and reports status",
          "[integration][planner]") {
  DuckDBTestHarness db;

  auto status = db.query("SELECT * FROM dbsp_use_planner()");
  REQUIRE(status->GetValue(0, 0).ToString().find("DISABLED") !=
          std::string::npos);

  auto on = db.query("SELECT * FROM dbsp_use_planner(true)");
  REQUIRE(on->GetValue(0, 0).ToString().find("ENABLED") != std::string::npos);

  status = db.query("SELECT * FROM dbsp_use_planner()");
  REQUIRE(status->GetValue(0, 0).ToString().find("ENABLED") !=
          std::string::npos);

  auto off = db.query("SELECT * FROM dbsp_use_planner(false)");
  REQUIRE(off->GetValue(0, 0).ToString().find("DISABLED") !=
          std::string::npos);
}

TEST_CASE("planner frontend: filter view differential",
          "[integration][planner]") {
  DuckDBTestHarness db;
  setupTable(db);

  const std::string sql = "SELECT * FROM t WHERE val > 50";
  db.exec("SELECT * FROM dbsp_create_view('v_filter', '" + sql + "')");
  requireViewMatchesQuery(db, "v_filter", sql);
  runDifferential(db, "v_filter", sql, 42);
}

TEST_CASE("planner frontend: projection with expressions",
          "[integration][planner]") {
  DuckDBTestHarness db;
  setupTable(db);

  const std::string sql = "SELECT id, val * 2 + 1 AS v2 FROM t";
  db.exec("SELECT * FROM dbsp_create_view('v_proj', '" + sql + "')");
  requireViewMatchesQuery(db, "v_proj", sql);
  runDifferential(db, "v_proj", sql, 7);
}

TEST_CASE("planner frontend: filter + projection with function calls",
          "[integration][planner]") {
  DuckDBTestHarness db;
  setupTable(db);

  const std::string sql =
      "SELECT id, upper(tag) AS tag_u FROM t WHERE val >= 20 AND val <= 80";
  db.exec("SELECT * FROM dbsp_create_view('v_fp', '" + sql + "')");
  requireViewMatchesQuery(db, "v_fp", sql);
  runDifferential(db, "v_fp", sql, 123);
}

TEST_CASE("planner frontend: mixed AND/OR predicate the parser rejects",
          "[integration][planner]") {
  DuckDBTestHarness db;
  setupTable(db);

  const std::string sql =
      "SELECT * FROM t WHERE (val > 50 AND tag = ''a'') OR val < 10";
  const std::string plain_sql =
      "SELECT * FROM t WHERE (val > 50 AND tag = 'a') OR val < 10";
  db.exec("SELECT * FROM dbsp_create_view('v_or', '" + sql + "')");
  requireViewMatchesQuery(db, "v_or", plain_sql);
  runDifferential(db, "v_or", plain_sql, 99);
}

TEST_CASE("planner frontend: unsupported plan falls back to parser",
          "[integration][planner]") {
  DuckDBTestHarness db;
  setupTable(db);

  // Aggregation is not translated in B1; must fall back and still work
  const std::string sql = "SELECT tag, SUM(val) FROM t GROUP BY tag";
  auto result =
      db.query("SELECT * FROM dbsp_create_view('v_agg', '" + sql + "')");
  REQUIRE_FALSE(result->HasError());

  auto rows = db.getViewRows("v_agg");
  REQUIRE(rows.size() == 2); // tags 'a' and 'b'
}

TEST_CASE("planner frontend: flag off uses parser path",
          "[integration][planner]") {
  DuckDBTestHarness db;
  db.createTable("t", "id INT, val INT, tag VARCHAR", {"(1, 10, 'a')"});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_sync('t')");
  // Planner stays disabled (default)

  db.exec("SELECT * FROM dbsp_create_view('v_off', "
          "'SELECT * FROM t WHERE val > 5')");
  db.assertViewRowCount("v_off", 1);
}

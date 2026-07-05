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
      // ~10% NULL vals: aggregates and predicates must ignore them
      std::string val =
          rng() % 10 == 0 ? "NULL" : std::to_string(rng() % 100);
      char tag = static_cast<char>('a' + rng() % 3);
      db.exec("INSERT INTO t VALUES (" + std::to_string(id) + ", " + val +
              ", '" + std::string(1, tag) + "')");
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

// Second table for join/set-op tests
void setupTableU(DuckDBTestHarness &db) {
  db.createTable("u", "id INT, val INT, tag VARCHAR",
                 {"(1, 5, 'a')", "(2, 70, 'c')", "(4, 30, 'b')"});
  db.exec("SELECT * FROM dbsp_track('u')");
  db.exec("SELECT * FROM dbsp_sync('u')");
}

// Randomized rounds mutating BOTH t and u, diffing after every round
void runDifferentialTwoTables(DuckDBTestHarness &db, const std::string &view,
                              const std::string &sql, unsigned seed) {
  std::mt19937 rng(seed);
  int next_id = 1000;
  std::vector<std::pair<std::string, int>> live; // (table, id)

  for (int round = 0; round < 12; round++) {
    int inserts = static_cast<int>(rng() % 5) + 1;
    for (int i = 0; i < inserts; i++) {
      std::string table = rng() % 2 ? "t" : "u";
      int id = rng() % 8; // small id space so joins actually match
      std::string val =
          rng() % 10 == 0 ? "NULL" : std::to_string(rng() % 100);
      char tag = static_cast<char>('a' + rng() % 3);
      db.exec("INSERT INTO " + table + " VALUES (" + std::to_string(id) +
              ", " + val + ", '" + std::string(1, tag) + "')");
      live.push_back({table, id});
      (void)next_id;
    }
    int deletes = live.empty() ? 0 : static_cast<int>(rng() % 3);
    for (int i = 0; i < deletes && !live.empty(); i++) {
      size_t idx = rng() % live.size();
      // Delete ONE matching row (ids repeat by design)
      db.exec("DELETE FROM " + live[idx].first + " WHERE rowid = (SELECT "
              "rowid FROM " + live[idx].first + " WHERE id = " +
              std::to_string(live[idx].second) + " LIMIT 1)");
      live.erase(live.begin() + static_cast<long>(idx));
    }
    db.exec("SELECT * FROM dbsp_sync('t')");
    db.exec("SELECT * FROM dbsp_sync('u')");
    requireViewMatchesQuery(db, view, sql);
  }
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

TEST_CASE("planner frontend: multi-aggregate GROUP BY differential",
          "[integration][planner][aggregate]") {
  DuckDBTestHarness db;
  setupTable(db);

  const std::string sql = "SELECT tag, COUNT(*), COUNT(val), SUM(val), "
                          "AVG(val), MIN(val), MAX(val) FROM t GROUP BY tag";
  db.exec("SELECT * FROM dbsp_create_view('v_multi_agg', '" + sql + "')");
  requireViewMatchesQuery(db, "v_multi_agg", sql);
  runDifferential(db, "v_multi_agg", sql, 11);
}

TEST_CASE("planner frontend: expression group keys differential",
          "[integration][planner][aggregate]") {
  DuckDBTestHarness db;
  setupTable(db);

  const std::string sql =
      "SELECT val % 3 AS bucket, SUM(val), COUNT(*) FROM t GROUP BY bucket";
  db.exec("SELECT * FROM dbsp_create_view('v_expr_key', '" + sql + "')");
  requireViewMatchesQuery(db, "v_expr_key", sql);
  runDifferential(db, "v_expr_key", sql, 23);
}

TEST_CASE("planner frontend: HAVING differential",
          "[integration][planner][aggregate]") {
  DuckDBTestHarness db;
  setupTable(db);

  const std::string sql = "SELECT tag, SUM(val) AS total FROM t "
                          "GROUP BY tag HAVING SUM(val) > 100";
  db.exec("SELECT * FROM dbsp_create_view('v_having', '" + sql + "')");
  requireViewMatchesQuery(db, "v_having", sql);
  runDifferential(db, "v_having", sql, 31);
}

TEST_CASE("planner frontend: global aggregate incl. empty table",
          "[integration][planner][aggregate]") {
  DuckDBTestHarness db;
  // Start from an EMPTY table: global aggregate must still show one row
  db.exec("CREATE TABLE t (id INT, val INT, tag VARCHAR)");
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_sync('t')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql = "SELECT COUNT(*), SUM(val), AVG(val) FROM t";
  db.exec("SELECT * FROM dbsp_create_view('v_global', '" + sql + "')");
  requireViewMatchesQuery(db, "v_global", sql); // 1 row: (0, NULL, NULL)

  runDifferential(db, "v_global", sql, 47);

  // Delete everything: still one row, back to (0, NULL, NULL)
  db.exec("DELETE FROM t");
  db.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(db, "v_global", sql);
}

TEST_CASE("planner frontend: inner equi-join differential",
          "[integration][planner][join]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql =
      "SELECT t.id, t.val, u.val FROM t JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_join', '" + sql + "')");
  requireViewMatchesQuery(db, "v_join", sql);
  runDifferentialTwoTables(db, "v_join", sql, 5);
}

TEST_CASE("planner frontend: join with residual predicate differential",
          "[integration][planner][join]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql =
      "SELECT t.id, t.val, u.val FROM t JOIN u ON t.id = u.id "
      "AND t.val > u.val";
  db.exec("SELECT * FROM dbsp_create_view('v_join_res', '" + sql + "')");
  requireViewMatchesQuery(db, "v_join_res", sql);
  runDifferentialTwoTables(db, "v_join_res", sql, 13);
}

TEST_CASE("planner frontend: join + aggregate differential",
          "[integration][planner][join][aggregate]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql =
      "SELECT t.tag, COUNT(*), SUM(u.val) FROM t JOIN u ON t.id = u.id "
      "GROUP BY t.tag";
  db.exec("SELECT * FROM dbsp_create_view('v_join_agg', '" + sql + "')");
  requireViewMatchesQuery(db, "v_join_agg", sql);
  runDifferentialTwoTables(db, "v_join_agg", sql, 17);
}

TEST_CASE("planner frontend: cross join differential",
          "[integration][planner][join]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql = "SELECT t.id, u.id FROM t, u";
  db.exec("SELECT * FROM dbsp_create_view('v_cross', '" + sql + "')");
  requireViewMatchesQuery(db, "v_cross", sql);
  runDifferentialTwoTables(db, "v_cross", sql, 19);
}

TEST_CASE("planner frontend: DISTINCT differential",
          "[integration][planner][distinct]") {
  DuckDBTestHarness db;
  setupTable(db);

  const std::string sql = "SELECT DISTINCT tag, val FROM t";
  db.exec("SELECT * FROM dbsp_create_view('v_distinct', '" + sql + "')");
  requireViewMatchesQuery(db, "v_distinct", sql);
  runDifferential(db, "v_distinct", sql, 29);
}

TEST_CASE("planner frontend: set operations differential",
          "[integration][planner][setop]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  struct Case {
    const char *view;
    const char *op;
    unsigned seed;
  };
  const Case cases[] = {
      {"v_union_all", "UNION ALL", 37},
      {"v_union", "UNION", 41},
      {"v_intersect", "INTERSECT", 43},
      {"v_except", "EXCEPT", 53},
  };
  for (const auto &c : cases) {
    DYNAMIC_SECTION(c.op) {
      const std::string sql = std::string("SELECT id, val FROM t ") + c.op +
                              " SELECT id, val FROM u";
      db.exec("SELECT * FROM dbsp_create_view('" + std::string(c.view) +
              "', '" + sql + "')");
      requireViewMatchesQuery(db, c.view, sql);
      runDifferentialTwoTables(db, c.view, sql, c.seed);
    }
  }
}

TEST_CASE("planner frontend: unsupported plan falls back to parser",
          "[integration][planner]") {
  DuckDBTestHarness db;
  setupTable(db);

  // Window functions are not translated (B4); must fall back to the
  // bespoke parser's window view and still work
  const std::string sql =
      "SELECT id, val, ROW_NUMBER() OVER (ORDER BY id) FROM t";
  auto result =
      db.query("SELECT * FROM dbsp_create_view('v_window', '" + sql + "')");
  REQUIRE_FALSE(result->HasError());
  auto rows = db.getViewRows("v_window");
  REQUIRE(rows.size() == 3);
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

// Phase B1: planner frontend differential tests
//
// Views created with dbsp_use_planner(true) translate through DuckDB's
// binder/planner (dbsp_plan_translator.hpp). Differential harness: after
// every delta batch, the incrementally maintained view result must equal
// DuckDB's own answer for the view SQL, across randomized insert/delete
// sequences. Unsupported plans must fall back to the bespoke parser.

#include "../test_helpers.hpp"
#include "catch.hpp"

#include <filesystem>
#include <fstream>
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

TEST_CASE("dbsp_use_planner stays callable and always reports ENABLED",
          "[integration][planner]") {
  DuckDBTestHarness db;

  // The planner is the only frontend since C5 (parser deleted): the toggle
  // is a backwards-compatible no-op — existing scripts calling it must not
  // break, and it must never claim a parser fallback exists
  for (const char *call : {"dbsp_use_planner()", "dbsp_use_planner(false)",
                           "dbsp_use_planner(true)"}) {
    auto status = db.query(std::string("SELECT * FROM ") + call);
    REQUIRE_FALSE(status->HasError());
    REQUIRE(status->GetValue(0, 0).ToString().find("ENABLED") !=
            std::string::npos);
  }
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

TEST_CASE("planner frontend: window functions differential",
          "[integration][planner][window]") {
  DuckDBTestHarness db;
  setupTable(db);

  // Unique ORDER BY key (id) keeps ROW_NUMBER/LAG deterministic
  struct Case {
    const char *view;
    const char *sql;
    unsigned seed;
  };
  const Case cases[] = {
      {"v_rownum",
       "SELECT id, val, ROW_NUMBER() OVER (PARTITION BY tag ORDER BY id) "
       "FROM t",
       61},
      {"v_rank",
       "SELECT id, tag, RANK() OVER (PARTITION BY tag ORDER BY id) FROM t",
       67},
      {"v_lag",
       "SELECT id, val, LAG(val) OVER (PARTITION BY tag ORDER BY id) FROM t",
       71},
  };
  for (const auto &c : cases) {
    DYNAMIC_SECTION(c.view) {
      db.exec("SELECT * FROM dbsp_create_view('" + std::string(c.view) +
              "', '" + c.sql + "')");
      requireViewMatchesQuery(db, c.view, c.sql);
      runDifferential(db, c.view, c.sql, c.seed);
    }
  }
}

TEST_CASE("planner frontend: CTE differential",
          "[integration][planner][cte]") {
  DuckDBTestHarness db;
  setupTable(db);

  SECTION("single reference") {
    const std::string sql =
        "WITH big AS (SELECT id, val FROM t WHERE val > 20) "
        "SELECT * FROM big WHERE id % 2 = 0";
    db.exec("SELECT * FROM dbsp_create_view('v_cte', '" + sql + "')");
    requireViewMatchesQuery(db, "v_cte", sql);
    runDifferential(db, "v_cte", sql, 73);
  }

  SECTION("referenced twice (self-join through the CTE)") {
    const std::string sql =
        "WITH big AS (SELECT id, val FROM t WHERE val > 20) "
        "SELECT b1.id, b1.val, b2.val FROM big b1 JOIN big b2 "
        "ON b1.id = b2.id";
    db.exec("SELECT * FROM dbsp_create_view('v_cte2', '" + sql + "')");
    requireViewMatchesQuery(db, "v_cte2", sql);
    runDifferential(db, "v_cte2", sql, 79);
  }
}

TEST_CASE("planner frontend: self-correlated subquery and table-less recursion",
          "[integration][planner]") {
  DuckDBTestHarness db;
  setupTable(db);

  // Self-correlated subquery (DELIM_JOIN over the same table): supported
  // since E2 — must create and match DuckDB's answer
  const std::string corr_sql =
      "SELECT * FROM t a WHERE val > (SELECT AVG(val) FROM t b "
      "WHERE b.tag = a.tag)";
  auto corr = db.query(
      "SELECT * FROM dbsp_create_view('v_corr', "
      "'SELECT * FROM t a WHERE val > (SELECT AVG(val) FROM t b "
      "WHERE b.tag = a.tag)')");
  INFO("corr error: " << (corr->HasError() ? corr->GetError() : "none"));
  REQUIRE_FALSE(corr->HasError());
  requireViewMatchesQuery(db, "v_corr", corr_sql);

  // Recursive CTE: planner rejects, parser path handles it
  auto rec = db.query(
      "SELECT * FROM dbsp_create_view('v_rec', "
      "'WITH RECURSIVE r AS (SELECT 1 AS n UNION ALL SELECT n+1 FROM r "
      "WHERE n < 5) SELECT * FROM r')");
  if (rec->HasError()) {
    INFO("recursive fallback error: " << rec->GetError());
  }
  // Parser path may or may not support this exact shape; must not crash.
  // If it succeeded, the view must be queryable.
  if (!rec->HasError()) {
    auto rows = db.query("SELECT * FROM dbsp_query('v_rec')");
    REQUIRE_FALSE(rows->HasError());
  }
}

TEST_CASE("planner frontend: dbsp_use_planner(false) no longer disables it",
          "[integration][planner]") {
  DuckDBTestHarness db;
  db.createTable("t", "id INT, val INT, tag VARCHAR", {"(1, 10, 'a')"});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_sync('t')");
  // No parser to fall back to since C5: the old disable call is a no-op and
  // view creation must keep working through the planner
  db.exec("SELECT * FROM dbsp_use_planner(false)");

  db.exec("SELECT * FROM dbsp_create_view('v_off', "
          "'SELECT * FROM t WHERE val > 5')");
  db.assertViewRowCount("v_off", 1);
}

// ===== Phase C1: ORDER BY / LIMIT through the planner =====

namespace {

// True when the view was built by the planner frontend (no parser fallback)
bool plannerBuilt(DuckDBTestHarness &db, const std::string &view) {
  const auto *v = db.manager().get_view(view);
  return dynamic_cast<const dbsp_native::PlannedCircuitView *>(v) != nullptr;
}

// Rows of dbsp_query in scan order (first column only, as int64)
std::vector<int64_t> scanColumn0(DuckDBTestHarness &db,
                                 const std::string &view) {
  auto result = db.query("SELECT * FROM dbsp_query('" + view + "')");
  REQUIRE_FALSE(result->HasError());
  std::vector<int64_t> out;
  for (size_t r = 0; r < result->RowCount(); r++) {
    out.push_back(result->GetValue(0, r).GetValue<int64_t>());
  }
  return out;
}

} // namespace

TEST_CASE("planner C1: ORDER BY view scans in sorted order",
          "[integration][planner][sort]") {
  DuckDBTestHarness db;
  db.createTable("st", "id INT, val INT", {"(1, 30)", "(2, 10)", "(3, 20)"});
  db.exec("SELECT * FROM dbsp_track('st')");
  db.exec("SELECT * FROM dbsp_sync('st')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql = "SELECT val, id FROM st ORDER BY val DESC";
  db.exec("SELECT * FROM dbsp_create_view('v_sorted', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_sorted"));

  REQUIRE(scanColumn0(db, "v_sorted") == std::vector<int64_t>{30, 20, 10});

  db.exec("INSERT INTO st VALUES (4, 25)");
  db.exec("SELECT * FROM dbsp_sync('st')");
  REQUIRE(scanColumn0(db, "v_sorted") ==
          std::vector<int64_t>{30, 25, 20, 10});

  db.exec("DELETE FROM st WHERE id = 1");
  db.exec("SELECT * FROM dbsp_sync('st')");
  REQUIRE(scanColumn0(db, "v_sorted") == std::vector<int64_t>{25, 20, 10});
}

TEST_CASE("planner C1: ORDER BY on column dropped from output",
          "[integration][planner][sort]") {
  DuckDBTestHarness db;
  db.createTable("st2", "id INT, val INT", {"(1, 30)", "(2, 10)", "(3, 20)"});
  db.exec("SELECT * FROM dbsp_track('st2')");
  db.exec("SELECT * FROM dbsp_sync('st2')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  // val is the sort key but NOT in the SELECT list: the trailing projection
  // must fold into the sort view so scan order still follows val
  const std::string sql = "SELECT id FROM st2 ORDER BY val DESC";
  db.exec("SELECT * FROM dbsp_create_view('v_sorted_drop', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_sorted_drop"));
  REQUIRE(scanColumn0(db, "v_sorted_drop") == std::vector<int64_t>{1, 3, 2});
}

TEST_CASE("planner C1: LIMIT with ORDER BY maintains top-k incrementally",
          "[integration][planner][limit]") {
  DuckDBTestHarness db;
  db.createTable("lt", "id INT, val INT", {"(1, 30)", "(2, 10)", "(3, 20)"});
  db.exec("SELECT * FROM dbsp_track('lt')");
  db.exec("SELECT * FROM dbsp_sync('lt')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql = "SELECT id FROM lt ORDER BY val DESC, id LIMIT 2";
  db.exec("SELECT * FROM dbsp_create_view('v_top2', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_top2"));
  REQUIRE(scanColumn0(db, "v_top2") == std::vector<int64_t>{1, 3}); // 30, 20

  // New max displaces the tail
  db.exec("INSERT INTO lt VALUES (4, 40)");
  db.exec("SELECT * FROM dbsp_sync('lt')");
  REQUIRE(scanColumn0(db, "v_top2") == std::vector<int64_t>{4, 1}); // 40, 30

  // Deleting the max re-admits the displaced row
  db.exec("DELETE FROM lt WHERE id = 4");
  db.exec("SELECT * FROM dbsp_sync('lt')");
  REQUIRE(scanColumn0(db, "v_top2") == std::vector<int64_t>{1, 3}); // 30, 20
}

TEST_CASE("planner C1: bare LIMIT/OFFSET", "[integration][planner][limit]") {
  DuckDBTestHarness db;
  db.createTable("bt", "id INT",
                 {"(1)", "(2)", "(3)", "(4)", "(5)"});
  db.exec("SELECT * FROM dbsp_track('bt')");
  db.exec("SELECT * FROM dbsp_sync('bt')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql = "SELECT id FROM bt LIMIT 3 OFFSET 1";
  db.exec("SELECT * FROM dbsp_create_view('v_lim', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_lim"));
  db.assertViewRowCount("v_lim", 3);
}

TEST_CASE("planner C1: percentage LIMIT is not planner-built",
          "[integration][planner][limit]") {
  DuckDBTestHarness db;
  db.createTable("pt", "id INT", {"(1)", "(2)"});
  db.exec("SELECT * FROM dbsp_track('pt')");
  db.exec("SELECT * FROM dbsp_sync('pt')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  // Percentage LIMIT translates since Phase M3 (count tracks input
  // size); truly non-constant limits (expressions) still yield E110
  auto res = db.query("SELECT * FROM dbsp_create_view('v_pct', "
                      "'SELECT id FROM pt LIMIT 50%')");
  REQUIRE_FALSE(res->HasError());
  REQUIRE(plannerBuilt(db, "v_pct"));
  auto expected = db.query("SELECT COUNT(*) FROM (SELECT id FROM pt "
                           "LIMIT 50%)");
  auto actual = db.query("SELECT COUNT(*) FROM dbsp_query('v_pct')");
  REQUIRE(actual->GetValue(0, 0) == expected->GetValue(0, 0));
}

TEST_CASE("planner C1: ORDER BY + LIMIT differential",
          "[integration][planner][limit]") {
  DuckDBTestHarness db;
  setupTable(db);

  // Total order (val DESC, id) keeps the LIMIT subset deterministic on both
  // sides even with duplicate vals from the randomized rounds
  const std::string sql =
      "SELECT id, val FROM t ORDER BY val DESC, id LIMIT 5";
  db.exec("SELECT * FROM dbsp_create_view('v_topk', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_topk"));
  requireViewMatchesQuery(db, "v_topk", sql);
  runDifferential(db, "v_topk", sql, 71);
}

// ===== Phase C2: recursive CTEs through the planner =====

TEST_CASE("planner C2: recursive CTE UNION ALL",
          "[integration][planner][recursive]") {
  DuckDBTestHarness db;
  db.createTable("seed", "id INT", {"(1)"});
  db.exec("SELECT * FROM dbsp_track('seed')");
  db.exec("SELECT * FROM dbsp_sync('seed')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql =
      "WITH RECURSIVE r AS (SELECT id FROM seed UNION ALL "
      "SELECT id+1 FROM r WHERE id < 5) SELECT * FROM r";
  db.exec("SELECT * FROM dbsp_create_view('v_rec_all', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_rec_all"));
  db.assertViewRowCount("v_rec_all", 5); // 1..5

  db.exec("INSERT INTO seed VALUES (4)");
  db.exec("SELECT * FROM dbsp_sync('seed')");
  db.assertViewRowCount("v_rec_all", 7); // + {4,5}
}

TEST_CASE("planner C2: recursive CTE UNION dedups across deltas",
          "[integration][planner][recursive]") {
  DuckDBTestHarness db;
  db.createTable("seed2", "id INT", {"(1)"});
  db.exec("SELECT * FROM dbsp_track('seed2')");
  db.exec("SELECT * FROM dbsp_sync('seed2')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql =
      "WITH RECURSIVE r AS (SELECT id FROM seed2 UNION "
      "SELECT id+1 FROM r WHERE id < 5) SELECT * FROM r";
  db.exec("SELECT * FROM dbsp_create_view('v_rec_u', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_rec_u"));
  db.assertViewRowCount("v_rec_u", 5); // 1..5

  // 3,4,5 are already reachable: UNION must not double-count them even
  // though they arrive in a LATER delta than the original fixed point
  db.exec("INSERT INTO seed2 VALUES (3)");
  db.exec("SELECT * FROM dbsp_sync('seed2')");
  db.assertViewRowCount("v_rec_u", 5);
}

TEST_CASE("planner C2: recursive CTE joining a second table",
          "[integration][planner][recursive]") {
  DuckDBTestHarness db;
  // Transitive closure over an edge table — the recursive step JOINs a base
  // table, which the parser path never supported (single-source recursion)
  db.createTable("edges", "src INT, dst INT", {"(1, 2)", "(2, 3)", "(3, 4)"});
  db.exec("SELECT * FROM dbsp_track('edges')");
  db.exec("SELECT * FROM dbsp_sync('edges')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql =
      "WITH RECURSIVE reach AS (SELECT src, dst FROM edges WHERE src = 1 "
      "UNION SELECT r.src, e.dst FROM reach r JOIN edges e ON r.dst = e.src) "
      "SELECT * FROM reach";
  db.exec("SELECT * FROM dbsp_create_view('v_reach', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_reach"));
  db.assertViewRowCount("v_reach", 3); // (1,2) (1,3) (1,4)

  db.exec("INSERT INTO edges VALUES (4, 5)");
  db.exec("SELECT * FROM dbsp_sync('edges')");
  db.assertViewRowCount("v_reach", 4); // + (1,5)
}

// ===== Phase C3: DISTINCT ON through the planner =====

TEST_CASE("planner C3: DISTINCT ON keeps winner per key",
          "[integration][planner][distinct_on]") {
  DuckDBTestHarness db;
  db.createTable("dt", "id INT, val INT, tag VARCHAR",
                 {"(1, 10, 'a')", "(2, 30, 'a')", "(3, 20, 'b')"});
  db.exec("SELECT * FROM dbsp_track('dt')");
  db.exec("SELECT * FROM dbsp_sync('dt')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql = "SELECT DISTINCT ON (tag) id, val FROM dt "
                          "ORDER BY tag, val DESC";
  db.exec("SELECT * FROM dbsp_create_view('v_don', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_don"));

  // One winner per tag, scanned in tag order: 'a' -> id 2 (val 30 wins),
  // 'b' -> id 3
  REQUIRE(scanColumn0(db, "v_don") == std::vector<int64_t>{2, 3});

  // New max for 'a' displaces the old winner
  db.exec("INSERT INTO dt VALUES (4, 40, 'a')");
  db.exec("SELECT * FROM dbsp_sync('dt')");
  REQUIRE(scanColumn0(db, "v_don") == std::vector<int64_t>{4, 3});

  // Deleting the winner falls back to the runner-up
  db.exec("DELETE FROM dt WHERE id = 4");
  db.exec("SELECT * FROM dbsp_sync('dt')");
  REQUIRE(scanColumn0(db, "v_don") == std::vector<int64_t>{2, 3});

  // Deleting a whole partition removes its row
  db.exec("DELETE FROM dt WHERE tag = 'b'");
  db.exec("SELECT * FROM dbsp_sync('dt')");
  REQUIRE(scanColumn0(db, "v_don") == std::vector<int64_t>{2});
}

TEST_CASE("planner C3: DISTINCT ON differential",
          "[integration][planner][distinct_on]") {
  DuckDBTestHarness db;
  setupTable(db);

  // Deterministic winner per tag: highest val, id as tiebreak
  const std::string sql = "SELECT DISTINCT ON (tag) tag, val, id FROM t "
                          "ORDER BY tag, val DESC, id";
  db.exec("SELECT * FROM dbsp_create_view('v_don_diff', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_don_diff"));
  requireViewMatchesQuery(db, "v_don_diff", sql);
  runDifferential(db, "v_don_diff", sql, 83);
}

// ===== Phase C4: circuit-IR optimizer =====

namespace {

size_t nodeCount(DuckDBTestHarness &db, const std::string &view) {
  const auto *v = dynamic_cast<const dbsp_native::PlannedCircuitView *>(
      db.manager().get_view(view));
  REQUIRE(v != nullptr);
  return v->node_count();
}

} // namespace

TEST_CASE("planner C4: filter+project fuse into one node",
          "[integration][planner][ir_opt]") {
  DuckDBTestHarness db;
  db.createTable("ot", "id INT, val INT", {"(1, 5)", "(2, 15)"});
  db.exec("SELECT * FROM dbsp_track('ot')");
  db.exec("SELECT * FROM dbsp_sync('ot')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql = "SELECT id FROM ot WHERE val > 10";
  db.exec("SELECT * FROM dbsp_create_view('v_fused', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_fused"));
  size_t fused = nodeCount(db, "v_fused");

  // Same view without the IR optimizer: one extra node (filter + map split)
  dbsp_native::g_plan_ir_optimize = false;
  db.exec("SELECT * FROM dbsp_create_view('v_raw', '" + sql + "')");
  dbsp_native::g_plan_ir_optimize = true;
  size_t raw = nodeCount(db, "v_raw");
  REQUIRE(fused < raw);

  // Identical behavior, incrementally too
  db.exec("INSERT INTO ot VALUES (3, 20), (4, 1)");
  db.exec("SELECT * FROM dbsp_sync('ot')");
  requireViewMatchesQuery(db, "v_fused", sql);
  requireViewMatchesQuery(db, "v_raw", sql);
}

TEST_CASE("planner C4: join filter pushdown keeps results exact",
          "[integration][planner][ir_opt]") {
  DuckDBTestHarness db;
  db.createTable("pl", "id INT, x INT", {"(1, 5)", "(2, 20)"});
  db.exec("SELECT * FROM dbsp_track('pl')");
  db.exec("SELECT * FROM dbsp_sync('pl')");
  db.createTable("pr", "id INT, y INT", {"(1, 7)", "(2, 2)"});
  db.exec("SELECT * FROM dbsp_track('pr')");
  db.exec("SELECT * FROM dbsp_sync('pr')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql =
      "SELECT pl.id FROM pl JOIN pr ON pl.id = pr.id "
      "WHERE pl.x > 10 AND pr.y < 5";
  db.exec("SELECT * FROM dbsp_create_view('v_pd', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_pd"));

  // Pushdown must actually fire: pushed-down plan has MORE nodes (two
  // filters below the join) than the unoptimized one (one filter above)
  size_t pushed = nodeCount(db, "v_pd");
  dbsp_native::g_plan_ir_optimize = false;
  db.exec("SELECT * FROM dbsp_create_view('v_pd_raw', '" + sql + "')");
  dbsp_native::g_plan_ir_optimize = true;
  REQUIRE(pushed != nodeCount(db, "v_pd_raw"));

  db.assertViewRowCount("v_pd", 1); // only id=2 passes both predicates

  db.exec("INSERT INTO pl VALUES (3, 30)");
  db.exec("INSERT INTO pr VALUES (3, 1)");
  db.exec("SELECT * FROM dbsp_sync('pl')");
  db.exec("SELECT * FROM dbsp_sync('pr')");
  requireViewMatchesQuery(db, "v_pd", sql);
  db.assertViewRowCount("v_pd", 2);
}

// ===== Recursive views: deletion propagation =====

TEST_CASE("planner recursive: deleting the seed retracts derived rows",
          "[integration][planner][recursive][rec_delete]") {
  DuckDBTestHarness db;
  db.createTable("rseed", "id INT", {"(1)", "(20)"});
  db.exec("SELECT * FROM dbsp_track('rseed')");
  db.exec("SELECT * FROM dbsp_sync('rseed')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql =
      "WITH RECURSIVE r AS (SELECT id FROM rseed UNION ALL "
      "SELECT id+1 FROM r WHERE id < 5) SELECT * FROM r";
  db.exec("SELECT * FROM dbsp_create_view('v_rdel', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_rdel"));
  db.assertViewRowCount("v_rdel", 6); // 1..5 from seed 1, plus 20

  // Deleting seed 1 must retract its whole derived chain
  db.exec("DELETE FROM rseed WHERE id = 1");
  db.exec("SELECT * FROM dbsp_sync('rseed')");
  db.assertViewRowCount("v_rdel", 1); // only 20 remains

  // And reinserting brings it back
  db.exec("INSERT INTO rseed VALUES (3)");
  db.exec("SELECT * FROM dbsp_sync('rseed')");
  db.assertViewRowCount("v_rdel", 4); // 3,4,5 + 20
}

TEST_CASE("planner recursive: removing an edge shrinks the closure",
          "[integration][planner][recursive][rec_delete]") {
  DuckDBTestHarness db;
  db.createTable("redges", "src INT, dst INT",
                 {"(1, 2)", "(2, 3)", "(3, 4)"});
  db.exec("SELECT * FROM dbsp_track('redges')");
  db.exec("SELECT * FROM dbsp_sync('redges')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql =
      "WITH RECURSIVE reach AS (SELECT src, dst FROM redges WHERE src = 1 "
      "UNION SELECT r.src, e.dst FROM reach r JOIN redges e ON r.dst = e.src) "
      "SELECT * FROM reach";
  db.exec("SELECT * FROM dbsp_create_view('v_rreach', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_rreach"));
  db.assertViewRowCount("v_rreach", 3); // (1,2)(1,3)(1,4)
  requireViewMatchesQuery(db, "v_rreach", sql);

  // Cutting 2->3 must retract everything reachable only through it
  db.exec("DELETE FROM redges WHERE src = 2 AND dst = 3");
  db.exec("SELECT * FROM dbsp_sync('redges')");
  db.assertViewRowCount("v_rreach", 1); // only (1,2)
  requireViewMatchesQuery(db, "v_rreach", sql);
}

// ===== Phase D2: outer joins =====

TEST_CASE("planner D2: LEFT JOIN pads and unpads incrementally",
          "[integration][planner][outer_join]") {
  DuckDBTestHarness db;
  db.createTable("ol", "id INT, x INT", {"(1, 10)", "(2, 20)"});
  db.exec("SELECT * FROM dbsp_track('ol')");
  db.exec("SELECT * FROM dbsp_sync('ol')");
  db.createTable("orr", "id INT, y INT", {"(1, 100)"});
  db.exec("SELECT * FROM dbsp_track('orr')");
  db.exec("SELECT * FROM dbsp_sync('orr')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql =
      "SELECT ol.id, ol.x, orr.y FROM ol LEFT JOIN orr ON ol.id = orr.id";
  db.exec("SELECT * FROM dbsp_create_view('v_lj', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_lj"));
  requireViewMatchesQuery(db, "v_lj", sql); // (1,10,100), (2,20,NULL)

  // First match for id=2: NULL pad must retract
  db.exec("INSERT INTO orr VALUES (2, 200)");
  db.exec("SELECT * FROM dbsp_sync('orr')");
  requireViewMatchesQuery(db, "v_lj", sql);

  // Last match for id=2 leaves: pad comes back
  db.exec("DELETE FROM orr WHERE id = 2");
  db.exec("SELECT * FROM dbsp_sync('orr')");
  requireViewMatchesQuery(db, "v_lj", sql);

  // Unmatched left row deleted: its pad goes away
  db.exec("DELETE FROM ol WHERE id = 2");
  db.exec("SELECT * FROM dbsp_sync('ol')");
  requireViewMatchesQuery(db, "v_lj", sql);

  // NULL join key on the left: LEFT JOIN must still emit it padded
  db.exec("INSERT INTO ol VALUES (NULL, 30)");
  db.exec("SELECT * FROM dbsp_sync('ol')");
  requireViewMatchesQuery(db, "v_lj", sql);
}

TEST_CASE("planner D2: LEFT JOIN differential",
          "[integration][planner][outer_join]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql =
      "SELECT t.id, t.val, u.val FROM t LEFT JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_ljd', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_ljd"));
  requireViewMatchesQuery(db, "v_ljd", sql);
  runDifferentialTwoTables(db, "v_ljd", sql, 611);
}

TEST_CASE("planner D2: RIGHT JOIN differential",
          "[integration][planner][outer_join]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql =
      "SELECT t.val, u.id, u.val FROM t RIGHT JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_rjd', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_rjd"));
  requireViewMatchesQuery(db, "v_rjd", sql);
  runDifferentialTwoTables(db, "v_rjd", sql, 613);
}

TEST_CASE("planner D2: FULL JOIN differential",
          "[integration][planner][outer_join]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql =
      "SELECT t.id, t.val, u.id, u.val FROM t FULL JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_fjd', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_fjd"));
  requireViewMatchesQuery(db, "v_fjd", sql);
  runDifferentialTwoTables(db, "v_fjd", sql, 617);
}

TEST_CASE("planner D2: LEFT JOIN with residual predicate differential",
          "[integration][planner][outer_join]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  // Residual (t.val < u.val) participates in matching: a right row with the
  // same key but failing the residual must NOT count as a match
  const std::string sql = "SELECT t.id, t.val, u.val FROM t LEFT JOIN u "
                          "ON t.id = u.id AND t.val < u.val";
  db.exec("SELECT * FROM dbsp_create_view('v_ljr', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_ljr"));
  requireViewMatchesQuery(db, "v_ljr", sql);
  runDifferentialTwoTables(db, "v_ljr", sql, 619);
}

// ===== Phase D3: MARK joins + FIRST (IN / NOT IN / scalar subqueries) =====

TEST_CASE("planner D3: IN subquery differential",
          "[integration][planner][subquery]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql = "SELECT id, val FROM t WHERE id IN (SELECT id FROM u)";
  db.exec("SELECT * FROM dbsp_create_view('v_in', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_in"));
  requireViewMatchesQuery(db, "v_in", sql);
  runDifferentialTwoTables(db, "v_in", sql, 701);
}

TEST_CASE("planner D3: NOT IN with NULLs is three-valued",
          "[integration][planner][subquery]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  // u.val goes NULL ~10% of rounds: NOT IN must yield NULL (row filtered)
  // for every left row once the subquery side contains a NULL
  const std::string sql =
      "SELECT id, val FROM t WHERE val NOT IN (SELECT val FROM u)";
  db.exec("SELECT * FROM dbsp_create_view('v_notin', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_notin"));
  requireViewMatchesQuery(db, "v_notin", sql);
  runDifferentialTwoTables(db, "v_notin", sql, 703);
}

TEST_CASE("planner D3: scalar subquery comparison differential",
          "[integration][planner][subquery]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql =
      "SELECT id, val FROM t WHERE val > (SELECT AVG(val) FROM u)";
  db.exec("SELECT * FROM dbsp_create_view('v_scalar', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_scalar"));
  requireViewMatchesQuery(db, "v_scalar", sql);
  runDifferentialTwoTables(db, "v_scalar", sql, 709);
}

TEST_CASE("planner D3: IN over emptied subquery flips marks in bulk",
          "[integration][planner][subquery]") {
  DuckDBTestHarness db;
  db.createTable("mt", "id INT", {"(1)", "(2)", "(3)"});
  db.exec("SELECT * FROM dbsp_track('mt')");
  db.exec("SELECT * FROM dbsp_sync('mt')");
  db.createTable("ms", "id INT", {"(2)"});
  db.exec("SELECT * FROM dbsp_track('ms')");
  db.exec("SELECT * FROM dbsp_sync('ms')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  const std::string sql = "SELECT id FROM mt WHERE id NOT IN (SELECT id FROM ms)";
  db.exec("SELECT * FROM dbsp_create_view('v_bulk', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_bulk"));
  db.assertViewRowCount("v_bulk", 2); // 1, 3

  // Emptying the subquery side: NOT IN over empty set = TRUE for all
  db.exec("DELETE FROM ms");
  db.exec("SELECT * FROM dbsp_sync('ms')");
  db.assertViewRowCount("v_bulk", 3);

  // First NULL arriving on the subquery side: NOT IN = NULL for all
  db.exec("INSERT INTO ms VALUES (NULL)");
  db.exec("SELECT * FROM dbsp_sync('ms')");
  db.assertViewRowCount("v_bulk", 0);
}

// ===== Phase E1: incremental view-on-view cascades =====

TEST_CASE("planner E1: three-level cascade differential",
          "[integration][planner][cascade]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  // base tables -> join view -> aggregate view -> sort/limit view.
  // Every level must stay equal to DuckDB's answer for its own SQL under
  // delete-heavy churn — this exercises delta propagation between views,
  // not just table-to-view.
  const std::string sql_j =
      "SELECT t.id AS tid, t.val AS tval, u.val AS uval, t.tag AS ttag "
      "FROM t LEFT JOIN u ON t.id = u.id";
  const std::string sql_a =
      "SELECT ttag, COUNT(*) AS n, SUM(tval) AS s FROM v_e1_j GROUP BY ttag";
  const std::string sql_a_direct =
      "SELECT ttag, COUNT(*) AS n, SUM(tval) AS s FROM (" + sql_j +
      ") GROUP BY ttag";
  const std::string sql_s = "SELECT ttag, s FROM v_e1_a ORDER BY s DESC, ttag";
  const std::string sql_s_direct =
      "SELECT ttag, s FROM (" + sql_a_direct + ") ORDER BY s DESC, ttag";

  db.exec("SELECT * FROM dbsp_create_view('v_e1_j', '" + sql_j + "')");
  db.exec("SELECT * FROM dbsp_create_view('v_e1_a', '" + sql_a + "')");
  db.exec("SELECT * FROM dbsp_create_view('v_e1_s', '" + sql_s + "')");
  REQUIRE(plannerBuilt(db, "v_e1_j"));
  REQUIRE(plannerBuilt(db, "v_e1_a"));
  REQUIRE(plannerBuilt(db, "v_e1_s"));

  std::mt19937 rng(811);
  std::vector<std::pair<std::string, int>> live;
  for (int round = 0; round < 20; round++) {
    int inserts = static_cast<int>(rng() % 5) + 1;
    for (int i = 0; i < inserts; i++) {
      std::string table = rng() % 2 ? "t" : "u";
      int id = static_cast<int>(rng() % 8);
      std::string val = rng() % 10 == 0 ? "NULL" : std::to_string(rng() % 60);
      char tag = static_cast<char>('a' + rng() % 3);
      db.exec("INSERT INTO " + table + " VALUES (" + std::to_string(id) +
              ", " + val + ", '" + std::string(1, tag) + "')");
      live.push_back({table, id});
    }
    int deletes = live.empty() ? 0 : static_cast<int>(rng() % 4);
    for (int i = 0; i < deletes && !live.empty(); i++) {
      size_t idx = rng() % live.size();
      db.exec("DELETE FROM " + live[idx].first + " WHERE rowid = (SELECT "
              "rowid FROM " + live[idx].first + " WHERE id = " +
              std::to_string(live[idx].second) + " LIMIT 1)");
      live.erase(live.begin() + static_cast<long>(idx));
    }
    db.exec("SELECT * FROM dbsp_sync('t')");
    db.exec("SELECT * FROM dbsp_sync('u')");
    requireViewMatchesQuery(db, "v_e1_j", sql_j);
    requireViewMatchesQuery(db, "v_e1_a", sql_a_direct);
    requireViewMatchesQuery(db, "v_e1_s", sql_s_direct);
  }
}

TEST_CASE("planner E1: diamond dependency applies both parent deltas",
          "[integration][planner][cascade]") {
  DuckDBTestHarness db;
  setupTable(db);

  // t -> v_hi and v_lo -> v_union(v_hi, v_lo): the union view receives
  // deltas from BOTH parents in one propagation round; missing either one
  // (e.g. a second apply clearing the first delta) corrupts the union
  const std::string sql_hi = "SELECT id, val FROM t WHERE val >= 50";
  const std::string sql_lo = "SELECT id, val FROM t WHERE val < 50";
  const std::string sql_u = "SELECT * FROM v_e1_hi UNION ALL SELECT * FROM v_e1_lo";
  const std::string sql_u_direct =
      "SELECT * FROM (" + sql_hi + ") UNION ALL SELECT * FROM (" + sql_lo + ")";

  db.exec("SELECT * FROM dbsp_create_view('v_e1_hi', '" + sql_hi + "')");
  db.exec("SELECT * FROM dbsp_create_view('v_e1_lo', '" + sql_lo + "')");
  db.exec("SELECT * FROM dbsp_create_view('v_e1_u', '" + sql_u + "')");
  REQUIRE(plannerBuilt(db, "v_e1_u"));
  requireViewMatchesQuery(db, "v_e1_u", sql_u_direct);

  // One insert lands in exactly one parent; a mixed batch lands in both
  db.exec("INSERT INTO t VALUES (100, 80, 'z'), (101, 10, 'z')");
  db.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(db, "v_e1_u", sql_u_direct);

  db.exec("DELETE FROM t WHERE id IN (100, 101)");
  db.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(db, "v_e1_u", sql_u_direct);
}

// ===== Phase E2: correlated subqueries (DELIM_JOIN) =====

TEST_CASE("planner E2: correlated scalar subquery differential",
          "[integration][planner][delim]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql =
      "SELECT id, val FROM t WHERE val > "
      "(SELECT AVG(val) FROM u WHERE u.id = t.id)";
  db.exec("SELECT * FROM dbsp_create_view('v_corr', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_corr"));
  requireViewMatchesQuery(db, "v_corr", sql);
  runDifferentialTwoTables(db, "v_corr", sql, 907);
}

TEST_CASE("planner E2: correlated EXISTS differential",
          "[integration][planner][delim]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql =
      "SELECT id, val FROM t WHERE EXISTS "
      "(SELECT 1 FROM u WHERE u.id = t.id AND u.val > t.val)";
  db.exec("SELECT * FROM dbsp_create_view('v_exists', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_exists"));
  requireViewMatchesQuery(db, "v_exists", sql);
  runDifferentialTwoTables(db, "v_exists", sql, 911);
}

TEST_CASE("planner E2: correlated NOT EXISTS differential",
          "[integration][planner][delim]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  const std::string sql =
      "SELECT id, val FROM t WHERE NOT EXISTS "
      "(SELECT 1 FROM u WHERE u.id = t.id)";
  db.exec("SELECT * FROM dbsp_create_view('v_nexists', '" + sql + "')");
  REQUIRE(plannerBuilt(db, "v_nexists"));
  requireViewMatchesQuery(db, "v_nexists", sql);
  runDifferentialTwoTables(db, "v_nexists", sql, 919);
}

// ---------------------------------------------------------------------------
// Phase I1: shared join arrangements — N views joining the same table reuse
// one CDC-maintained index instead of N private copies
// ---------------------------------------------------------------------------

TEST_CASE("planner I1: identical join sides share one arrangement",
          "[integration][planner][arrangement]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);
  auto &mgr = db.manager();
  const size_t base = mgr.shared_arrangement_count();

  // Identical SQL → identical plan → identical fingerprints: an inner
  // join shares BOTH sides (I1b), so two identical views hold the same
  // two arrangements (t side + u side)
  const std::string sql1 =
      "SELECT t.id, t.val, u.val FROM t JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_arr1', '" + sql1 + "')");
  db.exec("SELECT * FROM dbsp_create_view('v_arr2', '" + sql1 + "')");
  REQUIRE(mgr.shared_arrangement_count() == base + 2);

  // O4: DIFFERENT column needs still share — arrangements hold full
  // rows with canonical (full-table-space) key fingerprints; each
  // consumer projects bucket rows to its own shape at probe time
  const std::string sql3 =
      "SELECT t.tag, u.tag FROM t JOIN u ON t.id = u.id WHERE t.val > 20";
  db.exec("SELECT * FROM dbsp_create_view('v_arr3', '" + sql3 + "')");
  REQUIRE(mgr.shared_arrangement_count() == base + 2);

  requireViewMatchesQuery(db, "v_arr1", sql1);
  requireViewMatchesQuery(db, "v_arr2", sql1);
  requireViewMatchesQuery(db, "v_arr3", sql3);

  // All views stay correct across randomized updates of BOTH tables
  runDifferentialTwoTables(db, "v_arr1", sql1, 101);
  requireViewMatchesQuery(db, "v_arr2", sql1);
  requireViewMatchesQuery(db, "v_arr3", sql3);
}

TEST_CASE("planner I1: arrangement survives dropping one of two views",
          "[integration][planner][arrangement]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);
  auto &mgr = db.manager();
  const size_t base = mgr.shared_arrangement_count();

  const std::string sql =
      "SELECT t.id, u.val FROM t JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_keep', '" + sql + "')");
  db.exec("SELECT * FROM dbsp_create_view('v_drop', '" + sql + "')");
  REQUIRE(mgr.shared_arrangement_count() == base + 2);

  db.exec("SELECT dbsp_drop('v_drop')");
  // Survivor still owns both arrangements and stays correct
  REQUIRE(mgr.shared_arrangement_count() == base + 2);
  runDifferentialTwoTables(db, "v_keep", sql, 103);

  db.exec("SELECT dbsp_drop('v_keep')");
  REQUIRE(mgr.shared_arrangement_count() == base);
}

TEST_CASE("planner I1: LEFT join probing a shared right side",
          "[integration][planner][arrangement][outer]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  // Right side (u) is a pure probe target in a LEFT join — shareable;
  // pads for unmatched t rows must still appear/disappear correctly
  const std::string sql =
      "SELECT t.id, t.val, u.val FROM t LEFT JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_arr_left', '" + sql + "')");
  auto &mgr = db.manager();
  REQUIRE(mgr.shared_arrangement_count() >= 1);
  requireViewMatchesQuery(db, "v_arr_left", sql);
  runDifferentialTwoTables(db, "v_arr_left", sql, 107);
}

TEST_CASE("planner I1: NOT IN with shared right-side counters",
          "[integration][planner][arrangement][mark]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  // MARK join: right side shared, three-valued logic driven by the
  // arrangement's total/null counters (NULL u.val rows in the generator)
  const std::string sql =
      "SELECT id, val FROM t WHERE val NOT IN (SELECT val FROM u)";
  db.exec("SELECT * FROM dbsp_create_view('v_arr_mark', '" + sql + "')");
  requireViewMatchesQuery(db, "v_arr_mark", sql);
  runDifferentialTwoTables(db, "v_arr_mark", sql, 109);

  // Drain u completely: every mark flips through the empty-right category
  db.exec("DELETE FROM u");
  db.exec("SELECT * FROM dbsp_sync('u')");
  requireViewMatchesQuery(db, "v_arr_mark", sql);
}

TEST_CASE("planner I1: self-padding sides refuse to share",
          "[integration][planner][arrangement][outer]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);
  auto &mgr = db.manager();
  const size_t base = mgr.shared_arrangement_count();

  // FULL OUTER: both sides self-pad — no arrangement may be created
  // (sharing one would lose unmatched-row pads at init)
  const std::string sql =
      "SELECT t.id, t.val, u.val FROM t FULL JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_arr_full', '" + sql + "')");
  REQUIRE(mgr.shared_arrangement_count() == base);
  requireViewMatchesQuery(db, "v_arr_full", sql);
  runDifferentialTwoTables(db, "v_arr_full", sql, 113);
}

TEST_CASE("planner I1b: both-sides-shared join bootstraps from one replay",
          "[integration][planner][arrangement]") {
  DuckDBTestHarness db;
  // Both tables preloaded BEFORE view creation: init pushes only the
  // left replay against the backfilled right arrangement — output must
  // still be the complete join
  setupTable(db);
  setupTableU(db);
  db.exec("INSERT INTO t VALUES (4, 40, 'c'), (5, 55, 'b')");
  db.exec("INSERT INTO u VALUES (5, 15, 'a'), (3, 25, 'c')");
  db.exec("SELECT * FROM dbsp_sync('t')");
  db.exec("SELECT * FROM dbsp_sync('u')");

  const std::string sql =
      "SELECT t.id, t.val, u.val FROM t JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_both', '" + sql + "')");

  auto &mgr = db.manager();
  REQUIRE(mgr.shared_arrangement_count() == 2);
  requireViewMatchesQuery(db, "v_both", sql);

  // Deltas to EACH side flow through the post-delta arrangement algebra
  runDifferentialTwoTables(db, "v_both", sql, 127);

  // Empty one side completely, then refill: weight bookkeeping must
  // survive the round trip on both arrangements
  db.exec("DELETE FROM u");
  db.exec("SELECT * FROM dbsp_sync('u')");
  requireViewMatchesQuery(db, "v_both", sql);
  db.exec("INSERT INTO u VALUES (1, 7, 'z'), (4, 9, 'z')");
  db.exec("SELECT * FROM dbsp_sync('u')");
  requireViewMatchesQuery(db, "v_both", sql);
}

TEST_CASE("planner: parallel level propagation matches sequential",
          "[integration][planner][parallel]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);
  // Toggle through the SQL surface so the function is covered too
  auto status = db.query("SELECT * FROM dbsp_parallel(true)");
  REQUIRE_FALSE(status->HasError());
  REQUIRE(status->GetValue(0, 0).ToString().find("ENABLED") !=
          std::string::npos);

  // Four sibling views (one level, stepped concurrently) plus a diamond
  // union on top (next level, must see BOTH parents' deltas)
  const std::string s1 = "SELECT id, val FROM t WHERE val > 30";
  const std::string s2 = "SELECT id, val FROM t WHERE val <= 30";
  const std::string s3 =
      "SELECT t.id, t.val, u.val FROM t JOIN u ON t.id = u.id";
  const std::string s4 = "SELECT tag, SUM(val) FROM t GROUP BY tag";
  const std::string s5 =
      "SELECT * FROM dbsp_query('vp1') UNION ALL "
      "SELECT * FROM dbsp_query('vp2')";
  db.exec("SELECT * FROM dbsp_create_view('vp1', '" + s1 + "')");
  db.exec("SELECT * FROM dbsp_create_view('vp2', '" + s2 + "')");
  db.exec("SELECT * FROM dbsp_create_view('vp3', '" + s3 + "')");
  db.exec("SELECT * FROM dbsp_create_view('vp4', '" + s4 + "')");
  db.exec("SELECT * FROM dbsp_create_view('vp5', "
          "'SELECT id, val FROM vp1 UNION ALL SELECT id, val FROM vp2')");

  // Deltas above the 256-row parallel threshold so threads actually spawn
  for (int round = 0; round < 4; round++) {
    db.exec("INSERT INTO t SELECT i, i % 97, chr(CAST(97 + i % 3 AS INT)) "
            "FROM range(" + std::to_string(round * 500) + ", " +
            std::to_string(round * 500 + 500) + ") s(i)");
    db.exec("DELETE FROM t WHERE id % 7 = " + std::to_string(round));
    db.exec("SELECT * FROM dbsp_sync('t')");
    db.exec("SELECT * FROM dbsp_sync('u')");
    requireViewMatchesQuery(db, "vp1", s1);
    requireViewMatchesQuery(db, "vp2", s2);
    requireViewMatchesQuery(db, "vp3", s3);
    requireViewMatchesQuery(db, "vp4", s4);
    requireViewMatchesQuery(
        db, "vp5",
        "SELECT id, val FROM (" + s1 + ") UNION ALL SELECT id, val FROM (" +
            s2 + ")");
  }
  db.exec("SELECT * FROM dbsp_parallel(false)");
}

// ---------------------------------------------------------------------------
// Phase J: ROLLUP/CUBE/GROUPING SETS + aggregate modifiers
// ---------------------------------------------------------------------------

TEST_CASE("planner J: ROLLUP differential",
          "[integration][planner][groupingsets]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql =
      "SELECT tag, id % 2 AS parity, COUNT(*), SUM(val) FROM t "
      "GROUP BY ROLLUP(tag, parity)";
  db.exec("SELECT * FROM dbsp_create_view('v_rollup', '" + sql + "')");
  requireViewMatchesQuery(db, "v_rollup", sql);
  runDifferential(db, "v_rollup", sql, 211);
}

TEST_CASE("planner J: CUBE differential",
          "[integration][planner][groupingsets]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql =
      "SELECT tag, id % 3 AS trio, COUNT(*), AVG(val), MIN(val) FROM t "
      "GROUP BY CUBE(tag, trio)";
  db.exec("SELECT * FROM dbsp_create_view('v_cube', '" + sql + "')");
  requireViewMatchesQuery(db, "v_cube", sql);
  runDifferential(db, "v_cube", sql, 223);
}

TEST_CASE("planner J: GROUPING SETS with GROUPING() differential",
          "[integration][planner][groupingsets]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql =
      "SELECT tag, id % 2 AS parity, SUM(val), "
      "GROUPING(tag), GROUPING(tag, parity) FROM t "
      "GROUP BY GROUPING SETS ((tag), (parity), ())";
  db.exec("SELECT * FROM dbsp_create_view('v_gsets', '" + sql + "')");
  requireViewMatchesQuery(db, "v_gsets", sql);
  runDifferential(db, "v_gsets", sql, 227);
}

TEST_CASE("planner J: FILTER clause differential",
          "[integration][planner][aggmod]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql =
      "SELECT tag, COUNT(*) FILTER (WHERE val > 40), "
      "SUM(val) FILTER (WHERE id % 2 = 0), "
      "AVG(val) FILTER (WHERE val < 90) FROM t GROUP BY tag";
  db.exec("SELECT * FROM dbsp_create_view('v_aggfilter', '" + sql + "')");
  requireViewMatchesQuery(db, "v_aggfilter", sql);
  runDifferential(db, "v_aggfilter", sql, 229);
}

TEST_CASE("planner J: DISTINCT aggregates differential",
          "[integration][planner][aggmod]") {
  DuckDBTestHarness db;
  setupTable(db);
  // val repeats across rows (generator draws from 0..99 with dupes) and
  // includes NULLs — COUNT/SUM/AVG must count each surviving value once
  const std::string sql =
      "SELECT tag, COUNT(DISTINCT val), SUM(DISTINCT val), "
      "AVG(DISTINCT val), MIN(DISTINCT val) FROM t GROUP BY tag";
  db.exec("SELECT * FROM dbsp_create_view('v_aggdist', '" + sql + "')");
  requireViewMatchesQuery(db, "v_aggdist", sql);
  runDifferential(db, "v_aggdist", sql, 233);
}

TEST_CASE("planner J: DISTINCT + FILTER combined, global",
          "[integration][planner][aggmod]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql =
      "SELECT COUNT(DISTINCT val) FILTER (WHERE tag != 'b'), "
      "SUM(DISTINCT val), COUNT(*) FROM t";
  std::string escaped = sql;
  for (size_t pos = 0; (pos = escaped.find('\'', pos)) != std::string::npos;
       pos += 2) {
    escaped.insert(pos, 1, '\'');
  }
  db.exec("SELECT * FROM dbsp_create_view('v_aggmix', '" + escaped + "')");
  requireViewMatchesQuery(db, "v_aggmix", sql);
  runDifferential(db, "v_aggmix", sql, 239);
}

TEST_CASE("planner J: ORDER BY inside order-insensitive aggregate",
          "[integration][planner][aggmod]") {
  DuckDBTestHarness db;
  setupTable(db);
  // Semantically inert for SUM — accepted, ignored, must equal plain SUM
  const std::string sql =
      "SELECT tag, SUM(val ORDER BY id) FROM t GROUP BY tag";
  db.exec("SELECT * FROM dbsp_create_view('v_aggorder', '" + sql + "')");
  requireViewMatchesQuery(db, "v_aggorder", sql);
  runDifferential(db, "v_aggorder", sql, 241);
}

TEST_CASE("planner J: ROLLUP over join with DISTINCT agg",
          "[integration][planner][groupingsets]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);
  const std::string sql =
      "SELECT t.tag, COUNT(DISTINCT u.val), SUM(u.val) "
      "FROM t JOIN u ON t.id = u.id GROUP BY ROLLUP(t.tag)";
  db.exec("SELECT * FROM dbsp_create_view('v_rolljoin', '" + sql + "')");
  requireViewMatchesQuery(db, "v_rolljoin", sql);
  runDifferentialTwoTables(db, "v_rolljoin", sql, 251);
}

// ---------------------------------------------------------------------------
// Phase J2: order-sensitive aggregates (string_agg / array_agg)
// ---------------------------------------------------------------------------

namespace {
std::string escapeSql(const std::string &sql) {
  std::string out = sql;
  for (size_t pos = 0; (pos = out.find('\'', pos)) != std::string::npos;
       pos += 2) {
    out.insert(pos, 1, '\'');
  }
  return out;
}
} // namespace

TEST_CASE("planner J2: string_agg ORDER BY differential",
          "[integration][planner][orderagg]") {
  DuckDBTestHarness db;
  setupTable(db);
  // id is unique → deterministic order, matches DuckDB exactly
  const std::string sql =
      "SELECT tag, STRING_AGG(CAST(val AS VARCHAR), '|' ORDER BY id) "
      "FROM t GROUP BY tag";
  db.exec("SELECT * FROM dbsp_create_view('v_sagg', '" + escapeSql(sql) +
          "')");
  requireViewMatchesQuery(db, "v_sagg", sql);
  runDifferential(db, "v_sagg", sql, 307);
}

TEST_CASE("planner J2: string_agg DESC + NULLS FIRST key differential",
          "[integration][planner][orderagg]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql =
      "SELECT tag, STRING_AGG(CAST(id AS VARCHAR) ORDER BY val DESC "
      "NULLS FIRST, id) FROM t GROUP BY tag";
  db.exec("SELECT * FROM dbsp_create_view('v_saggd', '" + escapeSql(sql) +
          "')");
  requireViewMatchesQuery(db, "v_saggd", sql);
  runDifferential(db, "v_saggd", sql, 311);
}

TEST_CASE("planner J2: array_agg ORDER BY differential (keeps NULLs)",
          "[integration][planner][orderagg]") {
  DuckDBTestHarness db;
  setupTable(db);
  // val includes NULLs from the generator — array_agg must keep them
  const std::string sql =
      "SELECT tag, ARRAY_AGG(val ORDER BY id) FROM t GROUP BY tag";
  db.exec("SELECT * FROM dbsp_create_view('v_aagg', '" + escapeSql(sql) +
          "')");
  requireViewMatchesQuery(db, "v_aagg", sql);
  runDifferential(db, "v_aagg", sql, 313);
}

TEST_CASE("planner J2: string_agg with FILTER, global",
          "[integration][planner][orderagg]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql =
      "SELECT STRING_AGG(CAST(id AS VARCHAR) ORDER BY id) "
      "FILTER (WHERE val > 40) FROM t";
  db.exec("SELECT * FROM dbsp_create_view('v_saggf', '" + escapeSql(sql) +
          "')");
  requireViewMatchesQuery(db, "v_saggf", sql);
  runDifferential(db, "v_saggf", sql, 317);
}

TEST_CASE("planner J2: unordered string_agg stays rejected",
          "[integration][planner][orderagg]") {
  DuckDBTestHarness db;
  setupTable(db);
  auto result = db.query(
      "SELECT * FROM dbsp_create_view('v_bad', "
      "'SELECT tag, STRING_AGG(CAST(val AS VARCHAR)) FROM t GROUP BY tag')");
  REQUIRE(result->HasError());
  REQUIRE(result->GetError().find("DBSP-E110") != std::string::npos);
  REQUIRE(result->GetError().find("ORDER BY") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Phase K1: disk-backed baselines (dbsp_spill)
// ---------------------------------------------------------------------------

TEST_CASE("planner K1: spill mode differential across view shapes",
          "[integration][planner][spill]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);
  auto status = db.query("SELECT * FROM dbsp_spill(true)");
  REQUIRE_FALSE(status->HasError());
  REQUIRE(status->GetValue(0, 0).ToString().find("ENABLED") !=
          std::string::npos);
  auto &mgr = db.manager();
  REQUIRE(mgr.spill_enabled());

  // Join view created AFTER spill: init replay + arrangement backfill
  // must stream from disk
  const std::string sql =
      "SELECT t.tag, COUNT(*), SUM(u.val) FROM t JOIN u ON t.id = u.id "
      "GROUP BY t.tag";
  db.exec("SELECT * FROM dbsp_create_view('v_spill', '" + sql + "')");
  requireViewMatchesQuery(db, "v_spill", sql);
  runDifferentialTwoTables(db, "v_spill", sql, 401);

  db.exec("SELECT * FROM dbsp_spill(false)");
  REQUIRE_FALSE(mgr.spill_enabled());
  // After migrating back to RAM everything still lines up
  db.exec("INSERT INTO t VALUES (7, 77, 'c')");
  db.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(db, "v_spill", sql);
}

TEST_CASE("planner K1: enabling spill migrates an existing baseline",
          "[integration][planner][spill]") {
  DuckDBTestHarness db;
  setupTable(db); // tracked + synced in RAM mode
  const std::string sql = "SELECT tag, SUM(val) FROM t GROUP BY tag";
  db.exec("SELECT * FROM dbsp_create_view('v_mig', '" + sql + "')");
  requireViewMatchesQuery(db, "v_mig", sql);

  db.exec("SELECT * FROM dbsp_spill(true)"); // migrate live baseline
  auto &mgr = db.manager();
  REQUIRE(mgr.get_tracked_table_count("memory.main.t") == 3); // canonical key (D2)

  // Deltas after migration diff against the spilled baseline
  db.exec("INSERT INTO t VALUES (10, 5, 'b'), (11, NULL, 'a')");
  db.exec("DELETE FROM t WHERE id = 1");
  db.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(db, "v_mig", sql);
  runDifferential(db, "v_mig", sql, 409);
  db.exec("SELECT * FROM dbsp_spill(false)");
}

TEST_CASE("planner K1: captured-delta commits hit the spilled baseline",
          "[integration][planner][spill][autocdc]") {
  DuckDBTestHarness db;
  setupTable(db);
  db.exec("SELECT * FROM dbsp_spill(true)");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");
  const std::string sql = "SELECT COUNT(*), SUM(val) FROM t";
  db.exec("SELECT * FROM dbsp_create_view('v_cap', '" + sql + "')");

  auto &mgr = db.manager();
  const uint64_t before = mgr.captured_delta_syncs();
  db.exec("BEGIN TRANSACTION");
  db.exec("INSERT INTO t VALUES (100, 1, 'z'), (101, 2, 'z')");
  db.exec("COMMIT");
  // Fast path must still fire (baseline weight guard reads spilled totals)
  REQUIRE(mgr.captured_delta_syncs() == before + 1);
  requireViewMatchesQuery(db, "v_cap", sql);

  // Follow-up scan-diff sync agrees with the appended baseline
  db.exec("SELECT * FROM dbsp_auto_sync(false)");
  db.exec("INSERT INTO t VALUES (102, 3, 'z')");
  db.exec("DELETE FROM t WHERE id = 100");
  db.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(db, "v_cap", sql);
  db.exec("SELECT * FROM dbsp_spill(false)");
}

// ---------------------------------------------------------------------------
// Phase K2: spilled shared join arrangements
// ---------------------------------------------------------------------------

TEST_CASE("planner K2: spilled arrangement differential + live migration",
          "[integration][planner][spill][arrangement]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);

  // Views FIRST (RAM arrangements), then spill: live arrangements must
  // migrate to disk and keep answering probes
  const std::string sql =
      "SELECT t.id, t.val, u.val FROM t JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_arrsp', '" + sql + "')");
  auto &mgr = db.manager();
  REQUIRE(mgr.shared_arrangement_count() >= 1);

  db.exec("SELECT * FROM dbsp_spill(true)");
  requireViewMatchesQuery(db, "v_arrsp", sql);
  runDifferentialTwoTables(db, "v_arrsp", sql, 501);

  // Back to RAM: buckets reload, keys re-derived from arrangement evals
  db.exec("SELECT * FROM dbsp_spill(false)");
  db.exec("INSERT INTO t VALUES (2, 33, 'z')");
  db.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(db, "v_arrsp", sql);
}

TEST_CASE("planner K2: NOT IN over spilled right side",
          "[integration][planner][spill][arrangement][mark]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);
  db.exec("SELECT * FROM dbsp_spill(true)");

  const std::string sql =
      "SELECT id, val FROM t WHERE val NOT IN (SELECT val FROM u)";
  db.exec("SELECT * FROM dbsp_create_view('v_spmark', '" + sql + "')");
  requireViewMatchesQuery(db, "v_spmark", sql);
  runDifferentialTwoTables(db, "v_spmark", sql, 503);

  db.exec("DELETE FROM u");
  db.exec("SELECT * FROM dbsp_sync('u')");
  requireViewMatchesQuery(db, "v_spmark", sql);
  db.exec("SELECT * FROM dbsp_spill(false)");
}

TEST_CASE("planner K2: parallel propagation over one spilled arrangement",
          "[integration][planner][spill][arrangement][parallel]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);
  db.exec("SELECT * FROM dbsp_spill(true)");
  db.exec("SELECT * FROM dbsp_parallel(true)");

  // Identical SQL → same fingerprint → several views probe ONE spilled
  // arrangement concurrently (the probe mutex is what this pins down)
  const std::string sql =
      "SELECT t.id, t.val, u.val FROM t JOIN u ON t.id = u.id";
  for (int v = 0; v < 4; v++) {
    db.exec("SELECT * FROM dbsp_create_view('v_par" + std::to_string(v) +
            "', '" + sql + "')");
  }

  for (int round = 0; round < 3; round++) {
    db.exec("INSERT INTO t SELECT i % 8, i, 'p' FROM range(" +
            std::to_string(round * 400) + ", " +
            std::to_string(round * 400 + 400) + ") s(i)");
    db.exec("INSERT INTO u SELECT i % 8, i, 'q' FROM range(" +
            std::to_string(round * 300) + ", " +
            std::to_string(round * 300 + 300) + ") s(i)");
    db.exec("SELECT * FROM dbsp_sync('t')");
    db.exec("SELECT * FROM dbsp_sync('u')");
    for (int v = 0; v < 4; v++) {
      requireViewMatchesQuery(db, "v_par" + std::to_string(v), sql);
    }
  }
  db.exec("SELECT * FROM dbsp_parallel(false)");
  db.exec("SELECT * FROM dbsp_spill(false)");
}

// ---------------------------------------------------------------------------
// Phase L1: holistic aggregates (median / quantile / mode)
// ---------------------------------------------------------------------------

TEST_CASE("planner L1: median differential",
          "[integration][planner][holistic]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql = "SELECT tag, MEDIAN(val) FROM t GROUP BY tag";
  db.exec("SELECT * FROM dbsp_create_view('v_med', '" + sql + "')");
  requireViewMatchesQuery(db, "v_med", sql);
  runDifferential(db, "v_med", sql, 601);
}

TEST_CASE("planner L1: quantile_cont and quantile_disc differential",
          "[integration][planner][holistic]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql =
      "SELECT tag, QUANTILE_CONT(val, 0.25), QUANTILE_DISC(val, 0.75) "
      "FROM t GROUP BY tag";
  db.exec("SELECT * FROM dbsp_create_view('v_quant', '" + sql + "')");
  requireViewMatchesQuery(db, "v_quant", sql);
  runDifferential(db, "v_quant", sql, 607);
}

TEST_CASE("planner L1: mode differential on tie-free data",
          "[integration][planner][holistic]") {
  DuckDBTestHarness db;
  // Deterministic multiplicities: value v appears v times → unique mode.
  // (Our tie-break is smallest value; DuckDB's is scan-order-dependent,
  // so ties would flake the differential.)
  db.exec("CREATE TABLE t (id INT, val INT, tag VARCHAR)");
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_sync('t')");
  const std::string sql = "SELECT MODE(val) FROM t";
  db.exec("SELECT * FROM dbsp_create_view('v_mode', '" + sql + "')");

  int next_id = 0;
  for (int v = 1; v <= 5; v++) {
    for (int c = 0; c < v; c++) {
      db.exec("INSERT INTO t VALUES (" + std::to_string(next_id++) + ", " +
              std::to_string(v) + ", 'x')");
    }
    db.exec("SELECT * FROM dbsp_sync('t')");
    requireViewMatchesQuery(db, "v_mode", sql);
  }
  // Delete the current winner's copies → mode falls back to 4
  db.exec("DELETE FROM t WHERE val = 5");
  db.exec("SELECT * FROM dbsp_sync('t')");
  requireViewMatchesQuery(db, "v_mode", sql);
}

TEST_CASE("planner L1: median with FILTER inside ROLLUP",
          "[integration][planner][holistic][groupingsets]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql =
      "SELECT tag, MEDIAN(val) FILTER (WHERE id % 2 = 0) "
      "FROM t GROUP BY ROLLUP(tag)";
  db.exec("SELECT * FROM dbsp_create_view('v_medroll', '" + sql + "')");
  requireViewMatchesQuery(db, "v_medroll", sql);
  runDifferential(db, "v_medroll", sql, 613);
}

TEST_CASE("planner L2: sharded probe passes match serial",
          "[integration][planner][parallel][shard]") {
  DuckDBTestHarness db;
  db.exec("CREATE TABLE bl (id INT, v INT)");
  db.exec("CREATE TABLE br (id INT, w INT)");
  db.exec("SELECT * FROM dbsp_track('bl')");
  db.exec("SELECT * FROM dbsp_track('br')");
  db.exec("SELECT * FROM dbsp_sync('bl')");
  db.exec("SELECT * FROM dbsp_sync('br')");
  db.exec("SELECT * FROM dbsp_parallel(true)");

  const std::string sql =
      "SELECT bl.id, bl.v, br.w FROM bl JOIN br ON bl.id = br.id";
  db.exec("SELECT * FROM dbsp_create_view('v_shard', '" + sql + "')");

  // Deltas above the 4096-row shard threshold, small key space so
  // buckets are fat and every shard emits
  for (int round = 0; round < 3; round++) {
    db.exec("INSERT INTO bl SELECT i % 100, i FROM range(" +
            std::to_string(round * 5000) + ", " +
            std::to_string(round * 5000 + 5000) + ") s(i)");
    db.exec("INSERT INTO br SELECT i % 100, i FROM range(" +
            std::to_string(round * 3000) + ", " +
            std::to_string(round * 3000 + 3000) + ") s(i)");
    db.exec("DELETE FROM bl WHERE v % 7 = " + std::to_string(round));
    db.exec("SELECT * FROM dbsp_sync('bl')");
    db.exec("SELECT * FROM dbsp_sync('br')");
    // Row counts only per round (full compare would be 500k+ rows);
    // exact compare at the end
    auto expected = db.query("SELECT COUNT(*) FROM (" + sql + ")");
    auto actual =
        db.query("SELECT COUNT(*) FROM dbsp_query('v_shard')");
    REQUIRE(actual->GetValue(0, 0) == expected->GetValue(0, 0));
  }
  requireViewMatchesQuery(db, "v_shard", sql);
  db.exec("SELECT * FROM dbsp_parallel(false)");
}

// ---------------------------------------------------------------------------
// Phase M: SQL-coverage leaves — window-over-expressions, mad, LIMIT %
// ---------------------------------------------------------------------------

TEST_CASE("planner M1: window PARTITION BY / ORDER BY over expressions",
          "[integration][planner][window][coverage]") {
  DuckDBTestHarness db;
  setupTable(db);
  // Both the partition and the order are expressions; previously E110
  // with a rewrite hint — now auto-projected below the window
  const std::string sql =
      "SELECT id, val, ROW_NUMBER() OVER "
      "(PARTITION BY id % 2 ORDER BY val * 2, id) FROM t";
  db.exec("SELECT * FROM dbsp_create_view('v_wexpr', '" + sql + "')");
  requireViewMatchesQuery(db, "v_wexpr", sql);
  runDifferential(db, "v_wexpr", sql, 701);
}

TEST_CASE("planner M1: window aggregate over an expression",
          "[integration][planner][window][coverage]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql =
      "SELECT id, SUM(val + 1) OVER "
      "(PARTITION BY tag ORDER BY id) FROM t";
  db.exec("SELECT * FROM dbsp_create_view('v_wagg', '" + sql + "')");
  requireViewMatchesQuery(db, "v_wagg", sql);
  runDifferential(db, "v_wagg", sql, 709);
}

TEST_CASE("planner M2: mad differential",
          "[integration][planner][holistic][coverage]") {
  DuckDBTestHarness db;
  setupTable(db);
  const std::string sql = "SELECT tag, MAD(val) FROM t GROUP BY tag";
  db.exec("SELECT * FROM dbsp_create_view('v_mad', '" + sql + "')");
  requireViewMatchesQuery(db, "v_mad", sql);
  runDifferential(db, "v_mad", sql, 719);
}

TEST_CASE("planner M3: percentage LIMIT differential",
          "[integration][planner][sort][coverage]") {
  DuckDBTestHarness db;
  setupTable(db);
  // 30% of a changing table: the cutoff count must track table size
  const std::string sql =
      "SELECT id, val FROM t ORDER BY id LIMIT 30 PERCENT";
  db.exec("SELECT * FROM dbsp_create_view('v_pct', '" + sql + "')");
  requireViewMatchesQuery(db, "v_pct", sql);
  runDifferential(db, "v_pct", sql, 727);
}

// ---------------------------------------------------------------------------
// Phase N: RAM-state work (bounded top-K, local index spill, big groups)
// ---------------------------------------------------------------------------

TEST_CASE("planner N2: bounded top-K under spill with refills",
          "[integration][planner][spill][topk]") {
  DuckDBTestHarness db;
  db.exec("CREATE TABLE tk (id INT, score INT)");
  db.exec("SELECT * FROM dbsp_track('tk')");
  db.exec("SELECT * FROM dbsp_sync('tk')");
  db.exec("SELECT * FROM dbsp_spill(true)");

  const std::string sql =
      "SELECT id, score FROM tk ORDER BY score DESC, id LIMIT 10";
  db.exec("SELECT * FROM dbsp_create_view('v_topk_sp', '" + sql + "')");

  // Fill well past the window, then delete the ENTIRE top repeatedly —
  // every round digs through the window and forces a refill from the log
  db.exec("INSERT INTO tk SELECT i, i FROM range(2000) s(i)");
  db.exec("SELECT * FROM dbsp_sync('tk')");
  requireViewMatchesQuery(db, "v_topk_sp", sql);

  for (int round = 0; round < 6; round++) {
    db.exec("DELETE FROM tk WHERE score >= (SELECT MAX(score) - 150 "
            "FROM tk)");
    db.exec("SELECT * FROM dbsp_sync('tk')");
    requireViewMatchesQuery(db, "v_topk_sp", sql);
    db.exec("INSERT INTO tk SELECT 10000 + " + std::to_string(round) +
            " * 100 + i, 5000 + i FROM range(50) s(i)");
    db.exec("SELECT * FROM dbsp_sync('tk')");
    requireViewMatchesQuery(db, "v_topk_sp", sql);
  }
  db.exec("SELECT * FROM dbsp_spill(false)");
}

TEST_CASE("planner N3: local (unshareable) join index spills",
          "[integration][planner][spill][localidx]") {
  DuckDBTestHarness db;
  setupTable(db);
  setupTableU(db);
  db.exec("SELECT * FROM dbsp_spill(true)");
  auto &mgr = db.manager();
  const size_t arr_base = mgr.shared_arrangement_count();

  // Right side is a SUBQUERY (filter under the join) — not a bare scan,
  // so no shared arrangement; its local index goes to the bucket log
  const std::string sql =
      "SELECT t.id, t.val, f.val FROM t JOIN "
      "(SELECT id, val FROM u WHERE val > 10) f ON t.id = f.id";
  db.exec("SELECT * FROM dbsp_create_view('v_lidx', '" + sql + "')");
  // No new arrangement for the filtered side
  REQUIRE(mgr.shared_arrangement_count() <= arr_base + 1);
  requireViewMatchesQuery(db, "v_lidx", sql);
  runDifferentialTwoTables(db, "v_lidx", sql, 811);

  // LEFT join: left self-pads (stays RAM), filtered right spills local
  const std::string sql2 =
      "SELECT t.id, f.val FROM t LEFT JOIN "
      "(SELECT id, val FROM u WHERE val > 10) f ON t.id = f.id";
  db.exec("SELECT * FROM dbsp_create_view('v_lidx2', '" + sql2 + "')");
  requireViewMatchesQuery(db, "v_lidx2", sql2);
  runDifferentialTwoTables(db, "v_lidx2", sql2, 821);
  db.exec("SELECT * FROM dbsp_spill(false)");
}

TEST_CASE("planner N4: oversized holistic group spills its values",
          "[integration][planner][spill][biggroup]") {
  DuckDBTestHarness db;
  db.exec("CREATE TABLE bg (id INT, x INT)");
  db.exec("SELECT * FROM dbsp_track('bg')");
  db.exec("SELECT * FROM dbsp_sync('bg')");
  db.exec("SELECT * FROM dbsp_spill(true)");

  // One giant group: 70k values crosses the 65536-value threshold and
  // migrates the multiset to disk mid-stream
  const std::string sql = "SELECT MEDIAN(x), MIN(x), MAX(x) FROM bg";
  db.exec("SELECT * FROM dbsp_create_view('v_bg', '" + sql + "')");
  db.exec("INSERT INTO bg SELECT i, i * 7 % 100000 FROM range(70000) s(i)");
  db.exec("SELECT * FROM dbsp_sync('bg')");
  requireViewMatchesQuery(db, "v_bg", sql);

  // Post-migration deltas hit the spilled path; deletions included
  db.exec("DELETE FROM bg WHERE id % 9 = 0");
  db.exec("INSERT INTO bg SELECT 100000 + i, i FROM range(500) s(i)");
  db.exec("SELECT * FROM dbsp_sync('bg')");
  requireViewMatchesQuery(db, "v_bg", sql);
  db.exec("SELECT * FROM dbsp_spill(false)");
}

TEST_CASE("zero-ceremony UX: create view, insert, read — no track/sync",
          "[integration][planner][autosync]") {
  DuckDBTestHarness db;
  // No dbsp_track, no dbsp_sync anywhere in this test: view creation
  // auto-tracks + loads sources, and auto-sync (default ON) keeps the
  // view current on every commit
  db.exec("CREATE TABLE orders (id INT, customer VARCHAR, amount INT)");
  db.exec("INSERT INTO orders VALUES (1, 'Alice', 100), (2, 'Bob', 200)");
  const std::string sql =
      "SELECT customer, SUM(amount) AS total FROM orders GROUP BY customer";
  db.exec("SELECT * FROM dbsp_create_view('totals', '" + sql + "')");
  requireViewMatchesQuery(db, "totals", sql);

  db.exec("INSERT INTO orders VALUES (3, 'Alice', 50)");
  requireViewMatchesQuery(db, "totals", sql);

  db.exec("DELETE FROM orders WHERE id = 2");
  requireViewMatchesQuery(db, "totals", sql);

  db.exec("UPDATE orders SET amount = 500 WHERE id = 1");
  requireViewMatchesQuery(db, "totals", sql);
}

TEST_CASE("spill: enabling sweeps dead processes' spill directories",
          "[integration][spill][cleanup]") {
  namespace fs = std::filesystem;
  // Plant a directory that looks abandoned by a long-dead process
  // (pid far above any live range)
  const fs::path stale =
      fs::temp_directory_path() / "dbsp_spill_99999989";
  fs::create_directories(stale);
  { std::ofstream(stale / "t.dbspill") << "junk"; }
  REQUIRE(fs::exists(stale));

  DuckDBTestHarness db;
  db.exec("SELECT * FROM dbsp_spill(true)");
  REQUIRE_FALSE(fs::exists(stale));
  db.exec("SELECT * FROM dbsp_spill(false)");
}

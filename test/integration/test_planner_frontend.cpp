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

  // Default is ON since B5
  auto status = db.query("SELECT * FROM dbsp_use_planner()");
  REQUIRE(status->GetValue(0, 0).ToString().find("ENABLED") !=
          std::string::npos);

  auto off = db.query("SELECT * FROM dbsp_use_planner(false)");
  REQUIRE(off->GetValue(0, 0).ToString().find("DISABLED") !=
          std::string::npos);

  status = db.query("SELECT * FROM dbsp_use_planner()");
  REQUIRE(status->GetValue(0, 0).ToString().find("DISABLED") !=
          std::string::npos);

  auto on = db.query("SELECT * FROM dbsp_use_planner(true)");
  REQUIRE(on->GetValue(0, 0).ToString().find("ENABLED") != std::string::npos);
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

TEST_CASE("planner frontend: correlated subquery and recursive CTE fall back",
          "[integration][planner]") {
  DuckDBTestHarness db;
  setupTable(db);

  // Correlated subquery: planner rejects (DELIM_JOIN), parser rejects
  // subqueries too — creation must fail with an error, not crash
  auto corr = db.query(
      "SELECT * FROM dbsp_create_view('v_corr', "
      "'SELECT * FROM t a WHERE val > (SELECT AVG(val) FROM t b "
      "WHERE b.tag = a.tag)')");
  REQUIRE(corr->HasError());

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

TEST_CASE("planner frontend: flag off uses parser path",
          "[integration][planner]") {
  DuckDBTestHarness db;
  db.createTable("t", "id INT, val INT, tag VARCHAR", {"(1, 10, 'a')"});
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_sync('t')");
  // Explicitly disable (default is ON since B5)
  db.exec("SELECT * FROM dbsp_use_planner(false)");

  db.exec("SELECT * FROM dbsp_create_view('v_off', "
          "'SELECT * FROM t WHERE val > 5')");
  db.assertViewRowCount("v_off", 1);
}

// ===== Phase C1: ORDER BY / LIMIT through the planner =====

namespace {

// True when the view was built by the planner frontend (no parser fallback)
bool plannerBuilt(const std::string &view) {
  const auto *v = dbsp_native::get_cdc_manager().get_view(view);
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
  REQUIRE(plannerBuilt("v_sorted"));

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
  REQUIRE(plannerBuilt("v_sorted_drop"));
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
  REQUIRE(plannerBuilt("v_top2"));
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
  REQUIRE(plannerBuilt("v_lim"));
  db.assertViewRowCount("v_lim", 3);
}

TEST_CASE("planner C1: percentage LIMIT is not planner-built",
          "[integration][planner][limit]") {
  DuckDBTestHarness db;
  db.createTable("pt", "id INT", {"(1)", "(2)"});
  db.exec("SELECT * FROM dbsp_track('pt')");
  db.exec("SELECT * FROM dbsp_sync('pt')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  // Non-constant/percentage limits must yield E110, not a wrong translation.
  // (Until C5 this falls back to the parser; either way it must not be a
  // PlannedCircuitView claiming to support it.)
  auto res = db.query("SELECT * FROM dbsp_create_view('v_pct', "
                      "'SELECT id FROM pt LIMIT 50%')");
  REQUIRE_FALSE(plannerBuilt("v_pct"));
  (void)res;
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
  REQUIRE(plannerBuilt("v_topk"));
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
  REQUIRE(plannerBuilt("v_rec_all"));
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
  REQUIRE(plannerBuilt("v_rec_u"));
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
  REQUIRE(plannerBuilt("v_reach"));
  db.assertViewRowCount("v_reach", 3); // (1,2) (1,3) (1,4)

  db.exec("INSERT INTO edges VALUES (4, 5)");
  db.exec("SELECT * FROM dbsp_sync('edges')");
  db.assertViewRowCount("v_reach", 4); // + (1,5)
}

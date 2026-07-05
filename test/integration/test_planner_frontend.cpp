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
  REQUIRE(plannerBuilt("v_don"));

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
  REQUIRE(plannerBuilt("v_don_diff"));
  requireViewMatchesQuery(db, "v_don_diff", sql);
  runDifferential(db, "v_don_diff", sql, 83);
}

// ===== Phase C4: circuit-IR optimizer =====

namespace {

size_t nodeCount(const std::string &view) {
  const auto *v = dynamic_cast<const dbsp_native::PlannedCircuitView *>(
      dbsp_native::get_cdc_manager().get_view(view));
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
  REQUIRE(plannerBuilt("v_fused"));
  size_t fused = nodeCount("v_fused");

  // Same view without the IR optimizer: one extra node (filter + map split)
  dbsp_native::g_plan_ir_optimize = false;
  db.exec("SELECT * FROM dbsp_create_view('v_raw', '" + sql + "')");
  dbsp_native::g_plan_ir_optimize = true;
  size_t raw = nodeCount("v_raw");
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
  REQUIRE(plannerBuilt("v_pd"));

  // Pushdown must actually fire: pushed-down plan has MORE nodes (two
  // filters below the join) than the unoptimized one (one filter above)
  size_t pushed = nodeCount("v_pd");
  dbsp_native::g_plan_ir_optimize = false;
  db.exec("SELECT * FROM dbsp_create_view('v_pd_raw', '" + sql + "')");
  dbsp_native::g_plan_ir_optimize = true;
  REQUIRE(pushed != nodeCount("v_pd_raw"));

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
  REQUIRE(plannerBuilt("v_rdel"));
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
  REQUIRE(plannerBuilt("v_rreach"));
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
  REQUIRE(plannerBuilt("v_lj"));
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
  REQUIRE(plannerBuilt("v_ljd"));
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
  REQUIRE(plannerBuilt("v_rjd"));
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
  REQUIRE(plannerBuilt("v_fjd"));
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
  REQUIRE(plannerBuilt("v_ljr"));
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
  REQUIRE(plannerBuilt("v_in"));
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
  REQUIRE(plannerBuilt("v_notin"));
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
  REQUIRE(plannerBuilt("v_scalar"));
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
  REQUIRE(plannerBuilt("v_bulk"));
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
  REQUIRE(plannerBuilt("v_e1_j"));
  REQUIRE(plannerBuilt("v_e1_a"));
  REQUIRE(plannerBuilt("v_e1_s"));

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
  REQUIRE(plannerBuilt("v_e1_u"));
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
  REQUIRE(plannerBuilt("v_corr"));
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
  REQUIRE(plannerBuilt("v_exists"));
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
  REQUIRE(plannerBuilt("v_nexists"));
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
  auto &mgr = dbsp_native::get_cdc_manager();
  const size_t base = mgr.shared_arrangement_count();

  // Identical SQL → identical plan → identical fingerprint: one
  // arrangement serves both views
  const std::string sql1 =
      "SELECT t.id, t.val, u.val FROM t JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_arr1', '" + sql1 + "')");
  db.exec("SELECT * FROM dbsp_create_view('v_arr2', '" + sql1 + "')");
  REQUIRE(mgr.shared_arrangement_count() == base + 1);

  // Different column needs → different projected row shape → a separate
  // arrangement (fingerprint includes the side's column projection)
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
  auto &mgr = dbsp_native::get_cdc_manager();
  const size_t base = mgr.shared_arrangement_count();

  const std::string sql =
      "SELECT t.id, u.val FROM t JOIN u ON t.id = u.id";
  db.exec("SELECT * FROM dbsp_create_view('v_keep', '" + sql + "')");
  db.exec("SELECT * FROM dbsp_create_view('v_drop', '" + sql + "')");
  REQUIRE(mgr.shared_arrangement_count() == base + 1);

  db.exec("SELECT dbsp_drop('v_drop')");
  // Survivor still owns the arrangement and stays correct
  REQUIRE(mgr.shared_arrangement_count() == base + 1);
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
  auto &mgr = dbsp_native::get_cdc_manager();
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
  auto &mgr = dbsp_native::get_cdc_manager();
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

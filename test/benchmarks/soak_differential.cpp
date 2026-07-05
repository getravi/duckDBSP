// Extended randomized soak (pre-Phase-E).
//
// The short per-feature differentials run 12-15 rounds on tiny data; this
// soak runs hundreds of delete-heavy rounds against a STACK of view types
// exercising the newest state machinery together: an outer join feeding an
// aggregate feeding a sort, plus MARK-join (NOT IN) and recursive views
// maintained over the same mutating tables. After every round, each view
// must equal DuckDB's own answer for its SQL.
//
// Not part of ctest (runtime); run manually: ./soak_differential
// Rounds via SOAK_ROUNDS env (default 300).

#include "../test_helpers.hpp"
#include "catch.hpp"

#include <cstdlib>
#include <random>

using namespace dbsp_test;

namespace {

void requireMatches(DuckDBTestHarness &db, const std::string &view,
                    const std::string &sql, int round) {
  auto expected = db.query("SELECT * FROM (" + sql + ") ORDER BY ALL");
  auto actual =
      db.query("SELECT * FROM dbsp_query('" + view + "') ORDER BY ALL");
  INFO("round " << round << " view " << view);
  REQUIRE_FALSE(expected->HasError());
  REQUIRE_FALSE(actual->HasError());
  REQUIRE(actual->ColumnCount() == expected->ColumnCount());
  REQUIRE(actual->RowCount() == expected->RowCount());
  for (size_t r = 0; r < expected->RowCount(); r++) {
    for (size_t c = 0; c < expected->ColumnCount(); c++) {
      INFO("round " << round << " view " << view << " row " << r << " col "
                    << c);
      REQUIRE(actual->GetValue(c, r).ToString() ==
              expected->GetValue(c, r).ToString());
    }
  }
}

} // namespace

TEST_CASE("Soak: stacked views under delete-heavy randomized churn",
          "[soak]") {
  const char *env = std::getenv("SOAK_ROUNDS");
  const int rounds = env ? std::atoi(env) : 300;

  DuckDBTestHarness db;
  db.exec("CREATE TABLE t (id INT, val INT, tag VARCHAR)");
  db.exec("CREATE TABLE u (id INT, val INT, tag VARCHAR)");
  db.exec("SELECT * FROM dbsp_track('t')");
  db.exec("SELECT * FROM dbsp_track('u')");
  db.exec("SELECT * FROM dbsp_sync('t')");
  db.exec("SELECT * FROM dbsp_sync('u')");
  db.exec("SELECT * FROM dbsp_use_planner(true)");

  // Stack: outer join -> (cascades into) aggregate -> sort. Plus MARK and
  // recursion over the same tables.
  const std::string sql_lj =
      "SELECT t.id AS tid, t.val AS tval, u.val AS uval, t.tag AS ttag "
      "FROM t LEFT JOIN u ON t.id = u.id";
  const std::string sql_agg =
      "SELECT ttag, COUNT(*) AS n, SUM(tval) AS s FROM v_soak_lj GROUP BY ttag";
  const std::string sql_agg_direct = // DuckDB-side equivalent for diffing
      "SELECT ttag, COUNT(*) AS n, SUM(tval) AS s FROM (" + sql_lj +
      ") GROUP BY ttag";
  const std::string sql_sort =
      "SELECT tid, tval FROM v_soak_lj ORDER BY tval DESC, tid LIMIT 7";
  const std::string sql_sort_direct =
      "SELECT tid, tval FROM (" + sql_lj + ") ORDER BY tval DESC, tid LIMIT 7";
  const std::string sql_notin =
      "SELECT id, val FROM t WHERE val NOT IN (SELECT val FROM u)";
  const std::string sql_rec =
      "WITH RECURSIVE r AS (SELECT id FROM t WHERE id = 0 UNION "
      "SELECT id+1 FROM r WHERE id < 6) SELECT * FROM r";

  db.exec("SELECT * FROM dbsp_create_view('v_soak_lj', '" + sql_lj + "')");
  db.exec("SELECT * FROM dbsp_create_view('v_soak_agg', '" + sql_agg + "')");
  db.exec("SELECT * FROM dbsp_create_view('v_soak_sort', '" + sql_sort + "')");
  db.exec("SELECT * FROM dbsp_create_view('v_soak_notin', '" + sql_notin +
          "')");
  db.exec("SELECT * FROM dbsp_create_view('v_soak_rec', '" + sql_rec + "')");

  std::mt19937 rng(20260705);
  std::vector<std::pair<std::string, int>> live;

  for (int round = 0; round < rounds; round++) {
    // Inserts: small id space so joins/marks collide constantly
    int inserts = static_cast<int>(rng() % 6) + 1;
    for (int i = 0; i < inserts; i++) {
      std::string table = rng() % 2 ? "t" : "u";
      int id = static_cast<int>(rng() % 10);
      std::string val = rng() % 8 == 0 ? "NULL" : std::to_string(rng() % 40);
      char tag = static_cast<char>('a' + rng() % 3);
      db.exec("INSERT INTO " + table + " VALUES (" + std::to_string(id) +
              ", " + val + ", '" + std::string(1, tag) + "')");
      live.push_back({table, id});
    }
    // Delete-heavy: up to 5 per round (the new pad/mark/recursive state
    // transitions live on the deletion path)
    int deletes = live.empty() ? 0 : static_cast<int>(rng() % 6);
    for (int i = 0; i < deletes && !live.empty(); i++) {
      size_t idx = rng() % live.size();
      db.exec("DELETE FROM " + live[idx].first + " WHERE rowid = (SELECT "
              "rowid FROM " + live[idx].first + " WHERE id = " +
              std::to_string(live[idx].second) + " LIMIT 1)");
      live.erase(live.begin() + static_cast<long>(idx));
    }
    db.exec("SELECT * FROM dbsp_sync('t')");
    db.exec("SELECT * FROM dbsp_sync('u')");

    requireMatches(db, "v_soak_lj", sql_lj, round);
    requireMatches(db, "v_soak_agg", sql_agg_direct, round);
    requireMatches(db, "v_soak_sort", sql_sort_direct, round);
    requireMatches(db, "v_soak_notin", sql_notin, round);
    requireMatches(db, "v_soak_rec", sql_rec, round);
  }
}

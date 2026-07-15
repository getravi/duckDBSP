// Shape-vetting and capture-SQL construction for the O(Δ) UPDATE/DELETE
// auto-sync fast path (docs/DESIGN_WRITE_CAPTURE.md). Pure plan tests —
// no views, no sync; the end-to-end path lives in the integration suite.

#include "catch.hpp"
#include "dbsp_write_capture.hpp"
#include "duckdb.hpp"
#include "duckdb/parser/parser.hpp"

using namespace dbsp_native;

namespace {

struct PlanFixture {
  duckdb::DuckDB db{nullptr};
  duckdb::Connection con{db};

  PlanFixture() {
    exec("CREATE TABLE t (id INTEGER, val INTEGER, name VARCHAR)");
    exec("CREATE TABLE ti (id INTEGER PRIMARY KEY, val INTEGER)");
    exec("CREATE TABLE tl (id INTEGER, xs INTEGER[])");
    exec("CREATE TABLE trid (rowid INTEGER, val INTEGER)");
  }

  void exec(const std::string &sql) {
    auto res = con.Query(sql);
    REQUIRE_FALSE(res->HasError());
  }

  std::unique_ptr<WriteCapturePlan> plan_for(const std::string &sql,
                                             const std::string &table) {
    duckdb::Parser parser;
    parser.ParseQuery(sql);
    REQUIRE(parser.statements.size() == 1);
    std::unique_ptr<WriteCapturePlan> out;
    auto &ctx = *con.context;
    ctx.RunFunctionInTransaction([&] {
      auto entry = resolve_table_entry(ctx, table);
      REQUIRE(entry);
      out = plan_write_capture(ctx, *parser.statements[0], *entry,
                               canonical_table_key(*entry));
    });
    return out;
  }
};

} // namespace

TEST_CASE("write capture: plain UPDATE is capturable", "[write_capture]") {
  PlanFixture fx;
  auto plan = fx.plan_for("UPDATE t SET val = val + 1 WHERE id = 3", "t");
  REQUIRE(plan);
  REQUIRE(plan->kind == WriteCapturePlan::Kind::Update);
  REQUIRE(plan->n_cols == 3);
  // projection: rowid, 3 old columns, 1 new value for physical column 1
  REQUIRE(plan->set_cols.size() == 1);
  REQUIRE(plan->set_cols[0].first == 1);
  REQUIRE(plan->set_cols[0].second == 4);
  REQUIRE(plan->capture_sql.find("SELECT rowid, \"id\", \"val\", \"name\"") ==
          0);
  REQUIRE(plan->capture_sql.find("CAST((") != std::string::npos);
  REQUIRE(plan->capture_sql.find("AS INTEGER)") != std::string::npos);
  REQUIRE(plan->capture_sql.find("WHERE") != std::string::npos);
  // capture query must actually run
  auto res = fx.con.Query(plan->capture_sql);
  REQUIRE_FALSE(res->HasError());
}

TEST_CASE("write capture: multi-column SET maps every projection slot",
          "[write_capture]") {
  PlanFixture fx;
  auto plan =
      fx.plan_for("UPDATE t SET name = 'x', id = id * 2 WHERE val > 0", "t");
  REQUIRE(plan);
  REQUIRE(plan->set_cols.size() == 2);
  REQUIRE(plan->set_cols[0].first == 2);  // name
  REQUIRE(plan->set_cols[0].second == 4); // first new-value slot
  REQUIRE(plan->set_cols[1].first == 0);  // id
  REQUIRE(plan->set_cols[1].second == 5);
}

TEST_CASE("write capture: UPDATE without WHERE captures whole table",
          "[write_capture]") {
  PlanFixture fx;
  auto plan = fx.plan_for("UPDATE t SET val = 0", "t");
  REQUIRE(plan);
  REQUIRE(plan->capture_sql.find("WHERE") == std::string::npos);
}

TEST_CASE("write capture: aliased UPDATE keeps the alias in FROM",
          "[write_capture]") {
  PlanFixture fx;
  auto plan = fx.plan_for("UPDATE t AS x SET val = x.val + 1 WHERE x.id = 1",
                          "t");
  REQUIRE(plan);
  REQUIRE(plan->capture_sql.find(" AS \"x\"") != std::string::npos);
  auto res = fx.con.Query(plan->capture_sql);
  REQUIRE_FALSE(res->HasError());
}

TEST_CASE("write capture: rejected UPDATE shapes", "[write_capture]") {
  PlanFixture fx;
  SECTION("UPDATE ... FROM") {
    REQUIRE_FALSE(fx.plan_for(
        "UPDATE t SET val = ti.val FROM ti WHERE t.id = ti.id", "t"));
  }
  SECTION("RETURNING") {
    REQUIRE_FALSE(
        fx.plan_for("UPDATE t SET val = 1 WHERE id = 1 RETURNING *", "t"));
  }
  SECTION("CTE") {
    REQUIRE_FALSE(fx.plan_for(
        "WITH c AS (SELECT 1 AS x) UPDATE t SET val = 1 WHERE id = 1", "t"));
  }
  SECTION("SET DEFAULT") {
    REQUIRE_FALSE(fx.plan_for("UPDATE t SET val = DEFAULT WHERE id = 1", "t"));
  }
  SECTION("subquery in WHERE") {
    REQUIRE_FALSE(fx.plan_for(
        "UPDATE t SET val = 1 WHERE id IN (SELECT id FROM ti)", "t"));
  }
  SECTION("subquery in SET") {
    REQUIRE_FALSE(fx.plan_for(
        "UPDATE t SET val = (SELECT MAX(val) FROM ti) WHERE id = 1", "t"));
  }
  SECTION("prepared-statement parameter") {
    REQUIRE_FALSE(fx.plan_for("UPDATE t SET val = $1 WHERE id = 1", "t"));
  }
  SECTION("SET column in an index (update_is_del_and_insert)") {
    REQUIRE_FALSE(fx.plan_for("UPDATE ti SET id = id + 1 WHERE val = 1", "ti"));
  }
  SECTION("non-indexed column of an indexed table is fine") {
    REQUIRE(fx.plan_for("UPDATE ti SET val = val + 1 WHERE id = 1", "ti"));
  }
  SECTION("LIST column (no regular update support)") {
    REQUIRE_FALSE(fx.plan_for("UPDATE tl SET xs = [1] WHERE id = 1", "tl"));
  }
  SECTION("user column named rowid shadows the guard pseudo-column") {
    REQUIRE_FALSE(
        fx.plan_for("UPDATE trid SET val = 1 WHERE rowid = 1", "trid"));
  }
}

TEST_CASE("write capture: plain DELETE is capturable", "[write_capture]") {
  PlanFixture fx;
  auto plan = fx.plan_for("DELETE FROM t WHERE id = 3", "t");
  REQUIRE(plan);
  REQUIRE(plan->kind == WriteCapturePlan::Kind::Delete);
  REQUIRE(plan->n_cols == 3);
  REQUIRE(plan->set_cols.empty());
  auto res = fx.con.Query(plan->capture_sql);
  REQUIRE_FALSE(res->HasError());
}

TEST_CASE("write capture: DELETE without WHERE (delete-all) is capturable",
          "[write_capture]") {
  PlanFixture fx;
  REQUIRE(fx.plan_for("DELETE FROM t", "t"));
}

TEST_CASE("write capture: rejected DELETE shapes", "[write_capture]") {
  PlanFixture fx;
  SECTION("USING") {
    REQUIRE_FALSE(
        fx.plan_for("DELETE FROM t USING ti WHERE t.id = ti.id", "t"));
  }
  SECTION("RETURNING") {
    REQUIRE_FALSE(fx.plan_for("DELETE FROM t WHERE id = 1 RETURNING *", "t"));
  }
  SECTION("subquery WHERE") {
    REQUIRE_FALSE(
        fx.plan_for("DELETE FROM t WHERE id IN (SELECT id FROM ti)", "t"));
  }
}

TEST_CASE("write capture: bound-plan stability vetting", "[write_capture]") {
  PlanFixture fx;
  auto vet = [&](const std::string &select_sql) {
    auto plan = fx.con.ExtractPlan(select_sql);
    REQUIRE(plan);
    return bound_plan_consistent(*plan);
  };
  REQUIRE(vet("SELECT rowid, id, val, name FROM t WHERE id = 1"));
  REQUIRE(vet("SELECT rowid, id, CAST((val + 1) AS INTEGER) FROM t"));
  REQUIRE_FALSE(vet("SELECT rowid, id FROM t WHERE random() > 0.5"));
  REQUIRE_FALSE(vet("SELECT rowid, id, CAST((random() * 10) AS INTEGER) FROM t"));
}

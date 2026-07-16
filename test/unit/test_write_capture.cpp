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
  // VALUES lists live in LogicalExpressionGet::expressions (shadows the
  // base member) — volatile functions there must still be caught
  REQUIRE_FALSE(vet("SELECT * FROM (VALUES (random()), (0.5)) v(x)"));
  REQUIRE(vet("SELECT * FROM (VALUES (1), (2)) v(x)"));
}

namespace {

std::unique_ptr<WriteCapturePlan> insert_plan_for(PlanFixture &fx,
                                                  const std::string &sql,
                                                  const std::string &table) {
  duckdb::Parser parser;
  parser.ParseQuery(sql);
  REQUIRE(parser.statements.size() == 1);
  std::unique_ptr<WriteCapturePlan> out;
  auto &ctx = *fx.con.context;
  ctx.RunFunctionInTransaction([&] {
    auto entry = resolve_table_entry(ctx, table);
    REQUIRE(entry);
    out = plan_insert_capture(*parser.statements[0], *entry);
  });
  return out;
}

} // namespace

TEST_CASE("insert capture: plain VALUES is capturable", "[write_capture]") {
  PlanFixture fx;
  auto plan = insert_plan_for(
      fx, "INSERT INTO t VALUES (1, 2, 'a'), (3, 4, 'b')", "t");
  REQUIRE(plan);
  REQUIRE(plan->kind == WriteCapturePlan::Kind::Insert);
  REQUIRE(plan->n_cols == 3);
  REQUIRE(plan->set_cols.empty());
  auto res = fx.con.Query(plan->capture_sql);
  REQUIRE_FALSE(res->HasError());
  REQUIRE(res->RowCount() == 2);
}

TEST_CASE("insert capture: full-cover permuted column list reorders",
          "[write_capture]") {
  PlanFixture fx;
  auto plan = insert_plan_for(
      fx, "INSERT INTO t (name, val, id) VALUES ('a', 2, 1)", "t");
  REQUIRE(plan);
  auto res = fx.con.Query(plan->capture_sql);
  REQUIRE_FALSE(res->HasError());
  // projection is table order: id, val, name
  REQUIRE(res->GetValue(0, 0).ToString() == "1");
  REQUIRE(res->GetValue(1, 0).ToString() == "2");
  REQUIRE(res->GetValue(2, 0).ToString() == "a");
}

TEST_CASE("insert capture: rejected INSERT shapes", "[write_capture]") {
  PlanFixture fx;
  SECTION("INSERT ... SELECT over a base table is now capturable") {
    REQUIRE(insert_plan_for(fx, "INSERT INTO t SELECT * FROM t", "t"));
  }
  SECTION("partial column list is now capturable (defaults resolved)") {
    REQUIRE(insert_plan_for(fx, "INSERT INTO t (id) VALUES (1)", "t"));
  }
  SECTION("DEFAULT VALUES") {
    REQUIRE_FALSE(insert_plan_for(fx, "INSERT INTO t DEFAULT VALUES", "t"));
  }
  SECTION("DEFAULT inside VALUES") {
    REQUIRE_FALSE(insert_plan_for(
        fx, "INSERT INTO t VALUES (1, DEFAULT, 'a')", "t"));
  }
  SECTION("RETURNING") {
    REQUIRE_FALSE(insert_plan_for(
        fx, "INSERT INTO t VALUES (1, 2, 'a') RETURNING *", "t"));
  }
  SECTION("BY NAME") {
    REQUIRE_FALSE(insert_plan_for(
        fx, "INSERT INTO t BY NAME (SELECT 1 AS id)", "t"));
  }
  SECTION("subquery expression") {
    REQUIRE_FALSE(insert_plan_for(
        fx, "INSERT INTO t VALUES ((SELECT 1), 2, 'a')", "t"));
  }
  SECTION("parameter") {
    REQUIRE_FALSE(
        insert_plan_for(fx, "INSERT INTO t VALUES ($1, 2, 'a')", "t"));
  }
}

TEST_CASE("insert capture: partial column list pads defaults and NULLs",
          "[write_capture]") {
  PlanFixture fx;
  fx.exec("CREATE TABLE td (id INTEGER, val INTEGER DEFAULT 42, "
          "name VARCHAR)");
  auto plan = insert_plan_for(fx, "INSERT INTO td (id) VALUES (7)", "td");
  REQUIRE(plan);
  auto res = fx.con.Query(plan->capture_sql);
  REQUIRE_FALSE(res->HasError());
  REQUIRE(res->GetValue(0, 0).ToString() == "7");
  REQUIRE(res->GetValue(1, 0).ToString() == "42"); // declared DEFAULT
  REQUIRE(res->GetValue(2, 0).IsNull());           // no default -> NULL
}

TEST_CASE("insert capture: volatile default expression is caught downstream",
          "[write_capture]") {
  PlanFixture fx;
  fx.exec("CREATE SEQUENCE sq");
  fx.exec("CREATE TABLE ts (id INTEGER DEFAULT nextval('sq'), val INTEGER)");
  auto plan = insert_plan_for(fx, "INSERT INTO ts (val) VALUES (1)", "ts");
  // the parse-level walk cannot see stability; the bound-plan check must
  REQUIRE(plan);
  auto logical = fx.con.ExtractPlan(plan->capture_sql);
  REQUIRE(logical);
  REQUIRE_FALSE(bound_plan_consistent(*logical));
}

TEST_CASE("insert capture: INSERT ... SELECT shapes", "[write_capture]") {
  PlanFixture fx;
  SECTION("plain SELECT over a base table is capturable") {
    auto plan = insert_plan_for(
        fx, "INSERT INTO t SELECT id + 100, val, name FROM t WHERE val > 0",
        "t");
    REQUIRE(plan);
    auto res = fx.con.Query(plan->capture_sql);
    REQUIRE_FALSE(res->HasError());
  }
  SECTION("join + aggregate source is capturable") {
    REQUIRE(insert_plan_for(
        fx,
        "INSERT INTO ti SELECT t.id, SUM(t.val) FROM t "
        "JOIN ti ON t.id = ti.id GROUP BY t.id",
        "ti"));
  }
  SECTION("UNION source is capturable") {
    REQUIRE(insert_plan_for(
        fx, "INSERT INTO ti SELECT 1, 2 UNION SELECT 3, 4", "ti"));
  }
  SECTION("LIMIT rejected: row choice depends on scan order") {
    REQUIRE_FALSE(insert_plan_for(
        fx, "INSERT INTO t SELECT * FROM t LIMIT 1", "t"));
  }
  SECTION("TABLESAMPLE rejected") {
    REQUIRE_FALSE(insert_plan_for(
        fx, "INSERT INTO t SELECT * FROM t TABLESAMPLE 50%", "t"));
  }
  SECTION("USING SAMPLE rejected") {
    REQUIRE_FALSE(insert_plan_for(
        fx, "INSERT INTO t SELECT * FROM t USING SAMPLE 1", "t"));
  }
  SECTION("table function rejected (no stability metadata)") {
    REQUIRE_FALSE(insert_plan_for(
        fx, "INSERT INTO ti SELECT i, i FROM range(3) r(i)", "ti"));
  }
  SECTION("window function rejected (tie order not repeatable)") {
    REQUIRE_FALSE(insert_plan_for(
        fx, "INSERT INTO ti SELECT id, row_number() OVER (ORDER BY val) "
            "FROM t",
        "ti"));
  }
  SECTION("CTE inside the SELECT rejected") {
    REQUIRE_FALSE(insert_plan_for(
        fx, "INSERT INTO ti WITH c AS (SELECT 1 a, 2 b FROM t) "
            "SELECT * FROM c",
        "ti"));
  }
  SECTION("subquery source ref is capturable") {
    REQUIRE(insert_plan_for(
        fx, "INSERT INTO ti SELECT * FROM (SELECT id, val FROM t) s", "ti"));
  }
}

namespace {

std::unique_ptr<WriteCapturePlan> upsert_plan_for(PlanFixture &fx,
                                                  const std::string &sql,
                                                  const std::string &table) {
  duckdb::Parser parser;
  parser.ParseQuery(sql);
  REQUIRE(parser.statements.size() == 1);
  std::unique_ptr<WriteCapturePlan> out;
  auto &ctx = *fx.con.context;
  ctx.RunFunctionInTransaction([&] {
    auto entry = resolve_table_entry(ctx, table);
    REQUIRE(entry);
    out = plan_upsert_capture(ctx, *parser.statements[0], *entry,
                              canonical_table_key(*entry));
  });
  return out;
}

} // namespace

TEST_CASE("upsert capture: DO UPDATE probe plan", "[write_capture]") {
  PlanFixture fx;
  fx.exec("INSERT INTO ti VALUES (1, 10)");
  auto plan = upsert_plan_for(fx,
                              "INSERT INTO ti VALUES (1, 5), (2, 6) "
                              "ON CONFLICT (id) DO UPDATE SET "
                              "val = excluded.val",
                              "ti");
  REQUIRE(plan);
  REQUIRE(plan->kind == WriteCapturePlan::Kind::Upsert);
  REQUIRE(plan->n_cols == 2);
  REQUIRE(plan->insert_slot_base == 3);
  REQUIRE(plan->set_cols.size() == 1);
  REQUIRE(plan->set_cols[0].first == 1);  // val
  REQUIRE(plan->set_cols[0].second == 5); // 2n+1
  auto res = fx.con.Query(plan->capture_sql);
  REQUIRE_FALSE(res->HasError());
  REQUIRE(res->RowCount() == 2);
  // key 1 matches (rowid set), key 2 does not (rowid NULL)
  size_t matched = 0, unmatched = 0;
  for (size_t r = 0; r < res->RowCount(); r++) {
    (res->GetValue(0, r).IsNull() ? unmatched : matched)++;
  }
  REQUIRE(matched == 1);
  REQUIRE(unmatched == 1);
}

TEST_CASE("upsert capture: DO NOTHING has no SET slots", "[write_capture]") {
  PlanFixture fx;
  auto plan = upsert_plan_for(
      fx, "INSERT INTO ti VALUES (1, 5) ON CONFLICT (id) DO NOTHING", "ti");
  REQUIRE(plan);
  REQUIRE(plan->set_cols.empty());
}

TEST_CASE("upsert capture: rejected shapes", "[write_capture]") {
  PlanFixture fx;
  SECTION("OR REPLACE") {
    REQUIRE_FALSE(
        upsert_plan_for(fx, "INSERT OR REPLACE INTO ti VALUES (1, 5)", "ti"));
  }
  SECTION("no explicit conflict target") {
    REQUIRE_FALSE(upsert_plan_for(
        fx, "INSERT INTO ti VALUES (1, 5) ON CONFLICT DO NOTHING", "ti"));
  }
  SECTION("DO UPDATE ... WHERE condition") {
    REQUIRE_FALSE(upsert_plan_for(
        fx,
        "INSERT INTO ti VALUES (1, 5) ON CONFLICT (id) DO UPDATE SET "
        "val = excluded.val WHERE ti.val < 3",
        "ti"));
  }
  SECTION("SET on the (indexed) conflict column") {
    REQUIRE_FALSE(upsert_plan_for(
        fx,
        "INSERT INTO ti VALUES (1, 5) ON CONFLICT (id) DO UPDATE SET "
        "id = excluded.id + 100",
        "ti"));
  }
  SECTION("conflict column not provided by the source") {
    REQUIRE_FALSE(upsert_plan_for(
        fx,
        "INSERT INTO ti (val) VALUES (5) ON CONFLICT (id) DO NOTHING",
        "ti"));
  }
  SECTION("bare target-column SET declines at query time (ambiguous)") {
    auto plan = upsert_plan_for(
        fx,
        "INSERT INTO ti VALUES (1, 5) ON CONFLICT (id) DO UPDATE SET "
        "val = val + 1",
        "ti");
    REQUIRE(plan); // parse-level plan builds...
    auto res = fx.con.Query(plan->capture_sql);
    REQUIRE(res->HasError()); // ...but the ambiguous reference declines it
  }
}

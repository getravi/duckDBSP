#include "catch.hpp"
#include "dbsp_duckdb_types.hpp"
#include "dbsp_sql_parser.hpp"

using namespace dbsp_native;

TEST_CASE("Parse non-recursive CTE", "[cte][parser][p5]") {
  DBSPSqlParser parser;

  SECTION("Simple WITH clause") {
    auto res = parser.parse(
        "WITH high_earners AS (SELECT * FROM employees WHERE salary > 50000) "
        "SELECT * FROM high_earners");
    INFO("Error: " << res.error);
    REQUIRE(res.success);
    REQUIRE(res.view_def.ctes.size() == 1);
    REQUIRE(res.view_def.ctes[0].cte_name == "high_earners");
    REQUIRE(!res.view_def.ctes[0].cte_sql.empty());
  }

  SECTION("Multiple CTEs") {
    auto res = parser.parse(
        "WITH a AS (SELECT id FROM t1), b AS (SELECT id FROM t2) "
        "SELECT * FROM a JOIN b ON a.id = b.id");
    INFO("Error: " << res.error);
    REQUIRE(res.success);
    REQUIRE(res.view_def.ctes.size() == 2);
    // Check that CTE names are captured
    bool found_a = false, found_b = false;
    for (auto &cte : res.view_def.ctes) {
      if (cte.cte_name == "a")
        found_a = true;
      if (cte.cte_name == "b")
        found_b = true;
    }
    REQUIRE(found_a);
    REQUIRE(found_b);
  }
}

TEST_CASE("Parse subquery in FROM", "[subquery][parser][p5]") {
  DBSPSqlParser parser;

  SECTION("Simple derived table") {
    auto res = parser.parse(
        "SELECT * FROM (SELECT id, name FROM employees) sub");
    INFO("Error: " << res.error);
    REQUIRE(res.success);
    REQUIRE(res.view_def.derived_tables.size() == 1);
    REQUIRE(res.view_def.derived_tables[0].alias == "sub");
    REQUIRE(!res.view_def.derived_tables[0].subquery_sql.empty());
  }

  SECTION("Derived table with filter") {
    auto res = parser.parse(
        "SELECT * FROM (SELECT id, salary FROM employees WHERE salary > 1000) "
        "AS filtered");
    INFO("Error: " << res.error);
    REQUIRE(res.success);
    REQUIRE(res.view_def.derived_tables.size() == 1);
    REQUIRE(res.view_def.derived_tables[0].alias == "filtered");
  }
}

TEST_CASE("CTE source tables resolved correctly", "[cte][parser][p5]") {
  DBSPSqlParser parser;

  auto res = parser.parse(
      "WITH dept_stats AS (SELECT dept, COUNT(*) as cnt FROM employees GROUP "
      "BY dept) "
      "SELECT * FROM dept_stats WHERE cnt > 5");
  INFO("Error: " << res.error);
  REQUIRE(res.success);

  // The CTE references 'employees', the outer query references 'dept_stats'
  // Source tables should include 'dept_stats' (CTE name)
  bool has_dept_stats = false;
  for (auto &src : res.view_def.source_tables) {
    if (src == "dept_stats")
      has_dept_stats = true;
  }
  REQUIRE(has_dept_stats);
}

TEST_CASE("ViewFactory handles CTE type", "[cte][factory][p5]") {
  // Create a CTE-type view def that falls through to filter
  ParsedViewDef def;
  def.type = ParsedViewDef::ViewType::CTE;
  def.view_name = "cte_test";
  def.sql = "test";
  def.source_tables = {"t"};
  def.select_all = true;

  // Add a dummy CTE (ViewFactory won't create it, just falls through)
  ParsedViewDef::CTEInfo cte;
  cte.cte_name = "my_cte";
  cte.cte_sql = "SELECT * FROM t";
  def.ctes.push_back(cte);

  TableSchema schema;
  schema.table_name = "t";
  schema.columns = {{"id", duckdb::LogicalType::INTEGER},
                    {"val", duckdb::LogicalType::VARCHAR}};

  std::unordered_map<std::string, TableSchema> schemas;
  schemas["t"] = schema;

  auto view = ViewFactory::create_view(def, schemas);
  REQUIRE(view != nullptr);
}

TEST_CASE("Derived table creates source entry", "[subquery][parser][p5]") {
  DBSPSqlParser parser;

  auto res = parser.parse(
      "SELECT sub.id FROM (SELECT id FROM employees) sub WHERE sub.id > 10");
  INFO("Error: " << res.error);
  REQUIRE(res.success);
  REQUIRE(res.view_def.derived_tables.size() == 1);

  // The derived table alias should appear in source tables
  bool found = false;
  for (auto &src : res.view_def.source_tables) {
    if (src == "sub")
      found = true;
  }
  REQUIRE(found);
}

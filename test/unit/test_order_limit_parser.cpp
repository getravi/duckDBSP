#include "catch.hpp"
#include "dbsp_sql_parser.hpp"

using namespace dbsp_native;

TEST_CASE("SQL Parser - ORDER BY", "[sql_parser][sort]") {
  DBSPSqlParser parser;

  SECTION("Simple ORDER BY") {
    auto result =
        parser.parse("SELECT * FROM users ORDER BY age", "sorted_users");

    REQUIRE(result.success);
    REQUIRE(result.view_def.type == ParsedViewDef::ViewType::SORT);
    REQUIRE(result.view_def.sort_columns.size() == 1);
    REQUIRE(result.view_def.sort_columns[0].column_name == "age");
    REQUIRE(result.view_def.sort_columns[0].ascending); // Default ASC
  }

  SECTION("ORDER BY DESC") {
    auto result =
        parser.parse("SELECT * FROM users ORDER BY age DESC", "desc_users");

    REQUIRE(result.success);
    REQUIRE(result.view_def.sort_columns.size() == 1);
    REQUIRE(result.view_def.sort_columns[0].column_name == "age");
    REQUIRE_FALSE(result.view_def.sort_columns[0].ascending);
  }

  SECTION("Multiple columns") {
    auto result = parser.parse(
        "SELECT * FROM users ORDER BY age DESC, name ASC", "multi_sort");

    REQUIRE(result.success);
    REQUIRE(result.view_def.sort_columns.size() == 2);
    REQUIRE(result.view_def.sort_columns[0].column_name == "age");
    REQUIRE_FALSE(result.view_def.sort_columns[0].ascending);
    REQUIRE(result.view_def.sort_columns[1].column_name == "name");
    REQUIRE(result.view_def.sort_columns[1].ascending);
  }
}

TEST_CASE("SQL Parser - LIMIT", "[sql_parser][limit]") {
  DBSPSqlParser parser;

  SECTION("LIMIT only") {
    auto result = parser.parse("SELECT * FROM users LIMIT 10", "limited_users");

    REQUIRE(result.success);
    REQUIRE(result.view_def.type == ParsedViewDef::ViewType::LIMIT);
    REQUIRE(result.view_def.limit.has_value());
    REQUIRE(result.view_def.limit.value() == 10);
    REQUIRE(!result.view_def.offset.has_value());
  }

  SECTION("LIMIT and OFFSET") {
    auto result =
        parser.parse("SELECT * FROM users LIMIT 10 OFFSET 5", "paged_users");

    REQUIRE(result.success);
    REQUIRE(result.view_def.type == ParsedViewDef::ViewType::LIMIT);
    REQUIRE(result.view_def.limit.value() == 10);
    REQUIRE(result.view_def.offset.has_value());
    REQUIRE(result.view_def.offset.value() == 5);
  }

  SECTION("ORDER BY and LIMIT") {
    auto result =
        parser.parse("SELECT * FROM users ORDER BY age LIMIT 5", "top_users");

    REQUIRE(result.success);
    REQUIRE(result.view_def.type ==
            ParsedViewDef::ViewType::LIMIT); // LIMIT priority
    REQUIRE(result.view_def.limit.value() == 5);
    REQUIRE(result.view_def.sort_columns.size() == 1);
    REQUIRE(result.view_def.sort_columns[0].column_name == "age");
  }
}

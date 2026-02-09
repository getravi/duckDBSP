#include "catch.hpp"
#include "dbsp_sql_parser.hpp"

using namespace dbsp_native;

TEST_CASE("Set Operation Parser", "[parser][set_ops]") {
  DBSPSqlParser parser;

  SECTION("UNION ALL") {
    auto res = parser.parse("SELECT a FROM t1 UNION ALL SELECT a FROM t2");
    REQUIRE(res.success);
    REQUIRE(res.view_def.type == ParsedViewDef::ViewType::SET_OP);
    REQUIRE(res.view_def.set_op.has_value());
    REQUIRE(res.view_def.set_op->type == duckdb::SetOperationType::UNION);
    REQUIRE(res.view_def.set_op->all == true);
  }

  SECTION("UNION (Distinct)") {
    auto res = parser.parse("SELECT a FROM t1 UNION SELECT a FROM t2");
    REQUIRE(res.success);
    REQUIRE(res.view_def.type == ParsedViewDef::ViewType::SET_OP);
    REQUIRE(res.view_def.set_op.has_value());
    REQUIRE(res.view_def.set_op->type == duckdb::SetOperationType::UNION);
    REQUIRE(res.view_def.set_op->all == false);
  }

  SECTION("INTERSECT") {
    auto res = parser.parse("SELECT a FROM t1 INTERSECT SELECT a FROM t2");
    REQUIRE(res.success);
    REQUIRE(res.view_def.type == ParsedViewDef::ViewType::SET_OP);
    REQUIRE(res.view_def.set_op->type == duckdb::SetOperationType::INTERSECT);
  }

  SECTION("EXCEPT") {
    auto res = parser.parse("SELECT a FROM t1 EXCEPT SELECT a FROM t2");
    REQUIRE(res.success);
    REQUIRE(res.view_def.type == ParsedViewDef::ViewType::SET_OP);
    REQUIRE(res.view_def.set_op->type == duckdb::SetOperationType::EXCEPT);
  }
}

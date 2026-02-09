#include "../test_helpers.hpp"
#include "dbsp_sql_parser.hpp"

using namespace dbsp_native;

static DuckDBRow make_order_row(double price, int qty) {
  DuckDBRow row;
  row.columns.push_back(duckdb::Value(price));
  row.columns.push_back(duckdb::Value(qty));
  return row;
}

TEST_CASE("Advanced Projection arithmetic", "[projection]") {
  std::unordered_map<std::string, TableSchema> schemas;
  TableSchema schema;
  schema.table_name = "orders";
  schema.columns.push_back({"price", duckdb::LogicalType::DOUBLE});
  schema.columns.push_back({"quantity", duckdb::LogicalType::INTEGER});
  schemas["orders"] = schema;

  SECTION("Basic multiplication projection") {
    ParsedViewDef def;
    def.type = ParsedViewDef::ViewType::PROJECT;
    def.view_name = "v_total";
    def.sql = "SELECT price * quantity as total FROM orders";
    def.source_tables = {"orders"};

    ParsedViewDef::ProjectionExpression expr;
    expr.left_col = "price";
    expr.right_col = "quantity";
    expr.op = "*";
    expr.alias = "total";
    def.projection_exprs.push_back(expr);

    auto view = ViewFactory::create_view(def, schemas);
    REQUIRE(view != nullptr);

    DuckDBZSet changes;
    changes.insert(make_order_row(10.0, 5), 1); // 50.0
    view->apply_changes("orders", changes);

    const auto &result = view->get_result();
    DuckDBRow expected;
    expected.columns.push_back(duckdb::Value(50.0));
    REQUIRE((result.get(expected) == 1LL));
  }

  SECTION("Null handling in arithmetic") {
    ParsedViewDef def;
    def.type = ParsedViewDef::ViewType::PROJECT;
    def.view_name = "v_null";
    def.sql = "SELECT price + quantity as total FROM orders";
    def.source_tables = {"orders"};

    ParsedViewDef::ProjectionExpression expr;
    expr.left_col = "price";
    expr.right_col = "quantity";
    expr.op = "+";
    expr.alias = "total";
    def.projection_exprs.push_back(expr);

    auto view = ViewFactory::create_view(def, schemas);
    REQUIRE(view != nullptr);

    DuckDBZSet changes;
    DuckDBRow null_row;
    null_row.columns.push_back(duckdb::Value(duckdb::LogicalType::DOUBLE));
    null_row.columns.push_back(duckdb::Value(10));
    changes.insert(null_row, 1);

    view->apply_changes("orders", changes);

    DuckDBRow expected;
    expected.columns.push_back(duckdb::Value(duckdb::LogicalType::DOUBLE));
    REQUIRE((view->get_result().get(expected) == 1LL));
  }
}

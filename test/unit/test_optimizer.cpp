#define CATCH_CONFIG_MAIN
#include "catch.hpp"
#include "dbsp_optimizer.hpp"
#include "dbsp_sql_parser.hpp"

using namespace dbsp_native;

TEST_CASE("Optimizer: Filter Combination", "[optimizer]") {
  DBSPOptimizer optimizer;
  ParsedViewDef def;
  def.type = ParsedViewDef::ViewType::FILTER;

  // Add multiple filters
  // FilterInfo: {column_idx, column_name, op, value}
  def.filters.push_back({0, "col1", ">", duckdb::Value("10")});
  def.filters.push_back({1, "col2", "<", duckdb::Value("20")});

  auto optimized = optimizer.optimize(def);

  // The current implementation just updates stats, as filters are already
  // combined in the vector
  REQUIRE(optimizer.stats().filters_combined == 1);
}

TEST_CASE("Optimizer: Filter Pushdown through JOIN", "[optimizer]") {
  DBSPOptimizer optimizer;
  ParsedViewDef def;
  def.type = ParsedViewDef::ViewType::JOIN;

  ParsedViewDef::JoinInfo join_info;
  join_info.left_table = "table1";
  join_info.right_table = "table2";
  def.join_info = join_info;

  // table1 filter
  def.filters.push_back({0, "table1.col1", ">", duckdb::Value("10")});
  // table2 filter
  def.filters.push_back({1, "table2.col2", "<", duckdb::Value("20")});
  // complex/unknown filter
  def.filters.push_back({2, "col3", "=", duckdb::Value("30")});

  auto optimized = optimizer.optimize(def);

  REQUIRE(optimizer.stats().filters_pushed_down == 2);

  // Verify filters moved
  REQUIRE(optimized.left_pushed_filters.size() == 1);
  REQUIRE(optimized.left_pushed_filters[0].column_name == "col1");

  REQUIRE(optimized.right_pushed_filters.size() == 1);
  REQUIRE(optimized.right_pushed_filters[0].column_name == "col2");

  // Verify remaining filter
  REQUIRE(optimized.filters.size() == 1);
  REQUIRE(optimized.filters[0].column_name == "col3");
}

TEST_CASE("Optimizer: Projection Pruning", "[optimizer]") {
  DBSPOptimizer optimizer;
  ParsedViewDef def;
  def.type = ParsedViewDef::ViewType::PROJECT;
  def.select_all = false;

  // Project specific columns
  def.project_column_names = {"col1", "col2"};

  // Add a filter using another column
  def.filters.push_back({2, "col3", ">", duckdb::Value("0")});

  auto optimized = optimizer.optimize(def);

  // Verify required columns
  REQUIRE(optimized.required_columns.size() == 3);

  std::set<std::string> required(optimized.required_columns.begin(),
                                 optimized.required_columns.end());
  REQUIRE(required.count("col1"));
  REQUIRE(required.count("col2"));
  REQUIRE(required.count("col3"));
}

TEST_CASE("Optimizer: Pruning with Aggregates", "[optimizer]") {
  DBSPOptimizer optimizer;
  ParsedViewDef def;
  def.type = ParsedViewDef::ViewType::AGGREGATE;
  def.select_all = false;

  // Group by col1
  def.group_by_names = {"col1"};
  // Simulate SELECT col1 ...
  def.project_column_names.push_back("col1");

  // Aggregate on col2
  ParsedViewDef::AggInfo agg;
  agg.value_column_name = "col2";
  def.aggregates.push_back(agg);

  auto optimized = optimizer.optimize(def);

  // Verify required columns
  REQUIRE(optimized.required_columns.size() == 2);

  std::set<std::string> required(optimized.required_columns.begin(),
                                 optimized.required_columns.end());
  REQUIRE(required.count("col1"));
  REQUIRE(required.count("col2"));
}

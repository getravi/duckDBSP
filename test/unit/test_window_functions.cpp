#include "catch.hpp"
#include "dbsp_duckdb_types.hpp"
#include "dbsp_sql_parser.hpp"
#include "dbsp_window_view.hpp"

using namespace dbsp_native;

TEST_CASE("Window Functions Parser", "[window][parser]") {
  DBSPSqlParser parser;

  SECTION("Parse ROW_NUMBER()") {
    auto res = parser.parse(
        "SELECT ROW_NUMBER() OVER (PARTITION BY a ORDER BY b) as rn FROM t");
    INFO("Error: " << res.error);
    REQUIRE(res.success);
    REQUIRE(res.view_def.type == ParsedViewDef::ViewType::WINDOW);
    REQUIRE(res.view_def.windows.size() == 1);
    REQUIRE(res.view_def.windows[0].function == "ROW_NUMBER");
    REQUIRE(res.view_def.windows[0].alias == "rn");
    REQUIRE(res.view_def.windows[0].partition_by.size() == 1);
    REQUIRE(res.view_def.windows[0].partition_by[0] == "a");
    REQUIRE(res.view_def.windows[0].order_by.size() == 1);
    REQUIRE(res.view_def.windows[0].order_by[0].column_name == "b");
  }
}

TEST_CASE("NativeWindowView Execution", "[window][execution]") {
  // Schema: user_id (int), score (int)
  TableSchema schema;
  schema.columns = {{"user_id", duckdb::LogicalType::INTEGER},
                    {"score", duckdb::LogicalType::INTEGER}};

  // Window: ROW_NUMBER() OVER (PARTITION BY user_id ORDER BY score DESC)
  std::vector<NativeWindowView::WindowDef> windows;
  NativeWindowView::WindowDef win;
  win.function = "ROW_NUMBER";
  win.alias = "rn";
  win.partition_indices = {0}; // user_id
  NativeSortView::SortColumn sc;
  sc.column_idx = 1; // score
  sc.ascending = false;
  sc.nulls_first = false;
  win.sort_columns.push_back(sc);
  windows.push_back(win);

  TableSchema result_schema = schema;
  result_schema.columns.push_back({"rn", duckdb::LogicalType::BIGINT});

  NativeWindowView view("win_view", "sql", "t", result_schema, schema, windows);

  // Data:
  // User 1: 100, 200
  // User 2: 50
  DuckDBZSet input;

  DuckDBRow r1;
  r1.columns = {duckdb::Value(1), duckdb::Value(100)};
  input.insert(r1, 1);

  DuckDBRow r2;
  r2.columns = {duckdb::Value(1), duckdb::Value(200)};
  input.insert(r2, 1);

  DuckDBRow r3;
  r3.columns = {duckdb::Value(2), duckdb::Value(50)};
  input.insert(r3, 1);

  view.apply_changes("t", input);

  auto result = view.get_result();

  // Check results
  // User 1: 200 -> rn 1, 100 -> rn 2
  // User 2: 50 -> rn 1

  DuckDBRow out1;
  out1.columns = {duckdb::Value(1), duckdb::Value(200),
                  duckdb::Value((int64_t)1)};
  REQUIRE(result.get(out1) == 1);

  DuckDBRow out2;
  out2.columns = {duckdb::Value(1), duckdb::Value(100),
                  duckdb::Value((int64_t)2)};
  REQUIRE(result.get(out2) == 1);

  DuckDBRow out3;
  out3.columns = {duckdb::Value(2), duckdb::Value(50),
                  duckdb::Value((int64_t)1)};
  REQUIRE(result.get(out3) == 1);

  // Incremental Update: Add User 1 with score 300 (should become rn 1)
  DuckDBZSet delta;
  DuckDBRow r4;
  r4.columns = {duckdb::Value(1), duckdb::Value(300)};
  delta.insert(r4, 1);

  view.apply_changes("t", delta);

  auto result2 = view.get_result();

  // User 1 should now accept: 300->1, 200->2, 100->3
  DuckDBRow out4;
  out4.columns = {duckdb::Value(1), duckdb::Value(300),
                  duckdb::Value((int64_t)1)};
  REQUIRE(result2.get(out4) == 1);

  // Check OLD rank 1 is GONE (retracted)
  REQUIRE(result2.get(out1) == 0);

  // Check OLD rank 1 is replaced by rank 2
  DuckDBRow out1_new;
  out1_new.columns = {duckdb::Value(1), duckdb::Value(200),
                      duckdb::Value((int64_t)2)};
  REQUIRE(result2.get(out1_new) == 1);
}

TEST_CASE("NativeWindowView Aggregates", "[window][execution]") {
  // Schema: dept (varchar), salary (int)
  TableSchema schema;
  schema.columns = {{"dept", duckdb::LogicalType::VARCHAR},
                    {"salary", duckdb::LogicalType::INTEGER}};

  // Windows:
  // SUM(salary) OVER (PARTITION BY dept ORDER BY salary) as sum_sal
  // COUNT(salary) OVER (PARTITION BY dept ORDER BY salary) as count_sal
  // AVG(salary) OVER (PARTITION BY dept ORDER BY salary) as avg_sal
  // MIN(salary) OVER (PARTITION BY dept ORDER BY salary) as min_sal
  // MAX(salary) OVER (PARTITION BY dept ORDER BY salary) as max_sal

  std::vector<NativeWindowView::WindowDef> windows;

  auto add_win = [&](std::string func, std::string alias) {
    NativeWindowView::WindowDef win;
    win.function = func;
    win.alias = alias;
    win.partition_indices = {0}; // dept
    NativeSortView::SortColumn sc;
    sc.column_idx = 1; // salary
    sc.ascending = true;
    win.sort_columns.push_back(sc);
    win.arg_column_idx = 1; // salary
    windows.push_back(win);
  };

  add_win("SUM", "sum_sal");
  add_win("COUNT", "count_sal");
  add_win("AVG", "avg_sal");
  add_win("MIN", "min_sal");
  add_win("MAX", "max_sal");

  TableSchema result_schema = schema;
  result_schema.columns.push_back({"sum_sal", duckdb::LogicalType::DOUBLE});
  result_schema.columns.push_back({"count_sal", duckdb::LogicalType::BIGINT});
  result_schema.columns.push_back({"avg_sal", duckdb::LogicalType::DOUBLE});
  result_schema.columns.push_back({"min_sal", duckdb::LogicalType::INTEGER});
  result_schema.columns.push_back({"max_sal", duckdb::LogicalType::INTEGER});

  NativeWindowView view("agg_view", "sql", "t", result_schema, schema, windows);

  DuckDBZSet input;

  DuckDBRow r1;
  r1.columns = {duckdb::Value("Sales"), duckdb::Value(1000)};
  input.insert(r1, 1);

  DuckDBRow r2;
  r2.columns = {duckdb::Value("Sales"), duckdb::Value(2000)};
  input.insert(r2, 1);

  view.apply_changes("t", input);

  auto res = view.get_result();

  // Sales 1000: sum=1000, count=1, avg=1000, min=1000, max=1000
  DuckDBRow out1;
  out1.columns = {duckdb::Value("Sales"), duckdb::Value(1000),
                  duckdb::Value(1000.0),  duckdb::Value((int64_t)1),
                  duckdb::Value(1000.0),  duckdb::Value(1000),
                  duckdb::Value(1000)};
  REQUIRE(res.get(out1) == 1);

  // Sales 2000: sum=3000, count=2, avg=1500, min=1000, max=2000
  DuckDBRow out2;
  out2.columns = {duckdb::Value("Sales"), duckdb::Value(2000),
                  duckdb::Value(3000.0),  duckdb::Value((int64_t)2),
                  duckdb::Value(1500.0),  duckdb::Value(1000),
                  duckdb::Value(2000)};
  REQUIRE(res.get(out2) == 1);
}

TEST_CASE("NativeWindowView LAG and LEAD", "[window][execution]") {
  // Schema: id (int), val (int)
  TableSchema schema;
  schema.columns = {{"id", duckdb::LogicalType::INTEGER},
                    {"val", duckdb::LogicalType::INTEGER}};

  // Windows:
  // LAG(val, 1, 0) OVER (ORDER BY id) as prev_val
  // LEAD(val, 1, 0) OVER (ORDER BY id) as next_val
  std::vector<NativeWindowView::WindowDef> windows;

  NativeWindowView::WindowDef w1;
  w1.function = "LAG";
  w1.alias = "prev_val";
  w1.arg_column_idx = 1;
  NativeSortView::SortColumn sc;
  sc.column_idx = 0;
  sc.ascending = true;
  w1.sort_columns.push_back(sc);
  // Default offset 1, default null normally but here we'll test with a default
  // value if supported Actually, DuckDB's LAG can take 1-3 args. We'll start
  // with 1 arg (offset 1, default NULL).
  windows.push_back(w1);

  NativeWindowView::WindowDef w2;
  w2.function = "LEAD";
  w2.alias = "next_val";
  w2.arg_column_idx = 1;
  w2.sort_columns.push_back(sc);
  windows.push_back(w2);

  TableSchema result_schema = schema;
  result_schema.columns.push_back({"prev_val", duckdb::LogicalType::INTEGER});
  result_schema.columns.push_back({"next_val", duckdb::LogicalType::INTEGER});

  NativeWindowView view("lag_lead_view", "sql", "t", result_schema, schema,
                        windows);

  DuckDBZSet input;
  DuckDBRow r1, r2, r3;
  r1.columns = {duckdb::Value(1), duckdb::Value(10)};
  r2.columns = {duckdb::Value(2), duckdb::Value(20)};
  r3.columns = {duckdb::Value(3), duckdb::Value(30)};
  input.insert(r1, 1);
  input.insert(r2, 1);
  input.insert(r3, 1);

  view.apply_changes("t", input);
  auto res = view.get_result();

  // Row 1: id=1, val=10 -> prev=NULL, next=20
  DuckDBRow out1;
  out1.columns = {duckdb::Value(1), duckdb::Value(10), duckdb::Value(),
                  duckdb::Value(20)};
  REQUIRE(res.get(out1) == 1);

  // Row 2: id=2, val=20 -> prev=10, next=30
  DuckDBRow out2;
  out2.columns = {duckdb::Value(2), duckdb::Value(20), duckdb::Value(10),
                  duckdb::Value(30)};
  REQUIRE(res.get(out2) == 1);

  // Row 3: id=3, val=30 -> prev=20, next=NULL
  DuckDBRow out3;
  out3.columns = {duckdb::Value(3), duckdb::Value(30), duckdb::Value(20),
                  duckdb::Value()};
  REQUIRE(res.get(out3) == 1);
}

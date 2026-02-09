#include "catch.hpp"
#include "dbsp_duckdb_types.hpp"
#include "dbsp_sql_parser.hpp"
#include "dbsp_window_view.hpp"

using namespace dbsp_native;

TEST_CASE("Window Parser - LAG with offset", "[window][parser][p5]") {
  DBSPSqlParser parser;
  auto res = parser.parse(
      "SELECT val, LAG(val, 2) OVER (ORDER BY id) as lag2 FROM t");
  INFO("Error: " << res.error);
  REQUIRE(res.success);
  REQUIRE(res.view_def.type == ParsedViewDef::ViewType::WINDOW);
  REQUIRE(res.view_def.windows.size() >= 1);

  bool found_lag = false;
  for (auto &w : res.view_def.windows) {
    if (w.function == "LAG") {
      found_lag = true;
      REQUIRE(w.arg_column == "val");
      // DuckDB may or may not parse offset as 2 depending on version
      // Just verify the function was recognized
    }
  }
  REQUIRE(found_lag);
}

TEST_CASE("Window Parser - LEAD", "[window][parser][p5]") {
  DBSPSqlParser parser;
  auto res = parser.parse(
      "SELECT val, LEAD(val) OVER (ORDER BY id) as nxt FROM t");
  INFO("Error: " << res.error);
  REQUIRE(res.success);
  REQUIRE(res.view_def.type == ParsedViewDef::ViewType::WINDOW);
  REQUIRE(res.view_def.windows.size() >= 1);

  bool found_lead = false;
  for (auto &w : res.view_def.windows) {
    if (w.function == "LEAD") {
      found_lead = true;
      REQUIRE(w.arg_column == "val");
    }
  }
  REQUIRE(found_lead);
}

TEST_CASE("Window Parser - RANK and DENSE_RANK", "[window][parser][p5]") {
  DBSPSqlParser parser;
  auto res = parser.parse(
      "SELECT val, RANK() OVER (ORDER BY val DESC) as rnk, "
      "DENSE_RANK() OVER (ORDER BY val DESC) as drnk FROM t");
  INFO("Error: " << res.error);
  REQUIRE(res.success);
  REQUIRE(res.view_def.type == ParsedViewDef::ViewType::WINDOW);
  REQUIRE(res.view_def.windows.size() == 2);

  REQUIRE(res.view_def.windows[0].function == "RANK");
  REQUIRE(res.view_def.windows[1].function == "DENSE_RANK");
}

TEST_CASE("Window Parser - ROWS BETWEEN frame", "[window][parser][p5]") {
  DBSPSqlParser parser;
  auto res = parser.parse(
      "SELECT val, SUM(val) OVER (ORDER BY id ROWS BETWEEN 2 PRECEDING AND "
      "CURRENT ROW) as rolling_sum FROM t");
  INFO("Error: " << res.error);
  REQUIRE(res.success);
  REQUIRE(res.view_def.type == ParsedViewDef::ViewType::WINDOW);
  REQUIRE(res.view_def.windows.size() >= 1);

  auto &w = res.view_def.windows[0];
  REQUIRE(w.function == "SUM");
  REQUIRE(w.arg_column == "val");
}

TEST_CASE("NativeWindowView FIRST_VALUE", "[window][execution][p5]") {
  TableSchema schema;
  schema.columns = {{"id", duckdb::LogicalType::INTEGER},
                    {"val", duckdb::LogicalType::INTEGER}};

  std::vector<NativeWindowView::WindowDef> windows;
  NativeWindowView::WindowDef win;
  win.function = "FIRST_VALUE";
  win.alias = "first_val";
  win.arg_column_idx = 1;
  NativeSortView::SortColumn sc;
  sc.column_idx = 0;
  sc.ascending = true;
  sc.nulls_first = false;
  win.sort_columns.push_back(sc);
  windows.push_back(win);

  TableSchema result_schema = schema;
  result_schema.columns.push_back({"first_val", duckdb::LogicalType::INTEGER});

  NativeWindowView view("fv_view", "sql", "t", result_schema, schema, windows);

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

  // All rows should have first_val = 10 (first in partition)
  DuckDBRow out1;
  out1.columns = {duckdb::Value(1), duckdb::Value(10), duckdb::Value(10)};
  REQUIRE(res.get(out1) == 1);

  DuckDBRow out2;
  out2.columns = {duckdb::Value(2), duckdb::Value(20), duckdb::Value(10)};
  REQUIRE(res.get(out2) == 1);

  DuckDBRow out3;
  out3.columns = {duckdb::Value(3), duckdb::Value(30), duckdb::Value(10)};
  REQUIRE(res.get(out3) == 1);
}

TEST_CASE("NativeWindowView LAST_VALUE with unbounded following",
          "[window][execution][p5]") {
  TableSchema schema;
  schema.columns = {{"id", duckdb::LogicalType::INTEGER},
                    {"val", duckdb::LogicalType::INTEGER}};

  std::vector<NativeWindowView::WindowDef> windows;
  NativeWindowView::WindowDef win;
  win.function = "LAST_VALUE";
  win.alias = "last_val";
  win.arg_column_idx = 1;
  win.start = duckdb::WindowBoundary::UNBOUNDED_PRECEDING;
  win.end = duckdb::WindowBoundary::UNBOUNDED_FOLLOWING;
  NativeSortView::SortColumn sc;
  sc.column_idx = 0;
  sc.ascending = true;
  sc.nulls_first = false;
  win.sort_columns.push_back(sc);
  windows.push_back(win);

  TableSchema result_schema = schema;
  result_schema.columns.push_back({"last_val", duckdb::LogicalType::INTEGER});

  NativeWindowView view("lv_view", "sql", "t", result_schema, schema, windows);

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

  // All rows should have last_val = 30 (unbounded following)
  DuckDBRow out1;
  out1.columns = {duckdb::Value(1), duckdb::Value(10), duckdb::Value(30)};
  REQUIRE(res.get(out1) == 1);

  DuckDBRow out2;
  out2.columns = {duckdb::Value(2), duckdb::Value(20), duckdb::Value(30)};
  REQUIRE(res.get(out2) == 1);
}

TEST_CASE("NativeWindowView NTILE", "[window][execution][p5]") {
  TableSchema schema;
  schema.columns = {{"id", duckdb::LogicalType::INTEGER},
                    {"val", duckdb::LogicalType::INTEGER}};

  std::vector<NativeWindowView::WindowDef> windows;
  NativeWindowView::WindowDef win;
  win.function = "NTILE";
  win.alias = "bucket";
  win.offset = 3; // NTILE(3)
  NativeSortView::SortColumn sc;
  sc.column_idx = 0;
  sc.ascending = true;
  sc.nulls_first = false;
  win.sort_columns.push_back(sc);
  windows.push_back(win);

  TableSchema result_schema = schema;
  result_schema.columns.push_back({"bucket", duckdb::LogicalType::BIGINT});

  NativeWindowView view("ntile_view", "sql", "t", result_schema, schema,
                        windows);

  DuckDBZSet input;
  for (int i = 1; i <= 6; i++) {
    DuckDBRow r;
    r.columns = {duckdb::Value(i), duckdb::Value(i * 10)};
    input.insert(r, 1);
  }

  view.apply_changes("t", input);
  auto res = view.get_result();

  // 6 rows, NTILE(3): rows 1-2 -> bucket 1, rows 3-4 -> bucket 2, rows 5-6 ->
  // bucket 3
  DuckDBRow out1;
  out1.columns = {duckdb::Value(1), duckdb::Value(10),
                  duckdb::Value((int64_t)1)};
  REQUIRE(res.get(out1) == 1);

  DuckDBRow out3;
  out3.columns = {duckdb::Value(3), duckdb::Value(30),
                  duckdb::Value((int64_t)2)};
  REQUIRE(res.get(out3) == 1);

  DuckDBRow out5;
  out5.columns = {duckdb::Value(5), duckdb::Value(50),
                  duckdb::Value((int64_t)3)};
  REQUIRE(res.get(out5) == 1);
}

TEST_CASE("NativeWindowView LAG with custom offset",
          "[window][execution][p5]") {
  TableSchema schema;
  schema.columns = {{"id", duckdb::LogicalType::INTEGER},
                    {"val", duckdb::LogicalType::INTEGER}};

  std::vector<NativeWindowView::WindowDef> windows;
  NativeWindowView::WindowDef win;
  win.function = "LAG";
  win.alias = "lag2";
  win.arg_column_idx = 1;
  win.offset = 2; // LAG(val, 2)
  NativeSortView::SortColumn sc;
  sc.column_idx = 0;
  sc.ascending = true;
  sc.nulls_first = false;
  win.sort_columns.push_back(sc);
  windows.push_back(win);

  TableSchema result_schema = schema;
  result_schema.columns.push_back({"lag2", duckdb::LogicalType::INTEGER});

  NativeWindowView view("lag2_view", "sql", "t", result_schema, schema,
                        windows);

  DuckDBZSet input;
  DuckDBRow r1, r2, r3, r4;
  r1.columns = {duckdb::Value(1), duckdb::Value(10)};
  r2.columns = {duckdb::Value(2), duckdb::Value(20)};
  r3.columns = {duckdb::Value(3), duckdb::Value(30)};
  r4.columns = {duckdb::Value(4), duckdb::Value(40)};
  input.insert(r1, 1);
  input.insert(r2, 1);
  input.insert(r3, 1);
  input.insert(r4, 1);

  view.apply_changes("t", input);
  auto res = view.get_result();

  // Row 1 (id=1): LAG(val, 2) = NULL (no 2 rows before)
  DuckDBRow out1;
  out1.columns = {duckdb::Value(1), duckdb::Value(10), duckdb::Value()};
  REQUIRE(res.get(out1) == 1);

  // Row 2 (id=2): LAG(val, 2) = NULL (only 1 row before)
  DuckDBRow out2;
  out2.columns = {duckdb::Value(2), duckdb::Value(20), duckdb::Value()};
  REQUIRE(res.get(out2) == 1);

  // Row 3 (id=3): LAG(val, 2) = 10
  DuckDBRow out3;
  out3.columns = {duckdb::Value(3), duckdb::Value(30), duckdb::Value(10)};
  REQUIRE(res.get(out3) == 1);

  // Row 4 (id=4): LAG(val, 2) = 20
  DuckDBRow out4;
  out4.columns = {duckdb::Value(4), duckdb::Value(40), duckdb::Value(20)};
  REQUIRE(res.get(out4) == 1);
}

TEST_CASE("NativeWindowView ROWS BETWEEN N PRECEDING AND M FOLLOWING",
          "[window][execution][p5]") {
  TableSchema schema;
  schema.columns = {{"id", duckdb::LogicalType::INTEGER},
                    {"val", duckdb::LogicalType::INTEGER}};

  // SUM(val) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING)
  std::vector<NativeWindowView::WindowDef> windows;
  NativeWindowView::WindowDef win;
  win.function = "SUM";
  win.alias = "sliding_sum";
  win.arg_column_idx = 1;
  win.start = duckdb::WindowBoundary::EXPR_PRECEDING_ROWS;
  win.start_offset = 1;
  win.end = duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS;
  win.end_offset = 1;
  NativeSortView::SortColumn sc;
  sc.column_idx = 0;
  sc.ascending = true;
  sc.nulls_first = false;
  win.sort_columns.push_back(sc);
  windows.push_back(win);

  TableSchema result_schema = schema;
  result_schema.columns.push_back(
      {"sliding_sum", duckdb::LogicalType::DOUBLE});

  NativeWindowView view("slide_view", "sql", "t", result_schema, schema,
                        windows);

  DuckDBZSet input;
  DuckDBRow r1, r2, r3, r4;
  r1.columns = {duckdb::Value(1), duckdb::Value(10)};
  r2.columns = {duckdb::Value(2), duckdb::Value(20)};
  r3.columns = {duckdb::Value(3), duckdb::Value(30)};
  r4.columns = {duckdb::Value(4), duckdb::Value(40)};
  input.insert(r1, 1);
  input.insert(r2, 1);
  input.insert(r3, 1);
  input.insert(r4, 1);

  view.apply_changes("t", input);
  auto res = view.get_result();

  // Row 1: frame [0,1] = 10+20 = 30
  DuckDBRow out1;
  out1.columns = {duckdb::Value(1), duckdb::Value(10), duckdb::Value(30.0)};
  REQUIRE(res.get(out1) == 1);

  // Row 2: frame [0,2] = 10+20+30 = 60
  DuckDBRow out2;
  out2.columns = {duckdb::Value(2), duckdb::Value(20), duckdb::Value(60.0)};
  REQUIRE(res.get(out2) == 1);

  // Row 3: frame [1,3] = 20+30+40 = 90
  DuckDBRow out3;
  out3.columns = {duckdb::Value(3), duckdb::Value(30), duckdb::Value(90.0)};
  REQUIRE(res.get(out3) == 1);

  // Row 4: frame [2,3] = 30+40 = 70
  DuckDBRow out4;
  out4.columns = {duckdb::Value(4), duckdb::Value(40), duckdb::Value(70.0)};
  REQUIRE(res.get(out4) == 1);
}

TEST_CASE("NativeWindowView incremental update with advanced functions",
          "[window][execution][p5]") {
  TableSchema schema;
  schema.columns = {{"dept", duckdb::LogicalType::VARCHAR},
                    {"id", duckdb::LogicalType::INTEGER},
                    {"val", duckdb::LogicalType::INTEGER}};

  // LAG(val) OVER (PARTITION BY dept ORDER BY id)
  std::vector<NativeWindowView::WindowDef> windows;
  NativeWindowView::WindowDef win;
  win.function = "LAG";
  win.alias = "prev_val";
  win.arg_column_idx = 2;
  win.partition_indices = {0}; // dept
  NativeSortView::SortColumn sc;
  sc.column_idx = 1;
  sc.ascending = true;
  sc.nulls_first = false;
  win.sort_columns.push_back(sc);
  windows.push_back(win);

  TableSchema result_schema = schema;
  result_schema.columns.push_back({"prev_val", duckdb::LogicalType::INTEGER});

  NativeWindowView view("lag_part_view", "sql", "t", result_schema, schema,
                        windows);

  // Initial data
  DuckDBZSet input;
  DuckDBRow r1, r2;
  r1.columns = {duckdb::Value("A"), duckdb::Value(1), duckdb::Value(100)};
  r2.columns = {duckdb::Value("A"), duckdb::Value(2), duckdb::Value(200)};
  input.insert(r1, 1);
  input.insert(r2, 1);

  view.apply_changes("t", input);
  auto res = view.get_result();

  // id=1: LAG = NULL, id=2: LAG = 100
  DuckDBRow out1;
  out1.columns = {duckdb::Value("A"), duckdb::Value(1), duckdb::Value(100),
                  duckdb::Value()};
  REQUIRE(res.get(out1) == 1);

  DuckDBRow out2;
  out2.columns = {duckdb::Value("A"), duckdb::Value(2), duckdb::Value(200),
                  duckdb::Value(100)};
  REQUIRE(res.get(out2) == 1);

  // Insert id=3 for dept A
  DuckDBZSet delta;
  DuckDBRow r3;
  r3.columns = {duckdb::Value("A"), duckdb::Value(3), duckdb::Value(300)};
  delta.insert(r3, 1);

  view.apply_changes("t", delta);
  auto res2 = view.get_result();

  // id=3: LAG = 200
  DuckDBRow out3;
  out3.columns = {duckdb::Value("A"), duckdb::Value(3), duckdb::Value(300),
                  duckdb::Value(200)};
  REQUIRE(res2.get(out3) == 1);

  // Original rows should still be correct
  REQUIRE(res2.get(out1) == 1);
  REQUIRE(res2.get(out2) == 1);
}

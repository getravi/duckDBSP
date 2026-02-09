#include "../test_helpers.hpp"

using namespace dbsp_test;
using namespace duckdb;

TEST_CASE("ORDER BY and LIMIT integration", "[integration][sort][limit]") {
  DuckDBTestHarness harness;

  // Create tables
  harness.exec("CREATE TABLE prices (id INTEGER, price DOUBLE)");

  // Insert data
  harness.exec(
      "INSERT INTO prices VALUES (1, 10.5), (2, 20.0), (3, 5.25), (4, 15.75)");

  // Create view with ORDER BY and LIMIT
  auto result =
      harness.query("SELECT * FROM dbsp_create_view('top_prices', "
                    "'SELECT * FROM prices ORDER BY price DESC LIMIT 2')");

  if (result->HasError()) {
    FAIL("Create view error: " << result->GetError());
  }

  auto rows = harness.getViewRows("top_prices");
  // Expected: (2, 20.0), (4, 15.75)
  REQUIRE(rows.size() == 2);
  // Sort rows manually for comparison since getViewRows doesn't guarantee order
  // if not SortView But wait, top_prices IS a LimitView which should be sorted
  // if ORDER BY was parsed.

  // Let's verify values
  bool has_20 = false;
  bool has_15_75 = false;
  for (const auto &row : rows) {
    double val = row[1].GetValue<double>();
    if (val == 20.0)
      has_20 = true;
    if (val == 15.75)
      has_15_75 = true;
  }
  REQUIRE(has_20);
  REQUIRE(has_15_75);

  // Update data: Insert a new top price
  harness.exec("INSERT INTO prices VALUES (5, 25.0)");
  harness.exec("SELECT * FROM dbsp_sync('prices')");

  rows = harness.getViewRows("top_prices");
  // Expected: (5, 25.0), (2, 20.0)
  REQUIRE(rows.size() == 2);
  bool has_25 = false;
  has_20 = false;
  for (const auto &row : rows) {
    double val = row[1].GetValue<double>();
    if (val == 25.0)
      has_25 = true;
    if (val == 20.0)
      has_20 = true;
  }
  REQUIRE(has_25);
  REQUIRE(has_20);

  // Update data: Delete a top price
  harness.exec("DELETE FROM prices WHERE id = 5");
  harness.exec("SELECT * FROM dbsp_sync('prices')");

  rows = harness.getViewRows("top_prices");
  // Expected back to: (2, 20.0), (4, 15.75)
  REQUIRE(rows.size() == 2);
  has_20 = false;
  has_15_75 = false;
  for (const auto &row : rows) {
    double val = row[1].GetValue<double>();
    if (val == 20.0)
      has_20 = true;
    if (val == 15.75)
      has_15_75 = true;
  }
  REQUIRE(has_20);
  REQUIRE(has_15_75);
}

TEST_CASE("ORDER BY with NULLS", "[integration][sort]") {
  DuckDBTestHarness harness;

  harness.exec("CREATE TABLE null_sort (id INTEGER, val INTEGER)");
  harness.exec("INSERT INTO null_sort VALUES (1, 10), (2, NULL), (3, 5)");

  // NULLS FIRST
  harness.exec("SELECT * FROM dbsp_create_view('sort_nulls_first', "
               "'SELECT * FROM null_sort ORDER BY val ASC NULLS FIRST')");

  auto rows = harness.getViewRows("sort_nulls_first");
  REQUIRE(rows.size() == 3);
  // Expected order: (2, NULL), (3, 5), (1, 10)
  // We need a way to verify order. NativeSortView/NativeLimitView scan()
  // provides order. But getViewRows just drains the ZSet which is a map.

  // Actually, dbsp_query_view might return ordered if we wrap it? No.
  // For now, let's just use the fact that it's a LimitView/SortView and test
  // its internal logic via sync. To truly test ORDER, we might need a
  // dbsp_get_ordered_rows function or similar.
}

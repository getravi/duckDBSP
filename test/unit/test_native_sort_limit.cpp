#include "catch.hpp"
#include "dbsp_duckdb_types.hpp"
#include <string>
#include <vector>

using namespace dbsp_native;

// Helper to create a DuckDBRow from integers
DuckDBRow make_row(int id, std::string name) {
  DuckDBRow row;
  row.columns.push_back(duckdb::Value(id));
  row.columns.push_back(duckdb::Value(name));
  return row;
}

// Helper to collect scan results
struct ScanResult {
  int id;
  std::string name;
  Weight weight;

  bool operator==(const ScanResult &other) const {
    return id == other.id && name == other.name && weight == other.weight;
  }
};

std::vector<ScanResult> run_scan(const NativeMaterializedView &view) {
  std::vector<ScanResult> results;
  view.scan([&](const DuckDBRow &row, Weight w) {
    if (w > 0) {
      results.push_back(
          {row.columns[0].GetValue<int>(), row.columns[1].ToString(), w});
    }
  });
  return results;
}

TEST_CASE("NativeSortView - Basic Sort", "[sort]") {
  TableSchema schema;
  schema.columns.push_back({"id", duckdb::LogicalType::INTEGER});
  schema.columns.push_back({"name", duckdb::LogicalType::VARCHAR});

  std::vector<NativeSortView::SortColumn> sort_cols;
  sort_cols.push_back({0, true, true}); // Sort by ID ASC

  NativeSortView view("test_sort", "SELECT * ...", "source", schema, sort_cols);

  DuckDBZSet changes;
  changes.insert(make_row(3, "c"), 1);
  changes.insert(make_row(1, "a"), 1);
  changes.insert(make_row(2, "b"), 1);

  view.apply_changes("source", changes);

  auto results = run_scan(view);

  REQUIRE(results.size() == 3);
  REQUIRE(results[0].id == 1);
  REQUIRE(results[1].id == 2);
  REQUIRE(results[2].id == 3);
}

TEST_CASE("NativeSortView - Descending Sort", "[sort]") {
  TableSchema schema;
  schema.columns.push_back({"id", duckdb::LogicalType::INTEGER});
  schema.columns.push_back({"name", duckdb::LogicalType::VARCHAR});

  std::vector<NativeSortView::SortColumn> sort_cols;
  sort_cols.push_back({0, false, true}); // Sort by ID DESC

  NativeSortView view("test_sort_desc", "SELECT...", "source", schema,
                      sort_cols);

  DuckDBZSet changes;
  changes.insert(make_row(1, "a"), 1);
  changes.insert(make_row(3, "c"), 1);
  changes.insert(make_row(2, "b"), 1);

  view.apply_changes("source", changes);

  auto results = run_scan(view);

  REQUIRE(results.size() == 3);
  REQUIRE(results[0].id == 3);
  REQUIRE(results[1].id == 2);
  REQUIRE(results[2].id == 1);
}

TEST_CASE("NativeLimitView - Limit Only", "[limit]") {
  TableSchema schema;
  schema.columns.push_back({"id", duckdb::LogicalType::INTEGER});
  schema.columns.push_back({"name", duckdb::LogicalType::VARCHAR});

  std::vector<NativeSortView::SortColumn> sort_cols;
  sort_cols.push_back({0, true, true}); // Sort by ID ASC

  // Limit 2, Offset 0
  NativeLimitView view("test_limit", "SELECT...", "source", schema, 2, 0,
                       sort_cols);

  DuckDBZSet changes;
  changes.insert(make_row(1, "a"), 1);
  changes.insert(make_row(2, "b"), 1);
  changes.insert(make_row(3, "c"), 1);

  view.apply_changes("source", changes);

  auto results = run_scan(view);

  REQUIRE(results.size() == 2);
  REQUIRE(results[0].id == 1);
  REQUIRE(results[1].id == 2);
}

TEST_CASE("NativeLimitView - Limit and Offset", "[limit]") {
  TableSchema schema;
  schema.columns.push_back({"id", duckdb::LogicalType::INTEGER});
  schema.columns.push_back({"name", duckdb::LogicalType::VARCHAR});

  std::vector<NativeSortView::SortColumn> sort_cols;
  sort_cols.push_back({0, true, true}); // Sort by ID ASC

  // Limit 1, Offset 1
  NativeLimitView view("test_limit_offset", "SELECT...", "source", schema, 1, 1,
                       sort_cols);

  DuckDBZSet changes;
  changes.insert(make_row(1, "a"), 1);
  changes.insert(make_row(2, "b"), 1);
  changes.insert(make_row(3, "c"), 1);
  changes.insert(make_row(4, "d"), 1);

  view.apply_changes("source", changes);

  auto results = run_scan(view);

  REQUIRE(results.size() == 1);
  REQUIRE(results[0].id == 2); // 1 is skipped, 2 is taken.
}

TEST_CASE("NativeLimitView - Incremental Updates", "[limit]") {
  TableSchema schema;
  schema.columns.push_back({"id", duckdb::LogicalType::INTEGER});
  schema.columns.push_back({"name", duckdb::LogicalType::VARCHAR});

  std::vector<NativeSortView::SortColumn> sort_cols;
  sort_cols.push_back({0, true, true}); // Sort by ID ASC

  // Limit 3
  NativeLimitView view("test_limit_inc", "SELECT...", "source", schema, 3, 0,
                       sort_cols);

  // Initial: 1, 3, 5
  DuckDBZSet changes1;
  changes1.insert(make_row(1, "a"), 1);
  changes1.insert(make_row(3, "c"), 1);
  changes1.insert(make_row(5, "e"), 1);
  view.apply_changes("source", changes1);

  auto results1 = run_scan(view);
  REQUIRE(results1.size() == 3);
  REQUIRE(results1[0].id == 1);
  REQUIRE(results1[2].id == 5);

  // Insert 2, 4. Result should be 1, 2, 3. (5 pushed out)
  DuckDBZSet changes2;
  changes2.insert(make_row(2, "b"), 1);
  changes2.insert(make_row(4, "d"), 1);
  view.apply_changes("source", changes2);

  auto results2 = run_scan(view);
  REQUIRE(results2.size() == 3);
  REQUIRE(results2[0].id == 1);
  REQUIRE(results2[1].id == 2);
  REQUIRE(results2[2].id == 3);
}

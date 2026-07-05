#include "../test_helpers.hpp"
#include "dbsp_distinct_on.hpp"

using namespace dbsp_native;

static DuckDBRow make_distinct_row(int id, std::string cat, int score) {
  DuckDBRow row;
  row.columns.push_back(duckdb::Value(id));
  row.columns.push_back(duckdb::Value(cat));
  row.columns.push_back(duckdb::Value(score));
  return row;
}

TEST_CASE("NativeDistinctOnView basic semantics", "[distinct_on]") {
  TableSchema schema;
  schema.table_name = "test_table";
  schema.columns.push_back({"id", duckdb::LogicalType::INTEGER});
  schema.columns.push_back({"category", duckdb::LogicalType::VARCHAR});
  schema.columns.push_back({"score", duckdb::LogicalType::INTEGER});

  SECTION("Basic DISTINCT ON with ordering") {
    // SELECT DISTINCT ON (category) * FROM test_table ORDER BY category, score
    // DESC
    std::vector<size_t> partition_keys = {1}; // category
    std::vector<NativeDistinctOnView::SortColumn> sort_cols = {
        {1, true, false}, // category ASC
        {2, false, false} // score DESC
    };

    NativeDistinctOnView view("v1", "SQL", "test_table", schema, partition_keys,
                              sort_cols);

    DuckDBZSet changes;
    changes.insert(make_distinct_row(1, "A", 10), 1);
    changes.insert(make_distinct_row(2, "A", 20), 1);
    changes.insert(make_distinct_row(3, "B", 30), 1);

    view.apply_changes("test_table", changes);

    const auto &result = view.get_result();
    REQUIRE(result.size() == 2);
    REQUIRE((result.get(make_distinct_row(2, "A", 20)) == 1LL));
    REQUIRE((result.get(make_distinct_row(3, "B", 30)) == 1LL));

    // Incremental update: Add a better score for A
    DuckDBZSet changes2;
    changes2.insert(make_distinct_row(4, "A", 40), 1);
    view.apply_changes("test_table", changes2);

    const auto &delta = view.get_delta();
    REQUIRE((delta.get(make_distinct_row(2, "A", 20)) == -1LL));
    REQUIRE((delta.get(make_distinct_row(4, "A", 40)) == 1LL));

    REQUIRE((view.get_result().get(make_distinct_row(4, "A", 40)) == 1LL));
    REQUIRE((view.get_result().get(make_distinct_row(2, "A", 20)) == 0LL));
  }

  SECTION("Delete first row") {
    std::vector<size_t> partition_keys = {1};
    std::vector<NativeDistinctOnView::SortColumn> sort_cols = {
        {2, false, false}};

    NativeDistinctOnView view("v1", "SQL", "test_table", schema, partition_keys,
                              sort_cols);

    DuckDBZSet changes;
    changes.insert(make_distinct_row(1, "A", 100), 1);
    changes.insert(make_distinct_row(2, "A", 50), 1);
    view.apply_changes("test_table", changes);

    REQUIRE((view.get_result().get(make_distinct_row(1, "A", 100)) == 1LL));

    DuckDBZSet changes2;
    changes2.insert(make_distinct_row(1, "A", 100), -1);
    view.apply_changes("test_table", changes2);

    REQUIRE((view.get_delta().get(make_distinct_row(1, "A", 100)) == -1LL));
    REQUIRE((view.get_delta().get(make_distinct_row(2, "A", 50)) == 1LL));
  }
}

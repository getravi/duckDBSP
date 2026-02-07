// P2.2 TDD Tests: MIN/MAX Incremental Deletion Correctness
// Verifies O(log n) multiset-based MIN/MAX maintains correctness
// under all mutation patterns (insert, delete, mixed)

#include "../../dbsp_duckdb_types.hpp"
#include "catch.hpp"

using namespace dbsp_native;
using namespace duckdb;

// Helper: Create a NativeAggregateView for MIN or MAX
static std::unique_ptr<NativeAggregateView> make_minmax_view(
    const std::string &name, NativeAggregateView::AggType agg_type) {

  TableSchema schema;
  schema.table_name = name;
  schema.columns = {{"group_key", LogicalType::VARCHAR},
                    {"agg_result", LogicalType::BIGINT}};

  // Key function: extract column 0 (group key)
  auto key_fn = [](const DuckDBRow &row) -> DuckDBRow {
    DuckDBRow key;
    key.columns.push_back(row.columns[0]);
    return key;
  };

  // Value function: extract column 1 (value to aggregate)
  auto value_fn = [](const DuckDBRow &row) -> Value {
    return row.columns[1];
  };

  return std::make_unique<NativeAggregateView>(
      name, "SELECT group_key, " + std::string(agg_type == NativeAggregateView::AggType::MIN ? "MIN" : "MAX") + "(val) FROM t GROUP BY group_key",
      "source_table", schema, key_fn, value_fn, agg_type);
}

// Helper: Create a row with (group_key, value)
static DuckDBRow make_row(const std::string &key, int64_t val) {
  DuckDBRow row;
  row.columns.push_back(Value(key));
  row.columns.push_back(Value::BIGINT(val));
  return row;
}

// Helper: Create a row with (group_key, NULL)
static DuckDBRow make_null_row(const std::string &key) {
  DuckDBRow row;
  row.columns.push_back(Value(key));
  row.columns.push_back(Value(LogicalType::BIGINT)); // NULL
  return row;
}

// Helper: Get the aggregate value for a group from result ZSet
static int64_t get_agg_value(const DuckDBZSet &result, const std::string &key) {
  for (const auto &[row, weight] : result) {
    if (weight > 0 && row.columns[0].GetValue<std::string>() == key) {
      return row.columns[1].GetValue<int64_t>();
    }
  }
  throw std::runtime_error("Group not found: " + key);
}

// Helper: Check if a group exists in result
static bool has_group(const DuckDBZSet &result, const std::string &key) {
  for (const auto &[row, weight] : result) {
    if (weight > 0 && row.columns[0].GetValue<std::string>() == key) {
      return true;
    }
  }
  return false;
}

// ============================================================================
// MIN Aggregate Tests
// ============================================================================

TEST_CASE("MIN: basic computation with initial data", "[unit][minmax]") {
  auto view = make_minmax_view("min_test", NativeAggregateView::AggType::MIN);

  DuckDBZSet initial;
  initial.insert(make_row("A", 10), 1);
  initial.insert(make_row("A", 5), 1);
  initial.insert(make_row("A", 20), 1);
  initial.insert(make_row("B", 100), 1);
  initial.insert(make_row("B", 50), 1);

  view->apply_changes("source_table", initial);

  const auto &result = view->get_result();
  REQUIRE(get_agg_value(result, "A") == 5);
  REQUIRE(get_agg_value(result, "B") == 50);
}

TEST_CASE("MIN: deletion of minimum value updates correctly", "[unit][minmax]") {
  auto view = make_minmax_view("min_del", NativeAggregateView::AggType::MIN);

  // Insert initial data: A has {5, 10, 15, 20}
  DuckDBZSet initial;
  initial.insert(make_row("A", 5), 1);
  initial.insert(make_row("A", 10), 1);
  initial.insert(make_row("A", 15), 1);
  initial.insert(make_row("A", 20), 1);
  view->apply_changes("source_table", initial);

  REQUIRE(get_agg_value(view->get_result(), "A") == 5);

  // Delete minimum value (5)
  DuckDBZSet del;
  del.insert(make_row("A", 5), -1);
  view->apply_changes("source_table", del);

  // MIN should now be 10
  REQUIRE(get_agg_value(view->get_result(), "A") == 10);
}

TEST_CASE("MIN: deletion of non-minimum value", "[unit][minmax]") {
  auto view = make_minmax_view("min_nondel", NativeAggregateView::AggType::MIN);

  DuckDBZSet initial;
  initial.insert(make_row("A", 5), 1);
  initial.insert(make_row("A", 10), 1);
  initial.insert(make_row("A", 20), 1);
  view->apply_changes("source_table", initial);

  REQUIRE(get_agg_value(view->get_result(), "A") == 5);

  // Delete non-minimum (20)
  DuckDBZSet del;
  del.insert(make_row("A", 20), -1);
  view->apply_changes("source_table", del);

  // MIN should still be 5
  REQUIRE(get_agg_value(view->get_result(), "A") == 5);
}

TEST_CASE("MIN: duplicate minimum values handled correctly", "[unit][minmax]") {
  auto view = make_minmax_view("min_dup", NativeAggregateView::AggType::MIN);

  // Insert: {5, 5, 5, 10}
  DuckDBZSet initial;
  initial.insert(make_row("A", 5), 3); // weight 3 = three copies of 5
  initial.insert(make_row("A", 10), 1);
  view->apply_changes("source_table", initial);

  REQUIRE(get_agg_value(view->get_result(), "A") == 5);

  // Delete one copy of 5 (two remain)
  DuckDBZSet del1;
  del1.insert(make_row("A", 5), -1);
  view->apply_changes("source_table", del1);

  // MIN should still be 5 (two copies remain)
  REQUIRE(get_agg_value(view->get_result(), "A") == 5);

  // Delete two more copies of 5 (none remain)
  DuckDBZSet del2;
  del2.insert(make_row("A", 5), -2);
  view->apply_changes("source_table", del2);

  // MIN should now be 10
  REQUIRE(get_agg_value(view->get_result(), "A") == 10);
}

TEST_CASE("MIN: sequential deletions moving minimum up", "[unit][minmax]") {
  auto view = make_minmax_view("min_seq", NativeAggregateView::AggType::MIN);

  // Insert: {1, 2, 3, 4, 5}
  DuckDBZSet initial;
  for (int64_t i = 1; i <= 5; i++) {
    initial.insert(make_row("A", i), 1);
  }
  view->apply_changes("source_table", initial);
  REQUIRE(get_agg_value(view->get_result(), "A") == 1);

  // Delete 1 → MIN = 2
  DuckDBZSet d1;
  d1.insert(make_row("A", 1), -1);
  view->apply_changes("source_table", d1);
  REQUIRE(get_agg_value(view->get_result(), "A") == 2);

  // Delete 2 → MIN = 3
  DuckDBZSet d2;
  d2.insert(make_row("A", 2), -1);
  view->apply_changes("source_table", d2);
  REQUIRE(get_agg_value(view->get_result(), "A") == 3);

  // Delete 3 → MIN = 4
  DuckDBZSet d3;
  d3.insert(make_row("A", 3), -1);
  view->apply_changes("source_table", d3);
  REQUIRE(get_agg_value(view->get_result(), "A") == 4);

  // Delete 4 → MIN = 5
  DuckDBZSet d4;
  d4.insert(make_row("A", 4), -1);
  view->apply_changes("source_table", d4);
  REQUIRE(get_agg_value(view->get_result(), "A") == 5);
}

TEST_CASE("MIN: empty group after all deletions", "[unit][minmax]") {
  auto view = make_minmax_view("min_empty", NativeAggregateView::AggType::MIN);

  DuckDBZSet initial;
  initial.insert(make_row("A", 10), 1);
  initial.insert(make_row("A", 20), 1);
  view->apply_changes("source_table", initial);
  REQUIRE(has_group(view->get_result(), "A"));

  // Delete all values
  DuckDBZSet del;
  del.insert(make_row("A", 10), -1);
  del.insert(make_row("A", 20), -1);
  view->apply_changes("source_table", del);

  // Group should be removed
  REQUIRE_FALSE(has_group(view->get_result(), "A"));
}

// ============================================================================
// MAX Aggregate Tests
// ============================================================================

TEST_CASE("MAX: basic computation with initial data", "[unit][minmax]") {
  auto view = make_minmax_view("max_test", NativeAggregateView::AggType::MAX);

  DuckDBZSet initial;
  initial.insert(make_row("A", 10), 1);
  initial.insert(make_row("A", 5), 1);
  initial.insert(make_row("A", 20), 1);

  view->apply_changes("source_table", initial);

  REQUIRE(get_agg_value(view->get_result(), "A") == 20);
}

TEST_CASE("MAX: deletion of maximum value updates correctly", "[unit][minmax]") {
  auto view = make_minmax_view("max_del", NativeAggregateView::AggType::MAX);

  DuckDBZSet initial;
  initial.insert(make_row("A", 5), 1);
  initial.insert(make_row("A", 10), 1);
  initial.insert(make_row("A", 15), 1);
  initial.insert(make_row("A", 20), 1);
  view->apply_changes("source_table", initial);

  REQUIRE(get_agg_value(view->get_result(), "A") == 20);

  // Delete maximum (20)
  DuckDBZSet del;
  del.insert(make_row("A", 20), -1);
  view->apply_changes("source_table", del);

  // MAX should now be 15
  REQUIRE(get_agg_value(view->get_result(), "A") == 15);
}

TEST_CASE("MAX: duplicate maximum values handled correctly", "[unit][minmax]") {
  auto view = make_minmax_view("max_dup", NativeAggregateView::AggType::MAX);

  // {10, 20, 20, 20}
  DuckDBZSet initial;
  initial.insert(make_row("A", 10), 1);
  initial.insert(make_row("A", 20), 3);
  view->apply_changes("source_table", initial);

  REQUIRE(get_agg_value(view->get_result(), "A") == 20);

  // Delete one 20 (two remain)
  DuckDBZSet del1;
  del1.insert(make_row("A", 20), -1);
  view->apply_changes("source_table", del1);

  // Still 20
  REQUIRE(get_agg_value(view->get_result(), "A") == 20);

  // Delete remaining 20s
  DuckDBZSet del2;
  del2.insert(make_row("A", 20), -2);
  view->apply_changes("source_table", del2);

  // Now 10
  REQUIRE(get_agg_value(view->get_result(), "A") == 10);
}

// ============================================================================
// Multiple Groups Tests
// ============================================================================

TEST_CASE("MIN/MAX: multiple groups maintained independently", "[unit][minmax]") {
  auto min_view = make_minmax_view("multi_min", NativeAggregateView::AggType::MIN);

  DuckDBZSet initial;
  initial.insert(make_row("A", 10), 1);
  initial.insert(make_row("A", 20), 1);
  initial.insert(make_row("B", 100), 1);
  initial.insert(make_row("B", 200), 1);
  initial.insert(make_row("C", 50), 1);
  min_view->apply_changes("source_table", initial);

  REQUIRE(get_agg_value(min_view->get_result(), "A") == 10);
  REQUIRE(get_agg_value(min_view->get_result(), "B") == 100);
  REQUIRE(get_agg_value(min_view->get_result(), "C") == 50);

  // Delete min of B only
  DuckDBZSet del;
  del.insert(make_row("B", 100), -1);
  min_view->apply_changes("source_table", del);

  // A and C unchanged, B updated
  REQUIRE(get_agg_value(min_view->get_result(), "A") == 10);
  REQUIRE(get_agg_value(min_view->get_result(), "B") == 200);
  REQUIRE(get_agg_value(min_view->get_result(), "C") == 50);
}

// ============================================================================
// Mixed Insert + Delete Tests
// ============================================================================

TEST_CASE("MIN: insert new minimum after deletions", "[unit][minmax]") {
  auto view = make_minmax_view("min_ins", NativeAggregateView::AggType::MIN);

  DuckDBZSet initial;
  initial.insert(make_row("A", 10), 1);
  initial.insert(make_row("A", 20), 1);
  view->apply_changes("source_table", initial);
  REQUIRE(get_agg_value(view->get_result(), "A") == 10);

  // Delete 10, then insert 5 in same batch
  DuckDBZSet mixed;
  mixed.insert(make_row("A", 10), -1);
  mixed.insert(make_row("A", 5), 1);
  view->apply_changes("source_table", mixed);

  // New MIN should be 5
  REQUIRE(get_agg_value(view->get_result(), "A") == 5);
}

TEST_CASE("MAX: insert new maximum after deletions", "[unit][minmax]") {
  auto view = make_minmax_view("max_ins", NativeAggregateView::AggType::MAX);

  DuckDBZSet initial;
  initial.insert(make_row("A", 10), 1);
  initial.insert(make_row("A", 20), 1);
  view->apply_changes("source_table", initial);
  REQUIRE(get_agg_value(view->get_result(), "A") == 20);

  // Delete 20, insert 30
  DuckDBZSet mixed;
  mixed.insert(make_row("A", 20), -1);
  mixed.insert(make_row("A", 30), 1);
  view->apply_changes("source_table", mixed);

  REQUIRE(get_agg_value(view->get_result(), "A") == 30);
}

// ============================================================================
// NULL Handling Tests
// ============================================================================

TEST_CASE("MIN: NULL values are ignored", "[unit][minmax]") {
  auto view = make_minmax_view("min_null", NativeAggregateView::AggType::MIN);

  DuckDBZSet initial;
  initial.insert(make_null_row("A"), 1);
  initial.insert(make_row("A", 10), 1);
  initial.insert(make_null_row("A"), 1);
  initial.insert(make_row("A", 20), 1);
  view->apply_changes("source_table", initial);

  // MIN should be 10, ignoring NULLs
  REQUIRE(get_agg_value(view->get_result(), "A") == 10);
}

TEST_CASE("MIN: all NULLs still creates group with NULL result", "[unit][minmax]") {
  auto view = make_minmax_view("min_allnull", NativeAggregateView::AggType::MIN);

  DuckDBZSet initial;
  initial.insert(make_null_row("A"), 1);
  initial.insert(make_null_row("A"), 1);
  view->apply_changes("source_table", initial);

  // Group exists but value should be NULL
  REQUIRE(has_group(view->get_result(), "A"));
  // The aggregate value should be NULL (multiset empty, count=0, null_count=2)
  for (const auto &[row, weight] : view->get_result()) {
    if (weight > 0 && row.columns[0].GetValue<std::string>() == "A") {
      REQUIRE(row.columns[1].IsNull());
    }
  }
}

// ============================================================================
// Negative Value Tests
// ============================================================================

TEST_CASE("MIN: works with negative values", "[unit][minmax]") {
  auto view = make_minmax_view("min_neg", NativeAggregateView::AggType::MIN);

  DuckDBZSet initial;
  initial.insert(make_row("A", -10), 1);
  initial.insert(make_row("A", 5), 1);
  initial.insert(make_row("A", -20), 1);
  view->apply_changes("source_table", initial);

  REQUIRE(get_agg_value(view->get_result(), "A") == -20);

  // Delete -20
  DuckDBZSet del;
  del.insert(make_row("A", -20), -1);
  view->apply_changes("source_table", del);

  REQUIRE(get_agg_value(view->get_result(), "A") == -10);
}

// ============================================================================
// Single Value Tests
// ============================================================================

TEST_CASE("MIN equals MAX when single value", "[unit][minmax]") {
  auto min_view = make_minmax_view("single_min", NativeAggregateView::AggType::MIN);
  auto max_view = make_minmax_view("single_max", NativeAggregateView::AggType::MAX);

  DuckDBZSet initial;
  initial.insert(make_row("A", 42), 1);

  min_view->apply_changes("source_table", initial);
  max_view->apply_changes("source_table", initial);

  REQUIRE(get_agg_value(min_view->get_result(), "A") == 42);
  REQUIRE(get_agg_value(max_view->get_result(), "A") == 42);
}

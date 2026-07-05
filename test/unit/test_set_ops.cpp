#include "catch.hpp"
#include "dbsp_set_ops.hpp"

using namespace dbsp_native;

TEST_CASE("NativeSetView Execution", "[set_ops][execution]") {
  TableSchema schema;
  schema.table_name = "t";
  schema.columns.push_back({"a", duckdb::LogicalType::INTEGER});

  auto left_view = std::make_unique<NativeFilterView>(
      "left", "SELECT * FROM t1", "t1", schema,
      [](const DuckDBRow &r) { return true; });
  auto right_view = std::make_unique<NativeFilterView>(
      "right", "SELECT * FROM t2", "t2", schema,
      [](const DuckDBRow &r) { return true; });

  SECTION("UNION ALL") {
    NativeSetView view("union_all", "--", std::move(left_view),
                       std::move(right_view), schema,
                       duckdb::SetOperationType::UNION, true);

    DuckDBRow r1;
    r1.columns.push_back(duckdb::Value::INTEGER(1));
    DuckDBRow r2;
    r2.columns.push_back(duckdb::Value::INTEGER(2));

    DuckDBZSet in1;
    in1.insert(r1, 1);
    view.apply_changes("t1", in1);
    REQUIRE(view.get_result().get(r1) == 1);

    DuckDBZSet in2;
    in2.insert(r1, 1);
    in2.insert(r2, 1);
    view.apply_changes("t2", in2);
    REQUIRE(view.get_result().get(r1) == 2);
    REQUIRE(view.get_result().get(r2) == 1);

    // Deletion
    DuckDBZSet in3;
    in3.insert(r1, -1);
    view.apply_changes("t1", in3);
    REQUIRE(view.get_result().get(r1) == 1);
  }

  SECTION("UNION (Distinct)") {
    NativeSetView view("union_distinct", "--", std::move(left_view),
                       std::move(right_view), schema,
                       duckdb::SetOperationType::UNION, false);

    DuckDBRow r1;
    r1.columns.push_back(duckdb::Value::INTEGER(1));

    DuckDBZSet in1;
    in1.insert(r1, 1);
    view.apply_changes("t1", in1);
    REQUIRE(view.get_result().get(r1) == 1);

    DuckDBZSet in2;
    in2.insert(r1, 1);
    view.apply_changes("t2", in2);
    REQUIRE(view.get_result().get(r1) == 1); // Still 1 because of distinct

    DuckDBZSet in3;
    in3.insert(r1, -1);
    view.apply_changes("t1", in3);
    REQUIRE(view.get_result().get(r1) == 1); // Still 1 because it's still in t2

    DuckDBZSet in4;
    in4.insert(r1, -1);
    view.apply_changes("t2", in4);
    REQUIRE(view.get_result().get(r1) == 0); // Both gone
  }

  SECTION("INTERSECT") {
    NativeSetView view("intersect", "--", std::move(left_view),
                       std::move(right_view), schema,
                       duckdb::SetOperationType::INTERSECT, false);

    DuckDBRow r1;
    r1.columns.push_back(duckdb::Value::INTEGER(1));

    DuckDBZSet in1;
    in1.insert(r1, 1);
    view.apply_changes("t1", in1);
    REQUIRE(view.get_result().get(r1) == 0); // Not in both yet

    DuckDBZSet in2;
    in2.insert(r1, 1);
    view.apply_changes("t2", in2);
    REQUIRE(view.get_result().get(r1) == 1); // Now in both

    DuckDBZSet in3;
    in3.insert(r1, -1);
    view.apply_changes("t1", in3);
    REQUIRE(view.get_result().get(r1) == 0); // Gone from t1
  }

  SECTION("EXCEPT") {
    NativeSetView view("except", "--", std::move(left_view),
                       std::move(right_view), schema,
                       duckdb::SetOperationType::EXCEPT, false);

    DuckDBRow r1;
    r1.columns.push_back(duckdb::Value::INTEGER(1));

    DuckDBZSet in1;
    in1.insert(r1, 1);
    view.apply_changes("t1", in1);
    REQUIRE(view.get_result().get(r1) == 1); // In t1, not in t2

    DuckDBZSet in2;
    in2.insert(r1, 1);
    view.apply_changes("t2", in2);
    REQUIRE(view.get_result().get(r1) == 0); // Now in t2, so excluded

    DuckDBZSet in3;
    in3.insert(r1, -1);
    view.apply_changes("t2", in3);
    REQUIRE(view.get_result().get(r1) ==
            1); // Removed from t2, so back in result
  }
}

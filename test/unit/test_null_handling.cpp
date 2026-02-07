#include "catch.hpp"
#include "dbsp_duckdb_types.hpp"

using namespace dbsp_native;
using namespace duckdb;

TEST_CASE("NULL-aware row equality", "[null][equality]") {
  SECTION("NULL == NULL in default equality (GROUP BY/DISTINCT semantics)") {
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(1));
    row1.columns.push_back(Value(LogicalType::BIGINT)); // NULL

    DuckDBRow row2;
    row2.columns.push_back(Value::BIGINT(1));
    row2.columns.push_back(Value(LogicalType::BIGINT)); // NULL

    // For GROUP BY/DISTINCT: NULL == NULL
    REQUIRE(row1 == row2);
  }

  SECTION("NULL != non-NULL") {
    DuckDBRow row1;
    row1.columns.push_back(Value(LogicalType::BIGINT)); // NULL

    DuckDBRow row2;
    row2.columns.push_back(Value::BIGINT(1));

    REQUIRE_FALSE(row1 == row2);
  }

  SECTION("Multiple NULLs match") {
    DuckDBRow row1;
    row1.columns.push_back(Value(LogicalType::BIGINT));  // NULL
    row1.columns.push_back(Value(LogicalType::VARCHAR)); // NULL

    DuckDBRow row2;
    row2.columns.push_back(Value(LogicalType::BIGINT));  // NULL
    row2.columns.push_back(Value(LogicalType::VARCHAR)); // NULL

    REQUIRE(row1 == row2);
  }

  SECTION("Mixed NULL and non-NULL") {
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(1));
    row1.columns.push_back(Value(LogicalType::BIGINT)); // NULL

    DuckDBRow row2;
    row2.columns.push_back(Value::BIGINT(1));
    row2.columns.push_back(Value::BIGINT(2));

    REQUIRE_FALSE(row1 == row2);
  }
}

TEST_CASE("NULL-aware row hashing", "[null][hash]") {
  SECTION("NULL values hash consistently") {
    DuckDBRow row1;
    row1.columns.push_back(Value(LogicalType::BIGINT)); // NULL

    DuckDBRow row2;
    row2.columns.push_back(Value(LogicalType::BIGINT)); // NULL

    DuckDBRowHash hasher;
    REQUIRE(hasher(row1) == hasher(row2));
  }

  SECTION("NULL hashes differently from values") {
    DuckDBRow row_null;
    row_null.columns.push_back(Value(LogicalType::BIGINT)); // NULL

    DuckDBRow row_zero;
    row_zero.columns.push_back(Value::BIGINT(0));

    DuckDBRowHash hasher;
    // Different values should (usually) hash differently
    // Note: Hash collisions are possible but unlikely
    REQUIRE(hasher(row_null) != hasher(row_zero));
  }
}

TEST_CASE("JOIN-specific equality", "[null][join]") {
  SECTION("NULL != NULL in JOINs") {
    DuckDBRow row1;
    row1.columns.push_back(Value(LogicalType::BIGINT)); // NULL

    DuckDBRow row2;
    row2.columns.push_back(Value(LogicalType::BIGINT)); // NULL

    JoinRowEqual join_eq;
    // In JOINs: NULL never matches NULL
    REQUIRE_FALSE(join_eq(row1, row2));
  }

  SECTION("Non-NULL values match") {
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(42));

    DuckDBRow row2;
    row2.columns.push_back(Value::BIGINT(42));

    JoinRowEqual join_eq;
    REQUIRE(join_eq(row1, row2));
  }

  SECTION("NULL on either side prevents match") {
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(1));
    row1.columns.push_back(Value(LogicalType::BIGINT)); // NULL

    DuckDBRow row2;
    row2.columns.push_back(Value::BIGINT(1));
    row2.columns.push_back(Value::BIGINT(2));

    JoinRowEqual join_eq;
    REQUIRE_FALSE(join_eq(row1, row2));
  }
}

TEST_CASE("NULL in DISTINCT", "[null][distinct]") {
  SECTION("Multiple NULL rows deduplicate to one") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"value", LogicalType::BIGINT});

    NativeDistinctView view("test_distinct", "SELECT DISTINCT value FROM t",
                            "t", schema);

    // Insert multiple rows with NULL
    DuckDBZSet changes;
    DuckDBRow null_row1;
    null_row1.columns.push_back(Value(LogicalType::BIGINT));
    changes.insert(null_row1, 1);

    DuckDBRow null_row2;
    null_row2.columns.push_back(Value(LogicalType::BIGINT));
    changes.insert(null_row2, 1); // Same as null_row1

    view.apply_changes("t", changes);

    // Should have only one NULL in result
    const auto &result = view.get_result();
    REQUIRE(result.size() == 1);
    REQUIRE(result.get(null_row1) == 1);
  }

  SECTION("NULL and non-NULL values are distinct") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"value", LogicalType::BIGINT});

    NativeDistinctView view("test_distinct", "SELECT DISTINCT value FROM t",
                            "t", schema);

    DuckDBZSet changes;

    DuckDBRow null_row;
    null_row.columns.push_back(Value(LogicalType::BIGINT));
    changes.insert(null_row, 1);

    DuckDBRow value_row;
    value_row.columns.push_back(Value::BIGINT(42));
    changes.insert(value_row, 1);

    view.apply_changes("t", changes);

    const auto &result = view.get_result();
    REQUIRE(result.size() == 2);
  }
}

TEST_CASE("NULL in aggregates - SUM", "[null][aggregate][sum]") {
  SECTION("SUM ignores NULL values") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"key", LogicalType::BIGINT});
    schema.columns.push_back({"sum", LogicalType::BIGINT});

    auto key_fn = [](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      key.columns.push_back(row.columns[0]);
      return key;
    };

    auto value_fn = [](const DuckDBRow &row) -> Value {
      return row.columns[1];
    };

    NativeAggregateView view(
        "test_agg", "SELECT key, SUM(val) FROM t GROUP BY key", "t", schema,
        key_fn, value_fn, NativeAggregateView::AggType::SUM);

    DuckDBZSet changes;

    // Key 1: values 10, NULL, 20
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(1));
    row1.columns.push_back(Value::BIGINT(10));
    changes.insert(row1, 1);

    DuckDBRow row2;
    row2.columns.push_back(Value::BIGINT(1));
    row2.columns.push_back(Value(LogicalType::BIGINT)); // NULL
    changes.insert(row2, 1);

    DuckDBRow row3;
    row3.columns.push_back(Value::BIGINT(1));
    row3.columns.push_back(Value::BIGINT(20));
    changes.insert(row3, 1);

    view.apply_changes("t", changes);

    const auto &result = view.get_result();

    // Should have one group with sum=30 (NULL ignored)
    DuckDBRow expected_key;
    expected_key.columns.push_back(Value::BIGINT(1));
    expected_key.columns.push_back(Value::BIGINT(30));

    REQUIRE(result.get(expected_key) == 1);
  }

  SECTION("SUM of all NULLs returns NULL") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"key", LogicalType::BIGINT});
    schema.columns.push_back({"sum", LogicalType::BIGINT});

    auto key_fn = [](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      key.columns.push_back(row.columns[0]);
      return key;
    };

    auto value_fn = [](const DuckDBRow &row) -> Value {
      return row.columns[1];
    };

    NativeAggregateView view(
        "test_agg", "SELECT key, SUM(val) FROM t GROUP BY key", "t", schema,
        key_fn, value_fn, NativeAggregateView::AggType::SUM);

    DuckDBZSet changes;

    // Key 1: all NULL values
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(1));
    row1.columns.push_back(Value(LogicalType::BIGINT)); // NULL
    changes.insert(row1, 1);

    DuckDBRow row2;
    row2.columns.push_back(Value::BIGINT(1));
    row2.columns.push_back(Value(LogicalType::BIGINT)); // NULL
    changes.insert(row2, 1);

    view.apply_changes("t", changes);

    const auto &result = view.get_result();

    // Should have one group with SUM=NULL
    DuckDBRow expected_key;
    expected_key.columns.push_back(Value::BIGINT(1));
    expected_key.columns.push_back(Value(LogicalType::BIGINT)); // NULL result

    REQUIRE(result.get(expected_key) == 1);
  }
}

TEST_CASE("NULL in aggregates - COUNT", "[null][aggregate][count]") {
  SECTION("COUNT(column) excludes NULLs") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"key", LogicalType::BIGINT});
    schema.columns.push_back({"count", LogicalType::BIGINT});

    auto key_fn = [](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      key.columns.push_back(row.columns[0]);
      return key;
    };

    auto value_fn = [](const DuckDBRow &row) -> Value {
      return row.columns[1];
    };

    NativeAggregateView view(
        "test_agg", "SELECT key, COUNT(val) FROM t GROUP BY key", "t", schema,
        key_fn, value_fn, NativeAggregateView::AggType::COUNT);

    DuckDBZSet changes;

    // Key 1: 2 values, 1 NULL
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(1));
    row1.columns.push_back(Value::BIGINT(10));
    changes.insert(row1, 1);

    DuckDBRow row2;
    row2.columns.push_back(Value::BIGINT(1));
    row2.columns.push_back(Value(LogicalType::BIGINT)); // NULL
    changes.insert(row2, 1);

    DuckDBRow row3;
    row3.columns.push_back(Value::BIGINT(1));
    row3.columns.push_back(Value::BIGINT(20));
    changes.insert(row3, 1);

    view.apply_changes("t", changes);

    const auto &result = view.get_result();

    // COUNT should be 2 (excludes NULL)
    DuckDBRow expected_key;
    expected_key.columns.push_back(Value::BIGINT(1));
    expected_key.columns.push_back(Value::BIGINT(2));

    REQUIRE(result.get(expected_key) == 1);
  }

  SECTION("COUNT of all NULLs returns 0") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"key", LogicalType::BIGINT});
    schema.columns.push_back({"count", LogicalType::BIGINT});

    auto key_fn = [](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      key.columns.push_back(row.columns[0]);
      return key;
    };

    auto value_fn = [](const DuckDBRow &row) -> Value {
      return row.columns[1];
    };

    NativeAggregateView view(
        "test_agg", "SELECT key, COUNT(val) FROM t GROUP BY key", "t", schema,
        key_fn, value_fn, NativeAggregateView::AggType::COUNT);

    DuckDBZSet changes;

    // Key 1: all NULL values
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(1));
    row1.columns.push_back(Value(LogicalType::BIGINT)); // NULL
    changes.insert(row1, 1);

    DuckDBRow row2;
    row2.columns.push_back(Value::BIGINT(1));
    row2.columns.push_back(Value(LogicalType::BIGINT)); // NULL
    changes.insert(row2, 1);

    view.apply_changes("t", changes);

    const auto &result = view.get_result();

    // COUNT should be 0
    DuckDBRow expected_key;
    expected_key.columns.push_back(Value::BIGINT(1));
    expected_key.columns.push_back(Value::BIGINT(0));

    REQUIRE(result.get(expected_key) == 1);
  }
}

TEST_CASE("NULL in aggregates - AVG", "[null][aggregate][avg]") {
  SECTION("AVG ignores NULL values") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"key", LogicalType::BIGINT});
    schema.columns.push_back({"avg", LogicalType::BIGINT});

    auto key_fn = [](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      key.columns.push_back(row.columns[0]);
      return key;
    };

    auto value_fn = [](const DuckDBRow &row) -> Value {
      return row.columns[1];
    };

    NativeAggregateView view(
        "test_agg", "SELECT key, AVG(val) FROM t GROUP BY key", "t", schema,
        key_fn, value_fn, NativeAggregateView::AggType::AVG);

    DuckDBZSet changes;

    // Key 1: values 10, NULL, 20 -> avg should be 15 (30/2, not 30/3)
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(1));
    row1.columns.push_back(Value::BIGINT(10));
    changes.insert(row1, 1);

    DuckDBRow row2;
    row2.columns.push_back(Value::BIGINT(1));
    row2.columns.push_back(Value(LogicalType::BIGINT)); // NULL
    changes.insert(row2, 1);

    DuckDBRow row3;
    row3.columns.push_back(Value::BIGINT(1));
    row3.columns.push_back(Value::BIGINT(20));
    changes.insert(row3, 1);

    view.apply_changes("t", changes);

    const auto &result = view.get_result();

    // AVG = 30/2 = 15
    DuckDBRow expected_key;
    expected_key.columns.push_back(Value::BIGINT(1));
    expected_key.columns.push_back(Value::BIGINT(15));

    REQUIRE(result.get(expected_key) == 1);
  }

  SECTION("AVG of all NULLs returns NULL") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"key", LogicalType::BIGINT});
    schema.columns.push_back({"avg", LogicalType::BIGINT});

    auto key_fn = [](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      key.columns.push_back(row.columns[0]);
      return key;
    };

    auto value_fn = [](const DuckDBRow &row) -> Value {
      return row.columns[1];
    };

    NativeAggregateView view(
        "test_agg", "SELECT key, AVG(val) FROM t GROUP BY key", "t", schema,
        key_fn, value_fn, NativeAggregateView::AggType::AVG);

    DuckDBZSet changes;

    // All NULL values
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(1));
    row1.columns.push_back(Value(LogicalType::BIGINT)); // NULL
    changes.insert(row1, 1);

    view.apply_changes("t", changes);

    const auto &result = view.get_result();

    // AVG of all NULLs = NULL
    DuckDBRow expected_key;
    expected_key.columns.push_back(Value::BIGINT(1));
    expected_key.columns.push_back(Value(LogicalType::BIGINT)); // NULL

    REQUIRE(result.get(expected_key) == 1);
  }
}

TEST_CASE("NULL in GROUP BY", "[null][group_by]") {
  SECTION("NULL values form their own group") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"key", LogicalType::BIGINT});
    schema.columns.push_back({"count", LogicalType::BIGINT});

    auto key_fn = [](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      key.columns.push_back(row.columns[0]); // Group by first column
      return key;
    };

    auto value_fn = [](const DuckDBRow &row) -> Value {
      return row.columns[1];
    };

    NativeAggregateView view(
        "test_agg", "SELECT key, COUNT(val) FROM t GROUP BY key", "t", schema,
        key_fn, value_fn, NativeAggregateView::AggType::COUNT);

    DuckDBZSet changes;

    // Group 1: key=1
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(1));
    row1.columns.push_back(Value::BIGINT(10));
    changes.insert(row1, 1);

    // Group 2: key=NULL
    DuckDBRow row2;
    row2.columns.push_back(Value(LogicalType::BIGINT)); // NULL key
    row2.columns.push_back(Value::BIGINT(20));
    changes.insert(row2, 1);

    // Group 2 again: key=NULL (should go to same group)
    DuckDBRow row3;
    row3.columns.push_back(Value(LogicalType::BIGINT)); // NULL key
    row3.columns.push_back(Value::BIGINT(30));
    changes.insert(row3, 1);

    view.apply_changes("t", changes);

    const auto &result = view.get_result();

    // Should have 2 groups
    REQUIRE(result.size() == 2);

    // Group with key=1, count=1
    DuckDBRow group1;
    group1.columns.push_back(Value::BIGINT(1));
    group1.columns.push_back(Value::BIGINT(1));
    REQUIRE(result.get(group1) == 1);

    // Group with key=NULL, count=2
    DuckDBRow group2;
    group2.columns.push_back(Value(LogicalType::BIGINT)); // NULL
    group2.columns.push_back(Value::BIGINT(2));
    REQUIRE(result.get(group2) == 1);
  }
}

TEST_CASE("NULL in JOINs", "[null][join]") {
  SECTION("NULL keys don't match in JOIN") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"left_id", LogicalType::BIGINT});
    schema.columns.push_back({"right_id", LogicalType::BIGINT});

    auto left_key = [](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      key.columns.push_back(row.columns[0]);
      return key;
    };

    auto right_key = [](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      key.columns.push_back(row.columns[0]);
      return key;
    };

    NativeJoinView view("test_join", "SELECT * FROM l JOIN r ON l.id = r.id",
                        "left", "right", schema, left_key, right_key);

    // Left table: rows with key=NULL and key=1
    DuckDBZSet left_changes;

    DuckDBRow left_null;
    left_null.columns.push_back(Value(LogicalType::BIGINT)); // NULL
    left_changes.insert(left_null, 1);

    DuckDBRow left_1;
    left_1.columns.push_back(Value::BIGINT(1));
    left_changes.insert(left_1, 1);

    view.apply_changes("left", left_changes);

    // Right table: rows with key=NULL and key=1
    DuckDBZSet right_changes;

    DuckDBRow right_null;
    right_null.columns.push_back(Value(LogicalType::BIGINT)); // NULL
    right_changes.insert(right_null, 1);

    DuckDBRow right_1;
    right_1.columns.push_back(Value::BIGINT(1));
    right_changes.insert(right_1, 1);

    view.apply_changes("right", right_changes);

    const auto &result = view.get_result();

    // Should have only 1 join result (1=1), not (NULL=NULL)
    REQUIRE(result.size() == 1);

    // Verify it's the (1,1) join
    DuckDBRow expected;
    expected.columns.push_back(Value::BIGINT(1));
    expected.columns.push_back(Value::BIGINT(1));
    REQUIRE(result.get(expected) == 1);
  }
}

TEST_CASE("Incremental NULL updates", "[null][incremental]") {
  SECTION("Insert then delete NULL value") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"value", LogicalType::BIGINT});

    NativeDistinctView view("test_distinct", "SELECT DISTINCT value FROM t",
                            "t", schema);

    // Insert NULL
    DuckDBZSet changes1;
    DuckDBRow null_row;
    null_row.columns.push_back(Value(LogicalType::BIGINT));
    changes1.insert(null_row, 1);
    view.apply_changes("t", changes1);

    REQUIRE(view.get_result().size() == 1);

    // Delete NULL
    DuckDBZSet changes2;
    changes2.insert(null_row, -1);
    view.apply_changes("t", changes2);

    REQUIRE(view.get_result().size() == 0);
  }

  SECTION("Update value to NULL") {
    TableSchema schema;
    schema.table_name = "test";
    schema.columns.push_back({"key", LogicalType::BIGINT});
    schema.columns.push_back({"count", LogicalType::BIGINT});

    auto key_fn = [](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      key.columns.push_back(Value::BIGINT(1)); // Fixed key
      return key;
    };

    auto value_fn = [](const DuckDBRow &row) -> Value {
      return row.columns[0];
    };

    NativeAggregateView view("test_agg", "SELECT COUNT(val) FROM t", "t",
                             schema, key_fn, value_fn,
                             NativeAggregateView::AggType::COUNT);

    // Insert value=10
    DuckDBZSet changes1;
    DuckDBRow row1;
    row1.columns.push_back(Value::BIGINT(10));
    changes1.insert(row1, 1);
    view.apply_changes("t", changes1);

    // Count should be 1
    DuckDBRow key;
    key.columns.push_back(Value::BIGINT(1));
    key.columns.push_back(Value::BIGINT(1));
    REQUIRE(view.get_result().get(key) == 1);

    // Update: delete 10, insert NULL (simulates UPDATE SET val=NULL)
    DuckDBZSet changes2;
    changes2.insert(row1, -1);
    DuckDBRow row_null;
    row_null.columns.push_back(Value(LogicalType::BIGINT));
    changes2.insert(row_null, 1);
    view.apply_changes("t", changes2);

    // Count should be 0 (NULL excluded)
    DuckDBRow key2;
    key2.columns.push_back(Value::BIGINT(1));
    key2.columns.push_back(Value::BIGINT(0));
    REQUIRE(view.get_result().get(key2) == 1);
  }
}

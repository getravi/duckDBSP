#include <catch2/catch_test_macros.hpp>
#include "../test_helpers.hpp"

using namespace dbsp_test;

TEST_CASE("dbsp_sync detects insertions", "[integration][cdc][sync]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("orders", "id INT, amount DECIMAL", {"(1, 100.0)"});
    db.exec("SELECT * FROM dbsp_track('orders')");
    db.exec("SELECT * FROM dbsp_sync('orders')");
    db.exec("SELECT * FROM dbsp_create_view('totals', 'SELECT SUM(amount) FROM orders')");

    // Initial state
    auto rows = db.getViewRows("totals");
    REQUIRE(rows.size() == 1);
    auto initial_sum = rows[0][0].GetValue<double>();
    REQUIRE(initial_sum == 100.0);

    // Insert new row
    db.exec("INSERT INTO orders VALUES (2, 200.0)");
    db.exec("SELECT * FROM dbsp_sync('orders')");

    // View should be updated
    rows = db.getViewRows("totals");
    REQUIRE(rows.size() == 1);
    auto new_sum = rows[0][0].GetValue<double>();
    REQUIRE(new_sum == 300.0);
}

TEST_CASE("dbsp_sync detects deletions", "[integration][cdc][sync]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("orders", "id INT, customer VARCHAR, amount DECIMAL",
                   {"(1, 'Alice', 100.0)", "(2, 'Bob', 200.0)"});
    db.exec("SELECT * FROM dbsp_track('orders')");
    db.exec("SELECT * FROM dbsp_sync('orders')");
    db.exec("SELECT * FROM dbsp_create_view('count_view', 'SELECT COUNT(*) FROM orders')");

    // Initial count
    auto rows = db.getViewRows("count_view");
    auto initial_count = rows[0][0].GetValue<int64_t>();
    REQUIRE(initial_count == 2);

    // Delete row
    db.exec("DELETE FROM orders WHERE id = 1");
    db.exec("SELECT * FROM dbsp_sync('orders')");

    // Count should decrease
    rows = db.getViewRows("count_view");
    auto new_count = rows[0][0].GetValue<int64_t>();
    REQUIRE(new_count == 1);
}

TEST_CASE("dbsp_sync handles batch changes", "[integration][cdc][batch]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("orders", "id INT, customer VARCHAR, amount DECIMAL", {});
    db.exec("SELECT * FROM dbsp_track('orders')");
    db.exec("SELECT * FROM dbsp_sync('orders')");
    db.exec("SELECT * FROM dbsp_create_view('customer_totals', "
            "'SELECT customer, SUM(amount) FROM orders GROUP BY customer')");

    // Insert multiple rows at once
    db.exec("INSERT INTO orders VALUES (1, 'Alice', 100.0), (2, 'Bob', 200.0), "
            "(3, 'Alice', 150.0), (4, 'Charlie', 300.0)");
    db.exec("SELECT * FROM dbsp_sync('orders')");

    // Verify all updates processed
    db.assertViewRowCount("customer_totals", 3);

    auto rows = db.getViewRows("customer_totals");
    REQUIRE(rows.size() == 3);
}

TEST_CASE("dbsp_sync() syncs all tables", "[integration][cdc][sync-all]") {
    DuckDBTestHarness db;

    // Setup two tables
    db.createTable("t1", "id INT", {"(1)"});
    db.createTable("t2", "id INT", {"(2)"});
    db.exec("SELECT * FROM dbsp_track('t1')");
    db.exec("SELECT * FROM dbsp_track('t2')");
    db.exec("SELECT * FROM dbsp_sync()");

    db.exec("SELECT * FROM dbsp_create_view('v1', 'SELECT COUNT(*) FROM t1')");
    db.exec("SELECT * FROM dbsp_create_view('v2', 'SELECT COUNT(*) FROM t2')");

    // Insert into both
    db.exec("INSERT INTO t1 VALUES (10)");
    db.exec("INSERT INTO t2 VALUES (20)");

    // Sync all at once
    db.exec("SELECT * FROM dbsp_sync()");

    // Both views updated
    auto rows1 = db.getViewRows("v1");
    auto rows2 = db.getViewRows("v2");
    REQUIRE(rows1[0][0].GetValue<int64_t>() == 2);
    REQUIRE(rows2[0][0].GetValue<int64_t>() == 2);
}

TEST_CASE("dbsp_notify_insert manual CDC", "[integration][cdc][manual]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("orders", "id INT, amount DECIMAL", {});
    db.exec("SELECT * FROM dbsp_track('orders')");
    db.exec("SELECT * FROM dbsp_sync('orders')");
    db.exec("SELECT * FROM dbsp_create_view('total', 'SELECT SUM(amount) FROM orders')");

    // Manually notify insert
    db.exec("SELECT * FROM dbsp_notify_insert('orders', 1, 100.0)");

    // View should reflect change
    auto rows = db.getViewRows("total");
    auto sum = rows[0][0].GetValue<double>();
    REQUIRE(sum == 100.0);
}

TEST_CASE("Incremental updates are O(delta)", "[integration][performance]") {
    DuckDBTestHarness db;

    // Setup large table
    db.exec("CREATE TABLE large_table (id INT, category VARCHAR, value INT)");
    db.exec("SELECT * FROM dbsp_track('large_table')");

    // Insert 1000 rows
    for (int i = 0; i < 1000; i++) {
        db.exec("INSERT INTO large_table VALUES (" + std::to_string(i) +
                ", 'cat" + std::to_string(i % 10) + "', " + std::to_string(i * 10) + ")");
    }
    db.exec("SELECT * FROM dbsp_sync('large_table')");

    // Create aggregate view
    db.exec("SELECT * FROM dbsp_create_view('category_sums', "
            "'SELECT category, SUM(value) FROM large_table GROUP BY category')");

    // Insert one more row - should be fast
    db.exec("INSERT INTO large_table VALUES (1001, 'cat5', 10000)");
    db.exec("SELECT * FROM dbsp_sync('large_table')");

    // Verify view updated correctly
    db.assertViewRowCount("category_sums", 10);

    // This test just verifies correctness - actual performance benchmarking
    // comes in Phase 4
}

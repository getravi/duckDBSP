#include <catch2/catch_test_macros.hpp>
#include "../test_helpers.hpp"

using namespace dbsp_test;

TEST_CASE("dbsp_track tracks table", "[integration][track]") {
    DuckDBTestHarness db;

    // Create test table
    db.exec("CREATE TABLE orders (id INT, customer VARCHAR, amount DECIMAL)");

    // Track the table
    auto result = db.query("SELECT * FROM dbsp_track('orders')");
    REQUIRE_FALSE(result->HasError());

    // Verify response
    auto msg = result->GetValue(0, 0).ToString();
    REQUIRE(msg.find("Tracking table: orders") != std::string::npos);

    // Verify table appears in tracked list
    auto tables = db.query("SELECT * FROM dbsp_tables()");
    REQUIRE_FALSE(tables->HasError());
    REQUIRE(tables->RowCount() >= 1);
}

TEST_CASE("dbsp_create_view creates filter view", "[integration][view]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("orders",
                   "id INT, customer VARCHAR, amount DECIMAL",
                   {"(1, 'Alice', 100.0)", "(2, 'Bob', 200.0)", "(3, 'Alice', 50.0)"});
    db.exec("SELECT * FROM dbsp_track('orders')");
    db.exec("SELECT * FROM dbsp_sync('orders')");

    // Create filter view
    auto result = db.query(
        "SELECT * FROM dbsp_create_view('high_value', "
        "'SELECT * FROM orders WHERE amount > 100')");
    REQUIRE_FALSE(result->HasError());

    // Verify view is listed
    auto views = db.query("SELECT * FROM dbsp_views()");
    REQUIRE_FALSE(views->HasError());
    REQUIRE(views->RowCount() >= 1);

    // Query the view
    db.assertViewRowCount("high_value", 1);
}

TEST_CASE("dbsp_create_view creates aggregate view", "[integration][view][aggregate]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("orders",
                   "id INT, customer VARCHAR, amount DECIMAL",
                   {"(1, 'Alice', 100.0)", "(2, 'Bob', 200.0)", "(3, 'Alice', 50.0)"});
    db.exec("SELECT * FROM dbsp_track('orders')");
    db.exec("SELECT * FROM dbsp_sync('orders')");

    // Create aggregate view
    auto result = db.query(
        "SELECT * FROM dbsp_create_view('totals', "
        "'SELECT customer, SUM(amount) FROM orders GROUP BY customer')");
    REQUIRE_FALSE(result->HasError());

    // Query the view
    auto rows = db.getViewRows("totals");
    REQUIRE(rows.size() == 2);
}

TEST_CASE("dbsp_query returns view data", "[integration][query]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("products",
                   "id INT, name VARCHAR, price DECIMAL",
                   {"(1, 'Widget', 10.0)", "(2, 'Gadget', 20.0)"});
    db.exec("SELECT * FROM dbsp_track('products')");
    db.exec("SELECT * FROM dbsp_sync('products')");
    db.exec("SELECT * FROM dbsp_create_view('all_products', 'SELECT * FROM products')");

    // Query view
    auto result = db.query("SELECT * FROM dbsp_query('all_products')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 2);
    REQUIRE(result->ColumnCount() == 3);
}

TEST_CASE("dbsp_drop removes view", "[integration][drop]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("items", "id INT", {"(1)", "(2)"});
    db.exec("SELECT * FROM dbsp_track('items')");
    db.exec("SELECT * FROM dbsp_sync('items')");
    db.exec("SELECT * FROM dbsp_create_view('v1', 'SELECT * FROM items')");

    // Drop view
    auto result = db.query("SELECT dbsp_drop('v1')");
    REQUIRE_FALSE(result->HasError());

    // Verify view is gone
    auto views = db.query("SELECT * FROM dbsp_views()");
    REQUIRE(views->RowCount() == 0);
}

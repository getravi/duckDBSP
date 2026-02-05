#include <catch2/catch_test_macros.hpp>
#include "../test_helpers.hpp"
#include <filesystem>

using namespace dbsp_test;

TEST_CASE("dbsp_save to DuckDB table", "[integration][persistence][save]") {
    DuckDBTestHarness db;

    // Setup base table
    db.createTable("products",
                   "id INT, name VARCHAR, price DECIMAL",
                   {"(1, 'Widget', 10.0)", "(2, 'Gadget', 20.0)", "(3, 'Doohickey', 15.0)"});
    db.exec("SELECT * FROM dbsp_track('products')");
    db.exec("SELECT * FROM dbsp_sync('products')");

    // Create a view
    db.exec("SELECT * FROM dbsp_create_view('expensive_products', "
            "'SELECT * FROM products WHERE price >= 15')");

    // Save to DuckDB table
    auto result = db.query("SELECT * FROM dbsp_save('expensive_products', 'saved_view_table')");
    REQUIRE_FALSE(result->HasError());
    auto msg = result->GetValue(0, 0).ToString();
    REQUIRE(msg.find("Saved") != std::string::npos);

    // Verify the table was created and contains correct metadata
    auto table_check = db.query("SELECT * FROM saved_view_table");
    REQUIRE_FALSE(table_check->HasError());
    REQUIRE(table_check->RowCount() == 1);

    // Verify metadata columns exist
    auto view_name = table_check->GetValue(0, 0).ToString();
    auto query_text = table_check->GetValue(1, 0).ToString();
    REQUIRE(view_name == "expensive_products");
    REQUIRE(query_text.find("price >= 15") != std::string::npos);
}

TEST_CASE("dbsp_load from DuckDB table", "[integration][persistence][load]") {
    DuckDBTestHarness db;

    // Setup base table
    db.createTable("inventory",
                   "item VARCHAR, quantity INT",
                   {"('Apples', 50)", "('Oranges', 30)", "('Bananas', 20)"});
    db.exec("SELECT * FROM dbsp_track('inventory')");
    db.exec("SELECT * FROM dbsp_sync('inventory')");

    // Create and save a view
    db.exec("SELECT * FROM dbsp_create_view('low_stock', "
            "'SELECT * FROM inventory WHERE quantity < 40')");
    db.exec("SELECT * FROM dbsp_save('low_stock', 'persisted_views')");

    // Drop the view
    db.exec("SELECT * FROM dbsp_drop('low_stock')");

    // Verify view is gone
    auto views = db.query("SELECT * FROM dbsp_views()");
    REQUIRE(views->RowCount() == 0);

    // Load the view back
    auto result = db.query("SELECT * FROM dbsp_load('persisted_views')");
    REQUIRE_FALSE(result->HasError());
    auto msg = result->GetValue(0, 0).ToString();
    REQUIRE(msg.find("Loaded") != std::string::npos);

    // Verify view exists again
    views = db.query("SELECT * FROM dbsp_views()");
    REQUIRE(views->RowCount() == 1);

    // Query the restored view
    auto rows = db.getViewRows("low_stock");
    REQUIRE(rows.size() == 2); // Oranges and Bananas
}

TEST_CASE("dbsp_save to JSON file", "[integration][persistence][json][save]") {
    DuckDBTestHarness db;
    std::filesystem::path json_file = std::filesystem::temp_directory_path() / "test_view_save.json";

    // Cleanup any existing file
    if (std::filesystem::exists(json_file)) {
        std::filesystem::remove(json_file);
    }

    // Setup base table
    db.createTable("employees",
                   "id INT, name VARCHAR, salary DECIMAL",
                   {"(1, 'Alice', 75000)", "(2, 'Bob', 85000)", "(3, 'Charlie', 65000)"});
    db.exec("SELECT * FROM dbsp_track('employees')");
    db.exec("SELECT * FROM dbsp_sync('employees')");

    // Create a view
    db.exec("SELECT * FROM dbsp_create_view('high_earners', "
            "'SELECT * FROM employees WHERE salary > 70000')");

    // Save to JSON
    auto result = db.query("SELECT * FROM dbsp_save('high_earners', '" + json_file.string() + "', 'json')");
    REQUIRE_FALSE(result->HasError());
    auto msg = result->GetValue(0, 0).ToString();
    REQUIRE(msg.find("Saved") != std::string::npos);

    // Verify file exists
    REQUIRE(std::filesystem::exists(json_file));

    // Cleanup
    std::filesystem::remove(json_file);
}

TEST_CASE("dbsp_load from JSON file", "[integration][persistence][json][load]") {
    DuckDBTestHarness db;
    std::filesystem::path json_file = std::filesystem::temp_directory_path() / "test_view_load.json";

    // Cleanup any existing file
    if (std::filesystem::exists(json_file)) {
        std::filesystem::remove(json_file);
    }

    // Setup base table
    db.createTable("customers",
                   "id INT, name VARCHAR, status VARCHAR",
                   {"(1, 'Alice', 'active')", "(2, 'Bob', 'inactive')", "(3, 'Charlie', 'active')"});
    db.exec("SELECT * FROM dbsp_track('customers')");
    db.exec("SELECT * FROM dbsp_sync('customers')");

    // Create and save a view to JSON
    db.exec("SELECT * FROM dbsp_create_view('active_customers', "
            "'SELECT * FROM customers WHERE status = ''active''')");
    db.exec("SELECT * FROM dbsp_save('active_customers', '" + json_file.string() + "', 'json')");

    // Drop the view
    db.exec("SELECT * FROM dbsp_drop('active_customers')");

    // Load the view back from JSON
    auto result = db.query("SELECT * FROM dbsp_load('" + json_file.string() + "', 'json')");
    REQUIRE_FALSE(result->HasError());
    auto msg = result->GetValue(0, 0).ToString();
    REQUIRE(msg.find("Loaded") != std::string::npos);

    // Verify view exists and has correct data
    auto rows = db.getViewRows("active_customers");
    REQUIRE(rows.size() == 2); // Alice and Charlie

    // Cleanup
    std::filesystem::remove(json_file);
}

TEST_CASE("Persistence round-trip preserves view definitions", "[integration][persistence][roundtrip]") {
    DuckDBTestHarness db;

    // Setup base table
    db.createTable("orders",
                   "id INT, customer VARCHAR, total DECIMAL, status VARCHAR",
                   {
                       "(1, 'Alice', 100.0, 'completed')",
                       "(2, 'Bob', 200.0, 'pending')",
                       "(3, 'Charlie', 150.0, 'completed')",
                       "(4, 'Alice', 50.0, 'pending')"
                   });
    db.exec("SELECT * FROM dbsp_track('orders')");
    db.exec("SELECT * FROM dbsp_sync('orders')");

    // Create complex view with aggregation
    const std::string view_query = "SELECT customer, SUM(total) as total_spent "
                                   "FROM orders WHERE status = 'completed' "
                                   "GROUP BY customer HAVING SUM(total) > 100";
    db.exec("SELECT * FROM dbsp_create_view('big_spenders', '" + view_query + "')");

    // Get initial view results
    auto initial_rows = db.getViewRows("big_spenders");
    REQUIRE(initial_rows.size() == 1); // Only Charlie has >100 in completed

    // Save to table
    db.exec("SELECT * FROM dbsp_save('big_spenders', 'view_backup')");

    // Drop the view
    db.exec("SELECT * FROM dbsp_drop('big_spenders')");

    // Load it back
    db.exec("SELECT * FROM dbsp_load('view_backup')");

    // Verify the view produces same results
    auto restored_rows = db.getViewRows("big_spenders");
    REQUIRE(restored_rows.size() == initial_rows.size());

    // Verify view metadata is preserved
    auto views = db.query("SELECT * FROM dbsp_views()");
    REQUIRE(views->RowCount() == 1);
    auto view_name = views->GetValue(0, 0).ToString();
    REQUIRE(view_name == "big_spenders");

    // Verify the view still updates correctly
    db.exec("UPDATE orders SET status = 'completed' WHERE id = 4");
    db.exec("SELECT * FROM dbsp_sync('orders')");

    // Now Alice should appear (50 + pending 50 = 100, but we need >100)
    // Actually Alice still won't appear as total is exactly 100, not > 100
    // Let's verify count is still 1
    restored_rows = db.getViewRows("big_spenders");
    REQUIRE(restored_rows.size() == 1);
}

TEST_CASE("Loading rebuilds views from current table data", "[integration][persistence][rebuild]") {
    DuckDBTestHarness db;

    // Setup base table
    db.createTable("sales",
                   "id INT, product VARCHAR, amount DECIMAL",
                   {"(1, 'Widget', 100.0)", "(2, 'Gadget', 200.0)"});
    db.exec("SELECT * FROM dbsp_track('sales')");
    db.exec("SELECT * FROM dbsp_sync('sales')");

    // Create view
    db.exec("SELECT * FROM dbsp_create_view('high_value_sales', "
            "'SELECT * FROM sales WHERE amount >= 150')");

    // Initial state: only Gadget (200) qualifies
    auto initial_rows = db.getViewRows("high_value_sales");
    REQUIRE(initial_rows.size() == 1);

    // Save view definition
    db.exec("SELECT * FROM dbsp_save('high_value_sales', 'sales_view_backup')");

    // Drop view
    db.exec("SELECT * FROM dbsp_drop('high_value_sales')");

    // Modify base table while view is dropped
    db.exec("INSERT INTO sales VALUES (3, 'Doohickey', 300.0)");
    db.exec("UPDATE sales SET amount = 250 WHERE id = 1");
    db.exec("SELECT * FROM dbsp_sync('sales')");

    // Load view back - should rebuild from current table state
    db.exec("SELECT * FROM dbsp_load('sales_view_backup')");

    // View should now reflect current data: Widget (250), Gadget (200), Doohickey (300)
    // All three qualify for amount >= 150
    auto rebuilt_rows = db.getViewRows("high_value_sales");
    REQUIRE(rebuilt_rows.size() == 3);

    // Verify the view continues to track changes
    db.exec("DELETE FROM sales WHERE id = 2"); // Remove Gadget
    db.exec("SELECT * FROM dbsp_sync('sales')");

    auto updated_rows = db.getViewRows("high_value_sales");
    REQUIRE(updated_rows.size() == 2); // Widget and Doohickey remain
}

#include <catch2/catch_test_macros.hpp>
#include "../test_helpers.hpp"

using namespace dbsp_test;

TEST_CASE("View can depend on another view", "[integration][cascade][dependency]") {
    DuckDBTestHarness db;

    // Setup base table
    db.createTable("orders",
                   "id INT, customer VARCHAR, amount DECIMAL",
                   {"(1, 'Alice', 100.0)", "(2, 'Bob', 200.0)", "(3, 'Alice', 50.0)"});
    db.exec("SELECT * FROM dbsp_track('orders')");
    db.exec("SELECT * FROM dbsp_sync('orders')");

    // Create first-level view (aggregate)
    db.exec("SELECT * FROM dbsp_create_view('customer_totals', "
            "'SELECT customer, SUM(amount) as total FROM orders GROUP BY customer')");

    // Create second-level view (filter on first view)
    auto result = db.query(
        "SELECT * FROM dbsp_create_view('high_value_customers', "
        "'SELECT * FROM customer_totals WHERE total > 100')");
    REQUIRE_FALSE(result->HasError());

    // Query the cascaded view
    auto rows = db.getViewRows("high_value_customers");
    REQUIRE(rows.size() == 2); // Alice (150) and Bob (200)
}

TEST_CASE("Cascading views update correctly", "[integration][cascade][update]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("sales",
                   "id INT, product VARCHAR, quantity INT, price DECIMAL",
                   {"(1, 'Widget', 5, 10.0)", "(2, 'Gadget', 3, 20.0)"});
    db.exec("SELECT * FROM dbsp_track('sales')");
    db.exec("SELECT * FROM dbsp_sync('sales')");

    // Create chain: base -> totals -> filtered
    db.exec("SELECT * FROM dbsp_create_view('product_revenue', "
            "'SELECT product, SUM(quantity * price) as revenue FROM sales GROUP BY product')");
    db.exec("SELECT * FROM dbsp_create_view('top_products', "
            "'SELECT * FROM product_revenue WHERE revenue > 50')");

    // Initial state: Widget=50, Gadget=60, so only Gadget should be in top_products
    auto rows = db.getViewRows("top_products");
    REQUIRE(rows.size() == 1);

    // Insert more Widget sales
    db.exec("INSERT INTO sales VALUES (3, 'Widget', 10, 10.0)");
    db.exec("SELECT * FROM dbsp_sync('sales')");

    // Now Widget has revenue = 150, so both should be in top_products
    rows = db.getViewRows("top_products");
    REQUIRE(rows.size() == 2);
}

TEST_CASE("Three-level cascade works", "[integration][cascade][deep]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("transactions",
                   "id INT, user_id INT, category VARCHAR, amount DECIMAL",
                   {
                       "(1, 1, 'food', 50.0)",
                       "(2, 1, 'transport', 30.0)",
                       "(3, 2, 'food', 20.0)",
                       "(4, 2, 'transport', 100.0)"
                   });
    db.exec("SELECT * FROM dbsp_track('transactions')");
    db.exec("SELECT * FROM dbsp_sync('transactions')");

    // Level 1: User totals by category
    db.exec("SELECT * FROM dbsp_create_view('user_category_totals', "
            "'SELECT user_id, category, SUM(amount) as total FROM transactions "
            "GROUP BY user_id, category')");

    // Level 2: User grand totals
    db.exec("SELECT * FROM dbsp_create_view('user_totals', "
            "'SELECT user_id, SUM(total) as grand_total FROM user_category_totals "
            "GROUP BY user_id')");

    // Level 3: High spenders
    db.exec("SELECT * FROM dbsp_create_view('high_spenders', "
            "'SELECT * FROM user_totals WHERE grand_total > 100')");

    // Verify: user_id=1 has 80 total, user_id=2 has 120 total
    // Only user_id=2 should appear in high_spenders
    auto rows = db.getViewRows("high_spenders");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][0].GetValue<int64_t>() == 2);

    // Add transaction for user 1 to push them over 100
    db.exec("INSERT INTO transactions VALUES (5, 1, 'entertainment', 50.0)");
    db.exec("SELECT * FROM dbsp_sync('transactions')");

    // Now both users should be high spenders
    rows = db.getViewRows("high_spenders");
    REQUIRE(rows.size() == 2);
}

TEST_CASE("dbsp_deps shows dependencies", "[integration][cascade][deps]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("items", "id INT, value INT", {"(1, 100)"});
    db.exec("SELECT * FROM dbsp_track('items')");
    db.exec("SELECT * FROM dbsp_sync('items')");

    // Create view chain
    db.exec("SELECT * FROM dbsp_create_view('v1', 'SELECT * FROM items')");
    db.exec("SELECT * FROM dbsp_create_view('v2', 'SELECT * FROM v1')");
    db.exec("SELECT * FROM dbsp_create_view('v3', 'SELECT * FROM v2')");

    // Check dependencies for v2
    auto result = db.query("SELECT * FROM dbsp_deps('v2')");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() >= 1);

    // Verify v2 depends on v1 and is depended on by v3
    bool found_v1 = false;
    bool found_v3 = false;
    for (size_t i = 0; i < result->RowCount(); i++) {
        auto name = result->GetValue(0, i).ToString();
        auto relationship = result->GetValue(1, i).ToString();

        if (name == "v1" && relationship == "depends_on") {
            found_v1 = true;
        }
        if (name == "v3" && relationship == "depended_by") {
            found_v3 = true;
        }
    }
    REQUIRE(found_v1);
    REQUIRE(found_v3);
}

TEST_CASE("dbsp_drop_cascade removes dependents", "[integration][cascade][drop]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("data", "id INT", {"(1)", "(2)"});
    db.exec("SELECT * FROM dbsp_track('data')");
    db.exec("SELECT * FROM dbsp_sync('data')");

    // Create cascading views
    db.exec("SELECT * FROM dbsp_create_view('level1', 'SELECT * FROM data')");
    db.exec("SELECT * FROM dbsp_create_view('level2', 'SELECT * FROM level1')");
    db.exec("SELECT * FROM dbsp_create_view('level3', 'SELECT * FROM level2')");
    db.exec("SELECT * FROM dbsp_create_view('level4', 'SELECT * FROM level3')");

    // Verify all views exist
    auto views = db.query("SELECT * FROM dbsp_views()");
    REQUIRE(views->RowCount() == 4);

    // Regular drop should fail because of dependents
    auto drop_result = db.query("SELECT dbsp_drop('level1')");
    REQUIRE(drop_result->GetValue(0, 0).ToString().find("Cannot drop") != std::string::npos);

    // Cascade drop should remove level1 and all dependents
    auto cascade_result = db.query("SELECT dbsp_drop_cascade('level1')");
    REQUIRE_FALSE(cascade_result->HasError());

    auto msg = cascade_result->GetValue(0, 0).ToString();
    REQUIRE(msg.find("Dropped") != std::string::npos);

    // All views should be gone
    views = db.query("SELECT * FROM dbsp_views()");
    REQUIRE(views->RowCount() == 0);
}

TEST_CASE("Cycle detection prevents infinite loops", "[integration][cascade][cycles]") {
    DuckDBTestHarness db;

    // Setup
    db.createTable("base", "id INT", {"(1)"});
    db.exec("SELECT * FROM dbsp_track('base')");
    db.exec("SELECT * FROM dbsp_sync('base')");

    // Create two views
    db.exec("SELECT * FROM dbsp_create_view('v1', 'SELECT * FROM base')");
    db.exec("SELECT * FROM dbsp_create_view('v2', 'SELECT * FROM v1')");

    // Attempting to make v1 depend on v2 should fail (would create cycle)
    // Note: This would require updating v1's definition, which current API doesn't support
    // Instead, test creating a new view that references both, which is valid
    auto result = db.query("SELECT * FROM dbsp_create_view('v3', "
                          "'SELECT * FROM v1 UNION SELECT * FROM v2')");
    REQUIRE_FALSE(result->HasError()); // This is valid - no cycle

    // Attempting direct self-reference should fail at SQL parsing
    // (a view referencing itself in its definition)
    // This is caught by DuckDB's SQL parser or by our dependency checker
    auto self_ref = db.query("SELECT * FROM dbsp_create_view('self', "
                            "'SELECT * FROM self')");
    // Should either error in SQL parsing or cycle detection
    // We check that it doesn't succeed
    if (!self_ref->HasError()) {
        auto msg = self_ref->GetValue(0, 0).ToString();
        REQUIRE(msg.find("Error") != std::string::npos);
    }
}

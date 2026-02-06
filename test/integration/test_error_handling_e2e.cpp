#include "catch.hpp"
#include "../test_helpers.hpp"
#include "../../dbsp_errors.hpp"

using namespace dbsp_test;
using namespace dbsp_native;

TEST_CASE("End-to-end error handling", "[e2e][errors]") {
    DuckDBTestHarness db;

    SECTION("Parser error flows through to user") {
        // Create table
        db.exec("CREATE TABLE orders (id INT, customer_id INT, amount INT)");
        db.exec("SELECT * FROM dbsp_track('orders')");

        // Try to create view with HAVING clause
        auto result = db.query(
            "SELECT * FROM dbsp_create_view('high_orders', "
            "'SELECT customer_id, SUM(amount) FROM orders GROUP BY customer_id HAVING SUM(amount) > 1000')");

        // Should fail with formatted error
        REQUIRE(result->HasError());
        std::string error = result->GetError();

        // Verify error contains expected elements
        REQUIRE(error.find("DBSP-E101") != std::string::npos);
        REQUIRE(error.find("HAVING") != std::string::npos);
        REQUIRE(error.find("Workaround") != std::string::npos);
        REQUIRE(error.find("nested view") != std::string::npos);
        REQUIRE(error.find("Documentation:") != std::string::npos);
    }

    SECTION("Validation error flows through to user") {
        // Try to create view with invalid name starting with number
        auto result = db.query(
            "SELECT * FROM dbsp_create_view('123_invalid', 'SELECT 1 as value')");

        REQUIRE(result->HasError());
        std::string error = result->GetError();

        // Verify error format
        REQUIRE(error.find("DBSP-E") != std::string::npos); // Some E2xx code
        REQUIRE(error.find("Identifier") != std::string::npos);
    }

    SECTION("Successful operations still work") {
        // Create valid table and view
        db.exec("CREATE TABLE products (id INT, price INT)");

        auto track_result = db.query("SELECT * FROM dbsp_track('products')");
        REQUIRE_FALSE(track_result->HasError());

        auto view_result = db.query(
            "SELECT * FROM dbsp_create_view('expensive', 'SELECT * FROM products WHERE price > 100')");
        REQUIRE_FALSE(view_result->HasError());

        // Query should work
        auto query_result = db.query("SELECT * FROM dbsp_query('expensive')");
        REQUIRE_FALSE(query_result->HasError());
    }
}

#include "../test_helpers.hpp"
#include "catch.hpp"
#include "dbsp_errors.hpp"

using namespace dbsp_test;
using namespace dbsp_native;

TEST_CASE("End-to-end error handling", "[e2e][errors]") {
  DuckDBTestHarness db;

  SECTION("Parser error flows through to user") {
    // Create table
    db.exec("CREATE TABLE orders (id INT, customer_id INT, amount INT)");
    db.exec("SELECT * FROM dbsp_track('orders')");

    // Try to create view with ORDER BY (unsupported feature)
    auto result = db.query("SELECT * FROM dbsp_create_view('ordered', "
                           "'SELECT customer_id, amount FROM orders "
                           "ORDER BY amount DESC')");

    // Should fail with formatted error
    REQUIRE(result->HasError());
    std::string error = result->GetError();

    // Verify error contains expected elements
    REQUIRE(error.find("DBSP-E102") != std::string::npos);
    REQUIRE(error.find("ORDER BY") != std::string::npos);
    REQUIRE(error.find("Workaround") != std::string::npos);
    REQUIRE(error.find("Documentation:") != std::string::npos);
  }

  SECTION("HAVING clause works after Phase 1 implementation") {
    // HAVING is now supported (P1.2)
    db.exec("CREATE TABLE orders2 (id INT, customer_id INT, amount INT)");
    db.exec("SELECT * FROM dbsp_track('orders2')");
    db.exec("SELECT * FROM dbsp_sync('orders2')");

    auto result = db.query("SELECT * FROM dbsp_create_view('high_orders', "
                           "'SELECT customer_id, SUM(amount) FROM orders2 GROUP "
                           "BY customer_id HAVING SUM(amount) > 1000')");
    REQUIRE_FALSE(result->HasError());
  }

  SECTION("Successful operations still work") {
    // Create valid table and view
    db.exec("CREATE TABLE products (id INT, price INT)");

    auto track_result = db.query("SELECT * FROM dbsp_track('products')");
    REQUIRE_FALSE(track_result->HasError());

    auto view_result = db.query("SELECT * FROM dbsp_create_view('expensive', "
                                "'SELECT * FROM products WHERE price > 100')");
    REQUIRE_FALSE(view_result->HasError());

    // Query should work
    auto query_result = db.query("SELECT * FROM dbsp_query('expensive')");
    REQUIRE_FALSE(query_result->HasError());
  }
}

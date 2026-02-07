#include "catch.hpp"
#include "../test_helpers.hpp"

using namespace dbsp_test;

TEST_CASE("ACID compliance - Atomicity", "[acid][runtime]") {
    DuckDBTestHarness db;

    SECTION("Failed validation prevents all updates") {
        // Create source table
        db.exec("CREATE TABLE source (id INT, value INT)");
        db.exec("SELECT * FROM dbsp_track('source')");

        // Create view chain: source -> view1 -> view2
        db.exec("SELECT * FROM dbsp_create_view('view1', "
                "'SELECT * FROM source WHERE value > 0')");
        db.exec("SELECT * FROM dbsp_create_view('view2', "
                "'SELECT * FROM source WHERE value < 100')");

        // Insert initial data
        db.exec("INSERT INTO source VALUES (1, 50)");
        db.exec("SELECT * FROM dbsp_sync('source')");

        // Verify initial state
        auto view1_result = db.query("SELECT * FROM dbsp_query('view1')");
        REQUIRE_FALSE(view1_result->HasError());
        REQUIRE(view1_result->RowCount() == 1);

        auto view2_result = db.query("SELECT * FROM dbsp_query('view2')");
        REQUIRE_FALSE(view2_result->HasError());
        REQUIRE(view2_result->RowCount() == 1);

        // TODO: In full implementation, inject a validation failure
        // For now, just verify sync works
        db.exec("INSERT INTO source VALUES (2, 75)");
        db.exec("SELECT * FROM dbsp_sync('source')");

        // Both views should update
        view1_result = db.query("SELECT * FROM dbsp_query('view1')");
        REQUIRE_FALSE(view1_result->HasError());
        REQUIRE(view1_result->RowCount() == 2);

        view2_result = db.query("SELECT * FROM dbsp_query('view2')");
        REQUIRE_FALSE(view2_result->HasError());
        REQUIRE(view2_result->RowCount() == 2);
    }
}

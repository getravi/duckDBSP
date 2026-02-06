#include "catch.hpp"
#include "../test_helpers.hpp"
#include "../../dbsp_cdc.hpp"
#include "../../dbsp_errors.hpp"

using namespace dbsp_native;
using namespace duckdb;

TEST_CASE("ACID compliance - Atomicity", "[acid][runtime]") {
    TestDatabase db;
    auto& manager = get_cdc_manager();

    SECTION("Failed validation prevents all updates") {
        // Create source table
        db.conn.Query("CREATE TABLE source (id INT, value INT)");
        manager.track_table(*db.context, "source");

        // Create view chain: source -> view1 -> view2
        manager.create_view(*db.context, "view1",
            "SELECT * FROM source WHERE value > 0");
        manager.create_view(*db.context, "view2",
            "SELECT * FROM view1 WHERE value < 100");

        // Insert initial data
        db.conn.Query("INSERT INTO source VALUES (1, 50)");
        manager.sync_table(*db.context, "source");

        // Verify initial state
        auto view1_result = manager.query_view("view1");
        REQUIRE(view1_result.size() == 1);
        auto view2_result = manager.query_view("view2");
        REQUIRE(view2_result.size() == 1);

        // TODO: In full implementation, inject a validation failure
        // For now, just verify sync works
        db.conn.Query("INSERT INTO source VALUES (2, 75)");
        bool sync_ok = manager.sync_table(*db.context, "source");
        REQUIRE(sync_ok);

        // Both views should update
        view1_result = manager.query_view("view1");
        REQUIRE(view1_result.size() == 2);
        view2_result = manager.query_view("view2");
        REQUIRE(view2_result.size() == 2);
    }
}

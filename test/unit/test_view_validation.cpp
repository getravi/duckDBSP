#include "catch.hpp"
#include "../../include/dbsp_materialized_view.hpp"
#include "../../dbsp_duckdb_types.hpp"

using namespace dbsp;

TEST_CASE("View validation interface", "[validation]") {
    SECTION("Default validation passes") {
        // Create a simple filter view
        auto schema = std::make_shared<TableSchema>();
        schema->columns = {{"id", TypeId::INT}, {"value", TypeId::INT}};

        FilterView view(schema, 0, "=", DuckDBValue(10));

        DuckDBZSet changes;
        changes.add(DuckDBRow({DuckDBValue(1), DuckDBValue(10)}), 1);

        auto result = view.validate_changes(changes);
        REQUIRE(result.success);
        REQUIRE(result.error_message.empty());
    }
}

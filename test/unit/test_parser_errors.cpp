#include "catch.hpp"
#include "../../dbsp_sql_parser.hpp"
#include "../../dbsp_errors.hpp"

using namespace dbsp_native;

TEST_CASE("Parser errors - HAVING clause", "[parser][errors][e101]") {
    DBSPSqlParser parser;

    SECTION("HAVING clause is detected and rejected") {
        auto result = parser.parse(
            "SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 10",
            "test_view");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::HAVING_NOT_SUPPORTED);
        REQUIRE(result.error.find("DBSP-E101") != std::string::npos);
        REQUIRE(result.error.find("HAVING") != std::string::npos);
        REQUIRE(result.error.find("nested view") != std::string::npos);
        REQUIRE(result.error.find("TODO #3") != std::string::npos);
    }

    SECTION("GROUP BY without HAVING works") {
        auto result = parser.parse(
            "SELECT dept, COUNT(*) FROM emp GROUP BY dept",
            "test_view");

        REQUIRE(result.success);
    }
}

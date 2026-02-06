#include "catch.hpp"
#include "../../dbsp_errors.hpp"

using namespace dbsp_native;

TEST_CASE("Error codes - Categories", "[errors]") {
    SECTION("Parser error codes are E1xx") {
        REQUIRE(static_cast<int>(ErrorCode::HAVING_NOT_SUPPORTED) == 101);
        REQUIRE(static_cast<int>(ErrorCode::ORDER_BY_NOT_SUPPORTED) == 102);
        REQUIRE(static_cast<int>(ErrorCode::LIMIT_NOT_SUPPORTED) == 103);
    }

    SECTION("Validation error codes are E2xx") {
        REQUIRE(static_cast<int>(ErrorCode::INVALID_IDENTIFIER) == 201);
        REQUIRE(static_cast<int>(ErrorCode::PATH_TRAVERSAL) == 202);
    }

    SECTION("Runtime error codes are E3xx") {
        REQUIRE(static_cast<int>(ErrorCode::VIEW_UPDATE_FAILED) == 301);
    }
}

TEST_CASE("ErrorInfo structure", "[errors]") {
    SECTION("Create error info") {
        ErrorInfo info;
        info.code = ErrorCode::HAVING_NOT_SUPPORTED;
        info.message = "HAVING clause detected";
        info.sql = "SELECT * FROM t GROUP BY x HAVING COUNT(*) > 10";
        info.error_position = 30;

        REQUIRE(info.code == ErrorCode::HAVING_NOT_SUPPORTED);
        REQUIRE(info.error_position == 30);
    }
}

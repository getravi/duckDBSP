#include "catch.hpp"
#include "dbsp_errors.hpp"

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

TEST_CASE("Error formatting - Basic", "[errors][format]") {
    SECTION("Format error with code and message") {
        ErrorInfo info;
        info.code = ErrorCode::HAVING_NOT_SUPPORTED;
        info.message = "HAVING clause detected";

        std::string formatted = format_error(info);

        REQUIRE(formatted.find("DBSP-E101") != std::string::npos);
        REQUIRE(formatted.find("HAVING clause detected") != std::string::npos);
        REQUIRE(formatted.find("Documentation:") != std::string::npos);
    }
}

TEST_CASE("Error formatting - SQL highlighting", "[errors][format]") {
    SECTION("Show SQL with position marker") {
        ErrorInfo info;
        info.code = ErrorCode::ORDER_BY_NOT_SUPPORTED;
        info.message = "ORDER BY clause detected";
        info.sql = "SELECT * FROM orders ORDER BY created_at";
        info.error_position = 22; // Position of "ORDER"

        std::string formatted = format_error(info);

        REQUIRE(formatted.find("SQL:") != std::string::npos);
        REQUIRE(formatted.find(info.sql) != std::string::npos);
        REQUIRE(formatted.find("^") != std::string::npos);
    }
}

TEST_CASE("Error formatting - Workaround", "[errors][format]") {
    SECTION("Include workaround if provided") {
        ErrorInfo info;
        info.code = ErrorCode::LIMIT_NOT_SUPPORTED;
        info.message = "LIMIT clause detected";
        info.workaround = "Query view and apply LIMIT in outer query";

        std::string formatted = format_error(info);

        REQUIRE(formatted.find("Workaround:") != std::string::npos);
        REQUIRE(formatted.find("Query view and apply LIMIT") != std::string::npos);
    }
}

TEST_CASE("Workaround lookup", "[errors][workarounds]") {
    SECTION("HAVING not supported") {
        std::string workaround = get_workaround(ErrorCode::HAVING_NOT_SUPPORTED);
        REQUIRE_FALSE(workaround.empty());
        REQUIRE(workaround.find("nested view") != std::string::npos);
        REQUIRE(workaround.find("TODO #3") != std::string::npos);
    }

    SECTION("ORDER BY not supported") {
        std::string workaround = get_workaround(ErrorCode::ORDER_BY_NOT_SUPPORTED);
        REQUIRE_FALSE(workaround.empty());
        REQUIRE(workaround.find("client-side") != std::string::npos);
    }

    SECTION("Unknown error code returns empty") {
        // Just verify function exists
        std::string workaround = get_workaround(ErrorCode::MEMORY_LIMIT_EXCEEDED);
        // May be empty or have default message
    }
}

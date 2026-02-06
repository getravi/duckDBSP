#include "catch.hpp"
#include "../../dbsp_cdc.hpp"
#include "../../dbsp_errors.hpp"

using namespace dbsp_native;

TEST_CASE("Validation errors - Invalid identifiers", "[validation][e201]") {
    SECTION("Empty name") {
        std::string error_msg;
        ErrorCode code;
        bool valid = validate_identifier("", error_msg, code);

        REQUIRE_FALSE(valid);
        REQUIRE(code == ErrorCode::INVALID_IDENTIFIER);
        REQUIRE_FALSE(error_msg.empty());
    }

    SECTION("Name too long") {
        std::string long_name(256, 'a');
        std::string error_msg;
        ErrorCode code;
        bool valid = validate_identifier(long_name, error_msg, code);

        REQUIRE_FALSE(valid);
        REQUIRE(code == ErrorCode::IDENTIFIER_TOO_LONG);
    }

    SECTION("Starts with number") {
        std::string error_msg;
        ErrorCode code;
        bool valid = validate_identifier("123_table", error_msg, code);

        REQUIRE_FALSE(valid);
        REQUIRE(code == ErrorCode::INVALID_IDENTIFIER);
    }

    SECTION("Contains special characters") {
        std::string error_msg;
        ErrorCode code;
        bool valid = validate_identifier("table-name", error_msg, code);

        REQUIRE_FALSE(valid);
        REQUIRE(code == ErrorCode::INVALID_IDENTIFIER);
    }

    SECTION("Valid names pass") {
        std::string error_msg;
        ErrorCode code;

        REQUIRE(validate_identifier("valid_name", error_msg, code));
        REQUIRE(validate_identifier("_starts_underscore", error_msg, code));
        REQUIRE(validate_identifier("has123numbers", error_msg, code));
    }
}

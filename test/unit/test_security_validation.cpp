#include <catch2/catch_test_macros.hpp>
#include "../../duckdb_extension/dbsp_cdc.hpp"

using namespace dbsp_native;

TEST_CASE("is_valid_identifier validates SQL identifiers correctly", "[security][validation]") {
    SECTION("Accept valid alphanumeric identifiers") {
        REQUIRE(is_valid_identifier("valid_name"));
        REQUIRE(is_valid_identifier("ValidName123"));
        REQUIRE(is_valid_identifier("_underscore"));
        REQUIRE(is_valid_identifier("name_with_underscores"));
        REQUIRE(is_valid_identifier("CamelCase"));
        REQUIRE(is_valid_identifier("snake_case_123"));
    }

    SECTION("Reject identifiers with special characters") {
        REQUIRE_FALSE(is_valid_identifier("bad-name"));  // Dash
        REQUIRE_FALSE(is_valid_identifier("bad.name"));  // Dot
        REQUIRE_FALSE(is_valid_identifier("bad;name"));  // Semicolon
        REQUIRE_FALSE(is_valid_identifier("bad'name"));  // Single quote
        REQUIRE_FALSE(is_valid_identifier("bad\"name")); // Double quote
        REQUIRE_FALSE(is_valid_identifier("bad name"));  // Space
        REQUIRE_FALSE(is_valid_identifier("bad\nname")); // Newline
        REQUIRE_FALSE(is_valid_identifier("bad\tname")); // Tab
    }

    SECTION("Reject identifiers starting with numbers") {
        REQUIRE_FALSE(is_valid_identifier("123bad"));
        REQUIRE_FALSE(is_valid_identifier("0name"));
    }

    SECTION("Reject SQL injection attempts") {
        REQUIRE_FALSE(is_valid_identifier("'; DROP TABLE users; --"));
        REQUIRE_FALSE(is_valid_identifier("valid'; DELETE FROM data; --"));
        REQUIRE_FALSE(is_valid_identifier("name OR 1=1"));
    }

    SECTION("Reject empty identifiers") {
        REQUIRE_FALSE(is_valid_identifier(""));
    }

    SECTION("Reject very long identifiers") {
        std::string too_long(256, 'a');
        REQUIRE_FALSE(is_valid_identifier(too_long));
    }

    SECTION("Accept 255 character identifiers") {
        std::string max_length(255, 'a');
        REQUIRE(is_valid_identifier(max_length));
    }
}

TEST_CASE("validate_filepath prevents path traversal attacks", "[security][validation]") {
    SECTION("Accept valid relative paths") {
        REQUIRE(validate_filepath("file.json") == "file.json");
        REQUIRE(validate_filepath("backup.json") == "backup.json");
        REQUIRE(validate_filepath("data/backup.json") == "data/backup.json");
        REQUIRE(validate_filepath("a/b/c/file.json") == "a/b/c/file.json");
    }

    SECTION("Reject absolute paths") {
        REQUIRE(validate_filepath("/etc/passwd") == "");
        REQUIRE(validate_filepath("/tmp/file.json") == "");
        REQUIRE(validate_filepath("/Users/test/file.json") == "");
    }

    SECTION("Reject path traversal with ..") {
        REQUIRE(validate_filepath("../file.json") == "");
        REQUIRE(validate_filepath("../../etc/passwd") == "");
        REQUIRE(validate_filepath("data/../../../etc/passwd") == "");
        REQUIRE(validate_filepath("..") == "");
    }

    SECTION("Reject home directory expansion") {
        REQUIRE(validate_filepath("~/file.json") == "");
        REQUIRE(validate_filepath("~user/file.json") == "");
    }

    SECTION("Reject null bytes") {
        std::string with_null = "file\0.json";
        REQUIRE(validate_filepath(with_null) == "");
    }

    SECTION("Reject Windows absolute paths") {
        // These should be rejected on all platforms for consistency
        REQUIRE(validate_filepath("C:\\file.json") == "");
        REQUIRE(validate_filepath("D:\\Windows\\System32\\file.txt") == "");
    }

    SECTION("Reject paths with backslashes") {
        REQUIRE(validate_filepath("path\\to\\file.json") == "");
    }

    SECTION("Accept paths without extension") {
        REQUIRE(validate_filepath("backup") == "backup");
        REQUIRE(validate_filepath("data/backup") == "data/backup");
    }
}

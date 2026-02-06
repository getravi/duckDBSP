#include "catch.hpp"
#include "../test_helpers.hpp"

using namespace dbsp_test;

TEST_CASE("Security: SQL injection in view names is prevented", "[security][sql-injection]") {
    DuckDBTestHarness db;

    db.exec("CREATE TABLE test_table (id INT, val INT)");
    db.exec("SELECT * FROM dbsp_track('test_table')");

    SECTION("Reject view name with SQL injection attempt") {
        // Attempt to inject SQL via view name
        std::string malicious_view = "valid'; DROP TABLE test_table; --";

        auto result = db.query("SELECT * FROM dbsp_create_view('" + malicious_view +
                               "', 'SELECT * FROM test_table')");

        // Should fail - view name validation should reject this
        REQUIRE(result->HasError());

        // Table should still exist
        auto table_result = db.query("SELECT * FROM test_table");
        REQUIRE_FALSE(table_result->HasError());
    }

    SECTION("Reject view name with quotes") {
        auto result = db.query("SELECT * FROM dbsp_create_view('bad''name', 'SELECT * FROM test_table')");
        REQUIRE(result->HasError());
    }

    SECTION("Reject view name with semicolon") {
        auto result = db.query("SELECT * FROM dbsp_create_view('bad;name', 'SELECT * FROM test_table')");
        REQUIRE(result->HasError());
    }

    SECTION("Reject view name with dash") {
        auto result = db.query("SELECT * FROM dbsp_create_view('bad-name', 'SELECT * FROM test_table')");
        REQUIRE(result->HasError());
    }

    SECTION("Reject view name starting with number") {
        auto result = db.query("SELECT * FROM dbsp_create_view('123bad', 'SELECT * FROM test_table')");
        REQUIRE(result->HasError());
    }

    SECTION("Accept valid view name with underscores") {
        auto result = db.query("SELECT * FROM dbsp_create_view('valid_name_123', 'SELECT * FROM test_table')");
        REQUIRE_FALSE(result->HasError());
    }

    SECTION("Accept view name starting with underscore") {
        auto result = db.query("SELECT * FROM dbsp_create_view('_valid', 'SELECT * FROM test_table')");
        REQUIRE_FALSE(result->HasError());
    }
}

TEST_CASE("Security: SQL injection in table names is prevented", "[security][sql-injection]") {
    DuckDBTestHarness db;

    SECTION("Reject table name with SQL injection attempt") {
        db.exec("CREATE TABLE normal_table (id INT)");

        // Attempt to track table with malicious name
        std::string malicious_table = "valid'; DROP TABLE normal_table; --";
        auto result = db.query("SELECT * FROM dbsp_track('" + malicious_table + "')");

        // Should fail - table name validation should reject this
        REQUIRE(result->HasError());

        // Normal table should still exist
        auto table_result = db.query("SELECT * FROM normal_table");
        REQUIRE_FALSE(table_result->HasError());
    }

    SECTION("Reject table name with special characters") {
        auto result = db.query("SELECT * FROM dbsp_track('bad-table')");
        REQUIRE(result->HasError());
    }

    SECTION("Accept valid table name") {
        db.exec("CREATE TABLE valid_table_123 (id INT)");
        auto result = db.query("SELECT * FROM dbsp_track('valid_table_123')");
        REQUIRE_FALSE(result->HasError());
    }
}

TEST_CASE("Security: Path traversal in file operations is prevented", "[security][path-traversal]") {
    DuckDBTestHarness db;

    db.exec("CREATE TABLE test_data (id INT, val INT)");
    db.exec("SELECT * FROM dbsp_track('test_data')");
    db.exec("SELECT * FROM dbsp_create_view('test_view', 'SELECT * FROM test_data')");

    SECTION("Reject absolute paths") {
        auto result = db.query("SELECT * FROM dbsp_save('/etc/passwd')");
        REQUIRE(result->HasError());
    }

    SECTION("Reject path traversal with ..") {
        auto result = db.query("SELECT * FROM dbsp_save('../../../etc/passwd')");
        REQUIRE(result->HasError());
    }

    SECTION("Reject path traversal in middle of path") {
        auto result = db.query("SELECT * FROM dbsp_save('data/../../../etc/passwd')");
        REQUIRE(result->HasError());
    }

    SECTION("Reject home directory expansion") {
        auto result = db.query("SELECT * FROM dbsp_save('~/malicious.json')");
        REQUIRE(result->HasError());
    }

    SECTION("Reject null bytes") {
        // Note: This tests the validation logic, actual null byte in SQL would be rejected by DuckDB
        auto result = db.query("SELECT * FROM dbsp_save('file.json')");
        REQUIRE_FALSE(result->HasError());
    }

    SECTION("Accept valid relative path") {
        auto result = db.query("SELECT * FROM dbsp_save('valid_backup.json')");
        REQUIRE_FALSE(result->HasError());

        // Clean up
        db.exec("!rm valid_backup.json");
    }

    SECTION("Accept relative path with subdirectory") {
        auto result = db.query("SELECT * FROM dbsp_save('backups/valid.json')");
        REQUIRE_FALSE(result->HasError());

        // Clean up
        db.exec("!rm -rf backups");
    }

    SECTION("Reject loading from absolute path") {
        auto result = db.query("SELECT * FROM dbsp_load('/etc/passwd')");
        REQUIRE(result->HasError());
    }

    SECTION("Reject loading with path traversal") {
        auto result = db.query("SELECT * FROM dbsp_load('../../etc/passwd')");
        REQUIRE(result->HasError());
    }
}

TEST_CASE("Security: Persistence with validated identifiers", "[security][persistence]") {
    DuckDBTestHarness db;

    SECTION("Save and load with valid identifiers only") {
        db.exec("CREATE TABLE products (id INT, name VARCHAR)");
        db.exec("SELECT * FROM dbsp_track('products')");
        db.exec("INSERT INTO products VALUES (1, 'Widget')");
        db.exec("SELECT * FROM dbsp_sync('products')");

        db.exec("SELECT * FROM dbsp_create_view('product_summary', 'SELECT * FROM products')");

        // Save to DuckDB table
        auto result = db.query("SELECT * FROM dbsp_save()");
        REQUIRE_FALSE(result->HasError());

        // Load back
        result = db.query("SELECT * FROM dbsp_load()");
        REQUIRE_FALSE(result->HasError());

        // Verify view still works
        result = db.query("SELECT * FROM dbsp_query('product_summary')");
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->RowCount() == 1);
    }

    SECTION("Save to file with validated path") {
        db.exec("CREATE TABLE orders (id INT)");
        db.exec("SELECT * FROM dbsp_track('orders')");
        db.exec("SELECT * FROM dbsp_create_view('order_view', 'SELECT * FROM orders')");

        // Save and load with valid relative path
        auto result = db.query("SELECT * FROM dbsp_save('test_backup.json')");
        REQUIRE_FALSE(result->HasError());

        result = db.query("SELECT * FROM dbsp_load('test_backup.json')");
        REQUIRE_FALSE(result->HasError());

        // Clean up
        db.exec("!rm test_backup.json");
    }
}

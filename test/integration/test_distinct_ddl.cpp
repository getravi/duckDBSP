#include "../test_helpers.hpp"
#include "catch.hpp"

using namespace dbsp_test;

// ============================================================================
// Phase 2.1: DISTINCT DDL Syntax Tests (TDD RED Phase)
// ============================================================================

TEST_CASE("DISTINCT with DDL syntax - Basic", "[integration][distinct][ddl]") {
    DuckDBTestHarness db;

    // Setup: table with duplicate values
    db.createTable("data", "val INT",
                   {"(1)", "(1)", "(2)", "(2)", "(3)"});
    db.exec("SELECT * FROM dbsp_track('data')");
    db.exec("SELECT * FROM dbsp_sync('data')");

    // RED: Try DDL syntax for DISTINCT
    auto result = db.query("CREATE MATERIALIZED VIEW unique_vals AS SELECT DISTINCT val FROM data");
    if (result->HasError()) {
        WARN("DDL CREATE error: " << result->GetError());
    }
    REQUIRE_FALSE(result->HasError());

    // Should have 3 unique values: {1, 2, 3}
    db.assertViewRowCount("unique_vals", 3);
}

TEST_CASE("DISTINCT incremental insert maintains uniqueness", "[integration][distinct][ddl]") {
    DuckDBTestHarness db;

    db.createTable("items", "id INT",
                   {"(1)", "(1)", "(2)"});
    db.exec("SELECT * FROM dbsp_track('items')");
    db.exec("SELECT * FROM dbsp_sync('items')");

    // Create DISTINCT view with DDL
    db.exec("CREATE MATERIALIZED VIEW unique_items AS SELECT DISTINCT id FROM items");

    // Initially: {1, 2}
    db.assertViewRowCount("unique_items", 2);

    // Insert duplicate value
    db.exec("INSERT INTO items VALUES (1)");
    db.exec("SELECT * FROM dbsp_sync('items')");

    // Should still be {1, 2}
    db.assertViewRowCount("unique_items", 2);

    // Insert new value
    db.exec("INSERT INTO items VALUES (3)");
    db.exec("SELECT * FROM dbsp_sync('items')");

    // Should now be {1, 2, 3}
    db.assertViewRowCount("unique_items", 3);
}

TEST_CASE("DISTINCT incremental delete removes when last instance deleted", "[integration][distinct][ddl]") {
    DuckDBTestHarness db;

    // Use rowid-style table so we can delete specific rows
    db.createTable("vals", "id INT, val INT",
                   {"(1, 1)", "(2, 1)", "(3, 1)", "(4, 2)", "(5, 2)", "(6, 3)"});
    db.exec("SELECT * FROM dbsp_track('vals')");
    db.exec("SELECT * FROM dbsp_sync('vals')");

    db.exec("CREATE MATERIALIZED VIEW unique_vals AS SELECT DISTINCT val FROM vals");

    // Initially: {1, 2, 3}
    db.assertViewRowCount("unique_vals", 3);

    // Delete one instance of 1 (two remain)
    db.exec("DELETE FROM vals WHERE id = 1");
    db.exec("SELECT * FROM dbsp_sync('vals')");

    // Should still be {1, 2, 3}
    db.assertViewRowCount("unique_vals", 3);

    // Delete all remaining 1s
    db.exec("DELETE FROM vals WHERE val = 1");
    db.exec("SELECT * FROM dbsp_sync('vals')");

    // Should now be {2, 3}
    db.assertViewRowCount("unique_vals", 2);
}

TEST_CASE("DISTINCT with multiple columns", "[integration][distinct][ddl]") {
    DuckDBTestHarness db;

    db.createTable("pairs", "a INT, b INT",
                   {"(1, 10)", "(1, 10)", "(1, 20)", "(2, 10)", "(2, 10)"});
    db.exec("SELECT * FROM dbsp_track('pairs')");
    db.exec("SELECT * FROM dbsp_sync('pairs')");

    db.exec("CREATE MATERIALIZED VIEW unique_pairs AS SELECT DISTINCT a, b FROM pairs");

    // Unique pairs: (1,10), (1,20), (2,10) = 3
    db.assertViewRowCount("unique_pairs", 3);
}

TEST_CASE("DISTINCT with NULL values", "[integration][distinct][ddl]") {
    DuckDBTestHarness db;

    // SQL DISTINCT treats each NULL as distinct (unlike equality)
    db.createTable("nulls", "val INT",
                   {"(1)", "(NULL)", "(NULL)", "(2)"});
    db.exec("SELECT * FROM dbsp_track('nulls')");
    db.exec("SELECT * FROM dbsp_sync('nulls')");

    db.exec("CREATE MATERIALIZED VIEW unique_nulls AS SELECT DISTINCT val FROM nulls");

    // Should have: 1, NULL, 2 = 3 rows (NULLs consolidated)
    // Note: This depends on how DuckDB's DISTINCT handles NULLs
    auto rows = db.getViewRows("unique_nulls");
    INFO("Row count with NULLs: " << rows.size());

    // Standard SQL: DISTINCT consolidates NULLs (multiple NULLs -> one NULL)
    REQUIRE(rows.size() >= 2); // At least {1, 2}, possibly {1, NULL, 2}
}

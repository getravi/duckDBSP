#include "catch.hpp"
#include <catch2/benchmark/catch_benchmark.hpp>
#include "../test_helpers.hpp"
#include <chrono>

using namespace dbsp_test;
using namespace std::chrono;

TEST_CASE("Benchmark: O(delta) vs O(n) for aggregates", "[!benchmark]") {
    DuckDBTestHarness db;

    // Setup large table
    const int TABLE_SIZE = 10000;
    const int NUM_GROUPS = 100;

    db.exec("CREATE TABLE large_sales (id INT, category INT, amount DECIMAL)");
    db.exec("SELECT * FROM dbsp_track('large_sales')");

    // Insert bulk data
    for (int i = 0; i < TABLE_SIZE; i++) {
        db.exec("INSERT INTO large_sales VALUES (" +
                std::to_string(i) + ", " +
                std::to_string(i % NUM_GROUPS) + ", " +
                std::to_string((i % 100) * 10.0) + ")");
    }
    db.exec("SELECT * FROM dbsp_sync('large_sales')");

    // Create DBSP incremental view
    db.exec("SELECT * FROM dbsp_create_view('category_totals', "
            "'SELECT category, SUM(amount) FROM large_sales GROUP BY category')");

    BENCHMARK("DBSP incremental insert") {
        db.exec("INSERT INTO large_sales VALUES (99999, 50, 500.0)");
        db.exec("SELECT * FROM dbsp_sync('large_sales')");
        return db.query("SELECT * FROM dbsp_query('category_totals')");
    };

    BENCHMARK("Traditional view recompute") {
        db.exec("CREATE OR REPLACE VIEW traditional_view AS "
                "SELECT category, SUM(amount) FROM large_sales GROUP BY category");
        return db.query("SELECT * FROM traditional_view");
    };

    // Expected: DBSP should be significantly faster (O(1) vs O(n))
}

TEST_CASE("Benchmark: Incremental insert scaling", "[!benchmark]") {
    DuckDBTestHarness db;

    SECTION("1K rows") {
        db.exec("CREATE TABLE t1k (id INT, val INT)");
        db.exec("SELECT * FROM dbsp_track('t1k')");
        for (int i = 0; i < 1000; i++) {
            db.exec("INSERT INTO t1k VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ")");
        }
        db.exec("SELECT * FROM dbsp_sync('t1k')");
        db.exec("SELECT * FROM dbsp_create_view('sum1k', 'SELECT SUM(val) FROM t1k')");

        BENCHMARK("Insert into 1K table") {
            db.exec("INSERT INTO t1k VALUES (9999, 9999)");
            db.exec("SELECT * FROM dbsp_sync('t1k')");
        };
    }

    SECTION("10K rows") {
        db.exec("CREATE TABLE t10k (id INT, val INT)");
        db.exec("SELECT * FROM dbsp_track('t10k')");
        for (int i = 0; i < 10000; i++) {
            db.exec("INSERT INTO t10k VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ")");
        }
        db.exec("SELECT * FROM dbsp_sync('t10k')");
        db.exec("SELECT * FROM dbsp_create_view('sum10k', 'SELECT SUM(val) FROM t10k')");

        BENCHMARK("Insert into 10K table") {
            db.exec("INSERT INTO t10k VALUES (99999, 99999)");
            db.exec("SELECT * FROM dbsp_sync('t10k')");
        };
    }

    SECTION("100K rows") {
        db.exec("CREATE TABLE t100k (id INT, val INT)");
        db.exec("SELECT * FROM dbsp_track('t100k')");
        for (int i = 0; i < 100000; i++) {
            db.exec("INSERT INTO t100k VALUES (" + std::to_string(i) + ", " + std::to_string(i * 10) + ")");
        }
        db.exec("SELECT * FROM dbsp_sync('t100k')");
        db.exec("SELECT * FROM dbsp_create_view('sum100k', 'SELECT SUM(val) FROM t100k')");

        BENCHMARK("Insert into 100K table") {
            db.exec("INSERT INTO t100k VALUES (999999, 999999)");
            db.exec("SELECT * FROM dbsp_sync('t100k')");
        };
    }

    // Expected: All three should have similar times (O(1) behavior)
}

TEST_CASE("Benchmark: Batch insert performance", "[!benchmark]") {
    DuckDBTestHarness db;

    db.exec("CREATE TABLE batch_test (id INT, category INT, val INT)");
    db.exec("SELECT * FROM dbsp_track('batch_test')");

    // Populate base data
    for (int i = 0; i < 5000; i++) {
        db.exec("INSERT INTO batch_test VALUES (" +
                std::to_string(i) + ", " +
                std::to_string(i % 10) + ", " +
                std::to_string(i) + ")");
    }
    db.exec("SELECT * FROM dbsp_sync('batch_test')");

    db.exec("SELECT * FROM dbsp_create_view('batch_agg', "
            "'SELECT category, SUM(val) FROM batch_test GROUP BY category')");

    BENCHMARK("Batch insert 10 rows") {
        for (int i = 0; i < 10; i++) {
            db.exec("INSERT INTO batch_test VALUES (" + std::to_string(10000 + i) + ", 5, 100)");
        }
        db.exec("SELECT * FROM dbsp_sync('batch_test')");
    };

    BENCHMARK("Batch insert 100 rows") {
        for (int i = 0; i < 100; i++) {
            db.exec("INSERT INTO batch_test VALUES (" + std::to_string(20000 + i) + ", 5, 100)");
        }
        db.exec("SELECT * FROM dbsp_sync('batch_test')");
    };

    // Expected: Time should scale with batch size (O(delta))
}

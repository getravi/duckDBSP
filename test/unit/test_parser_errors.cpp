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

TEST_CASE("Parser errors - ORDER BY clause", "[parser][errors][e102]") {
    DBSPSqlParser parser;

    SECTION("ORDER BY is detected and rejected") {
        auto result = parser.parse(
            "SELECT * FROM orders ORDER BY created_at DESC",
            "sorted_orders");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::ORDER_BY_NOT_SUPPORTED);
        REQUIRE(result.error.find("DBSP-E102") != std::string::npos);
        REQUIRE(result.error.find("ORDER BY") != std::string::npos);
        REQUIRE(result.error.find("client-side") != std::string::npos);
    }

    SECTION("Multiple ORDER BY columns") {
        auto result = parser.parse(
            "SELECT * FROM orders ORDER BY customer_id, created_at DESC",
            "sorted_orders");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::ORDER_BY_NOT_SUPPORTED);
    }
}

TEST_CASE("Parser errors - LIMIT clause", "[parser][errors][e103]") {
    DBSPSqlParser parser;

    SECTION("LIMIT is detected and rejected") {
        auto result = parser.parse(
            "SELECT * FROM orders LIMIT 100",
            "limited_orders");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::LIMIT_NOT_SUPPORTED);
        REQUIRE(result.error.find("DBSP-E103") != std::string::npos);
        REQUIRE(result.error.find("LIMIT") != std::string::npos);
    }

    SECTION("LIMIT with OFFSET") {
        auto result = parser.parse(
            "SELECT * FROM orders LIMIT 100 OFFSET 50",
            "limited_orders");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::LIMIT_NOT_SUPPORTED);
    }
}

TEST_CASE("Parser errors - Window functions", "[parser][errors][e104]") {
    DBSPSqlParser parser;

    SECTION("ROW_NUMBER window function") {
        auto result = parser.parse(
            "SELECT *, ROW_NUMBER() OVER (PARTITION BY dept ORDER BY salary) FROM emp",
            "ranked_emp");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::WINDOW_FUNCTIONS_NOT_SUPPORTED);
        REQUIRE(result.error.find("DBSP-E104") != std::string::npos);
        REQUIRE(result.error.find("Window") != std::string::npos);
    }

    SECTION("RANK window function") {
        auto result = parser.parse(
            "SELECT *, RANK() OVER (ORDER BY salary DESC) FROM emp",
            "ranked_emp");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::WINDOW_FUNCTIONS_NOT_SUPPORTED);
    }
}

TEST_CASE("Parser errors - Subqueries", "[parser][errors][e105]") {
    DBSPSqlParser parser;

    SECTION("Subquery in FROM clause") {
        auto result = parser.parse(
            "SELECT * FROM (SELECT id FROM orders WHERE amount > 100) AS high_orders",
            "derived_table");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::SUBQUERY_NOT_SUPPORTED);
        REQUIRE(result.error.find("DBSP-E105") != std::string::npos);
        REQUIRE(result.error.find("Subquery") != std::string::npos);
    }

    SECTION("Subquery in WHERE with IN") {
        auto result = parser.parse(
            "SELECT * FROM orders WHERE customer_id IN (SELECT id FROM customers WHERE active = true)",
            "filtered_orders");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::SUBQUERY_NOT_SUPPORTED);
    }

    SECTION("Subquery in WHERE with EXISTS") {
        auto result = parser.parse(
            "SELECT * FROM orders WHERE EXISTS (SELECT 1 FROM customers WHERE id = orders.customer_id)",
            "filtered_orders");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::SUBQUERY_NOT_SUPPORTED);
    }
}

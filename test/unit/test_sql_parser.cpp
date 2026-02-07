#include "catch.hpp"
#include "../../dbsp_sql_parser.hpp"
#include "../../dbsp_errors.hpp"

using namespace dbsp_native;

TEST_CASE("SQL Parser - Simple SELECT", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("SELECT * FROM table") {
        auto result = parser.parse("SELECT * FROM users", "test_view");

        REQUIRE(result.success);
        REQUIRE(result.view_def.view_name == "test_view");
        REQUIRE(result.view_def.source_tables.size() == 1);
        REQUIRE(result.view_def.source_tables[0] == "users");
        REQUIRE(result.view_def.select_all);
    }
}

TEST_CASE("SQL Parser - SELECT with columns", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("SELECT specific columns") {
        auto result = parser.parse("SELECT id, name, age FROM users", "user_view");

        REQUIRE(result.success);
        REQUIRE(result.view_def.project_column_names.size() == 3);
        REQUIRE(result.view_def.project_column_names[0] == "id");
        REQUIRE(result.view_def.project_column_names[1] == "name");
        REQUIRE(result.view_def.project_column_names[2] == "age");
        REQUIRE_FALSE(result.view_def.select_all);
    }
}

TEST_CASE("SQL Parser - WHERE clause", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("Simple equality filter") {
        auto result = parser.parse("SELECT * FROM users WHERE age = 25", "filtered_users");

        REQUIRE(result.success);
        REQUIRE(result.view_def.filters.size() == 1);
        REQUIRE(result.view_def.filters[0].column_name == "age");
        REQUIRE(result.view_def.filters[0].op == "=");
        REQUIRE(result.view_def.type == ParsedViewDef::ViewType::FILTER);
    }

    SECTION("Greater than filter") {
        auto result = parser.parse("SELECT * FROM users WHERE age > 18", "adult_users");

        REQUIRE(result.success);
        REQUIRE(result.view_def.filters.size() == 1);
        REQUIRE(result.view_def.filters[0].column_name == "age");
        REQUIRE(result.view_def.filters[0].op == ">");
    }

    SECTION("Less than filter") {
        auto result = parser.parse("SELECT * FROM products WHERE price < 100", "cheap_products");

        REQUIRE(result.success);
        REQUIRE(result.view_def.filters.size() == 1);
        REQUIRE(result.view_def.filters[0].column_name == "price");
        REQUIRE(result.view_def.filters[0].op == "<");
    }
}

TEST_CASE("SQL Parser - GROUP BY", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("Simple aggregate with GROUP BY") {
        auto result = parser.parse("SELECT city, COUNT(id) FROM users GROUP BY city", "users_by_city");

        REQUIRE(result.success);
        REQUIRE(result.view_def.aggregates.size() == 1);
        REQUIRE(result.view_def.aggregates[0].function == "COUNT");
        REQUIRE(result.view_def.aggregates[0].value_column_name == "id");
        REQUIRE(result.view_def.type == ParsedViewDef::ViewType::AGGREGATE);
    }

    SECTION("SUM aggregate") {
        auto result = parser.parse("SELECT category, SUM(price) FROM products GROUP BY category", "sum_by_category");

        REQUIRE(result.success);
        REQUIRE(result.view_def.aggregates.size() == 1);
        REQUIRE(result.view_def.aggregates[0].function == "SUM");
        REQUIRE(result.view_def.aggregates[0].value_column_name == "price");
    }

    SECTION("AVG aggregate") {
        auto result = parser.parse("SELECT department, AVG(salary) FROM employees GROUP BY department", "avg_salary");

        REQUIRE(result.success);
        REQUIRE(result.view_def.aggregates.size() == 1);
        REQUIRE(result.view_def.aggregates[0].function == "AVG");
        REQUIRE(result.view_def.aggregates[0].value_column_name == "salary");
    }
}

TEST_CASE("SQL Parser - Multiple aggregates", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("Multiple aggregates in one query") {
        auto result = parser.parse(
            "SELECT city, COUNT(id), SUM(amount) FROM orders GROUP BY city",
            "city_stats"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.aggregates.size() == 2);
        REQUIRE(result.view_def.aggregates[0].function == "COUNT");
        REQUIRE(result.view_def.aggregates[0].value_column_name == "id");
        REQUIRE(result.view_def.aggregates[1].function == "SUM");
        REQUIRE(result.view_def.aggregates[1].value_column_name == "amount");
    }
}

TEST_CASE("SQL Parser - JOIN", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("Simple INNER JOIN") {
        auto result = parser.parse(
            "SELECT * FROM orders JOIN customers ON orders.customer_id = customers.id",
            "order_details"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.source_tables.size() == 2);
        REQUIRE(result.view_def.source_tables[0] == "orders");
        REQUIRE(result.view_def.source_tables[1] == "customers");
        REQUIRE(result.view_def.join_info.has_value());
        REQUIRE(result.view_def.join_info->left_table == "orders");
        REQUIRE(result.view_def.join_info->right_table == "customers");
        REQUIRE(result.view_def.join_info->left_column == "customer_id");
        REQUIRE(result.view_def.join_info->right_column == "id");
        REQUIRE(result.view_def.type == ParsedViewDef::ViewType::JOIN);
    }
}

TEST_CASE("SQL Parser - DISTINCT", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("SELECT DISTINCT") {
        auto result = parser.parse("SELECT DISTINCT city FROM users", "distinct_cities");

        REQUIRE(result.success);
        REQUIRE(result.view_def.is_distinct);
        REQUIRE(result.view_def.type == ParsedViewDef::ViewType::DISTINCT);
    }

    SECTION("SELECT DISTINCT with multiple columns") {
        auto result = parser.parse("SELECT DISTINCT city, state FROM users", "distinct_locations");

        REQUIRE(result.success);
        REQUIRE(result.view_def.is_distinct);
        REQUIRE(result.view_def.project_column_names.size() == 2);
        REQUIRE(result.view_def.project_column_names[0] == "city");
        REQUIRE(result.view_def.project_column_names[1] == "state");
    }
}

TEST_CASE("SQL Parser - Complex WHERE with AND", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("Multiple filters with AND") {
        auto result = parser.parse(
            "SELECT * FROM products WHERE price > 50 AND category = 'electronics'",
            "filtered_products"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.filters.size() == 2);
        REQUIRE(result.view_def.filters[0].column_name == "price");
        REQUIRE(result.view_def.filters[0].op == ">");
        REQUIRE(result.view_def.filters[1].column_name == "category");
        REQUIRE(result.view_def.filters[1].op == "=");
    }
}

TEST_CASE("SQL Parser - View referencing another view", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("View on top of view") {
        // First create base view (from table)
        auto base_result = parser.parse("SELECT * FROM users WHERE age > 18", "adult_users");
        REQUIRE(base_result.success);

        // Now create view on top of that view
        auto derived_result = parser.parse("SELECT name, city FROM adult_users", "adult_user_names");
        REQUIRE(derived_result.success);
        REQUIRE(derived_result.view_def.source_tables[0] == "adult_users");
        REQUIRE(derived_result.view_def.project_column_names.size() == 2);
    }
}

TEST_CASE("SQL Parser - Invalid SQL", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("Syntax error") {
        auto result = parser.parse("SELECT * FORM users", "bad_view");

        REQUIRE_FALSE(result.success);
        REQUIRE_FALSE(result.error.empty());
    }

    SECTION("Incomplete statement") {
        auto result = parser.parse("SELECT * FROM", "incomplete_view");

        REQUIRE_FALSE(result.success);
        REQUIRE_FALSE(result.error.empty());
    }
}

TEST_CASE("SQL Parser - Empty SELECT", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("Empty string") {
        auto result = parser.parse("", "empty_view");

        REQUIRE_FALSE(result.success);
        REQUIRE_FALSE(result.error.empty());
    }
}

TEST_CASE("SQL Parser - Missing FROM clause", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("SELECT without FROM") {
        auto result = parser.parse("SELECT 1, 2, 3", "no_from_view");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error == "FROM clause required");
    }
}

TEST_CASE("SQL Parser - View type determination", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("JOIN has highest priority") {
        auto result = parser.parse(
            "SELECT DISTINCT * FROM orders JOIN customers ON orders.customer_id = customers.id WHERE amount > 100",
            "complex_view"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.type == ParsedViewDef::ViewType::JOIN);
    }

    SECTION("AGGREGATE has priority over DISTINCT") {
        auto result = parser.parse(
            "SELECT DISTINCT city, COUNT(id) FROM users GROUP BY city",
            "distinct_agg"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.type == ParsedViewDef::ViewType::AGGREGATE);
    }

    SECTION("DISTINCT has priority over FILTER") {
        auto result = parser.parse(
            "SELECT DISTINCT city FROM users WHERE age > 18",
            "distinct_filter"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.type == ParsedViewDef::ViewType::DISTINCT);
    }

    SECTION("FILTER has priority over PROJECT") {
        auto result = parser.parse(
            "SELECT id, name FROM users WHERE age > 18",
            "filter_project"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.type == ParsedViewDef::ViewType::FILTER);
    }

    SECTION("PROJECT type") {
        auto result = parser.parse(
            "SELECT id, name, email FROM users",
            "project_view"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.type == ParsedViewDef::ViewType::PROJECT);
    }
}

TEST_CASE("SQL Parser - MIN and MAX aggregates", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("MIN aggregate") {
        auto result = parser.parse(
            "SELECT category, MIN(price) FROM products GROUP BY category",
            "min_price"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.aggregates.size() == 1);
        REQUIRE(result.view_def.aggregates[0].function == "MIN");
        REQUIRE(result.view_def.aggregates[0].value_column_name == "price");
    }

    SECTION("MAX aggregate") {
        auto result = parser.parse(
            "SELECT category, MAX(price) FROM products GROUP BY category",
            "max_price"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.aggregates.size() == 1);
        REQUIRE(result.view_def.aggregates[0].function == "MAX");
        REQUIRE(result.view_def.aggregates[0].value_column_name == "price");
    }
}

TEST_CASE("SQL Parser - Comparison operators", "[sql_parser]") {
    DBSPSqlParser parser;

    SECTION("Greater than or equal") {
        auto result = parser.parse("SELECT * FROM users WHERE age >= 18", "adults");

        REQUIRE(result.success);
        REQUIRE(result.view_def.filters.size() == 1);
        REQUIRE(result.view_def.filters[0].op == ">=");
    }

    SECTION("Less than or equal") {
        auto result = parser.parse("SELECT * FROM products WHERE price <= 100", "affordable");

        REQUIRE(result.success);
        REQUIRE(result.view_def.filters.size() == 1);
        REQUIRE(result.view_def.filters[0].op == "<=");
    }

    SECTION("Not equal") {
        auto result = parser.parse("SELECT * FROM users WHERE status != 'inactive'", "active_users");

        REQUIRE(result.success);
        REQUIRE(result.view_def.filters.size() == 1);
        REQUIRE(result.view_def.filters[0].op == "!=");
    }
}

TEST_CASE("SQL Parser - HAVING clause", "[sql_parser][having]") {
    DBSPSqlParser parser;

    SECTION("Simple HAVING with COUNT") {
        auto result = parser.parse(
            "SELECT dept, COUNT(id) FROM employees GROUP BY dept HAVING COUNT(id) > 10",
            "large_depts"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.type == ParsedViewDef::ViewType::AGGREGATE);
        REQUIRE(result.view_def.having_filters.size() == 1);
        REQUIRE(result.view_def.having_filters[0].ref_type == ParsedViewDef::HavingFilter::AGGREGATE_RESULT);
        REQUIRE(result.view_def.having_filters[0].agg_function == "COUNT");
        REQUIRE(result.view_def.having_filters[0].op == ">");
    }

    SECTION("HAVING with SUM") {
        auto result = parser.parse(
            "SELECT category, SUM(price) FROM products GROUP BY category HAVING SUM(price) > 1000",
            "expensive_categories"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.having_filters.size() == 1);
        REQUIRE(result.view_def.having_filters[0].agg_function == "SUM");
        REQUIRE(result.view_def.having_filters[0].op == ">");
    }

    SECTION("HAVING with group column reference") {
        auto result = parser.parse(
            "SELECT dept, COUNT(id) FROM employees GROUP BY dept HAVING dept = 'Sales'",
            "sales_dept"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.having_filters.size() == 1);
        REQUIRE(result.view_def.having_filters[0].ref_type == ParsedViewDef::HavingFilter::GROUP_COL);
        REQUIRE(result.view_def.having_filters[0].column_name == "dept");
        REQUIRE(result.view_def.having_filters[0].op == "=");
    }

    SECTION("HAVING with multiple conditions (AND)") {
        auto result = parser.parse(
            "SELECT customer_id, AVG(amount) FROM orders GROUP BY customer_id "
            "HAVING AVG(amount) >= 100 AND COUNT(amount) > 5",
            "good_customers"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.having_filters.size() == 2);
        REQUIRE(result.view_def.having_filters[0].agg_function == "AVG");
        REQUIRE(result.view_def.having_filters[0].op == ">=");
        REQUIRE(result.view_def.having_filters[1].agg_function == "COUNT");
        REQUIRE(result.view_def.having_filters[1].op == ">");
    }

    SECTION("HAVING with COUNT(*)") {
        auto result = parser.parse(
            "SELECT dept, COUNT(*) FROM employees GROUP BY dept HAVING COUNT(*) > 5",
            "big_depts"
        );

        REQUIRE(result.success);
        REQUIRE(result.view_def.having_filters.size() == 1);
        REQUIRE(result.view_def.having_filters[0].agg_function == "COUNT");
        REQUIRE(result.view_def.having_filters[0].op == ">");
    }
}

TEST_CASE("Parser errors - Error codes", "[sql_parser][errors]") {
    DBSPSqlParser parser;

    SECTION("ParseResult includes error code") {
        auto result = parser.parse("INVALID SQL");

        REQUIRE_FALSE(result.success);
        REQUIRE_FALSE(result.error.empty());
        // Error code should be set (will add specific checks later)
    }
}

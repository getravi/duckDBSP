// Test utilities for DBSP tests
#pragma once

#include <catch2/catch_test_macros.hpp>
#include "duckdb.hpp"
#include "../duckdb_extension/dbsp_duckdb_types.hpp"

namespace dbsp_test {

using namespace duckdb;
using namespace dbsp_native;

// Helper to create DuckDBRow from values
inline DuckDBRow makeRow(std::initializer_list<Value> values) {
    DuckDBRow row;
    for (const auto& val : values) {
        row.columns.push_back(val);
    }
    return row;
}

// Helper to create ZSet from row-weight pairs
inline DuckDBZSet makeZSet(
    std::initializer_list<std::pair<DuckDBRow, int64_t>> data) {
    DuckDBZSet zset;
    for (const auto& [row, weight] : data) {
        zset.insert(row, weight);
    }
    return zset;
}

// Custom assertion for Z-sets
inline void assertZSetEquals(const DuckDBZSet& actual,
                             const DuckDBZSet& expected) {
    REQUIRE(actual.support_size() == expected.support_size());
    for (const auto& [row, weight] : expected) {
        INFO("Checking row weight");
        REQUIRE(actual[row] == weight);
    }
}

// DuckDB test harness for integration tests
class DuckDBTestHarness {
private:
    DuckDB db_;
    Connection conn_;

public:
    DuckDBTestHarness() : db_(nullptr), conn_(db_) {
        // Load extension - update path as needed
        const char* ext_path = "duckdb_extension/build/dbsp.duckdb_extension";
        try {
            conn_.Query("LOAD '" + std::string(ext_path) + "'");
        } catch (const std::exception& e) {
            // Extension not built yet - tests will skip
        }
    }

    Connection& conn() { return conn_; }

    // Execute query and return result
    unique_ptr<MaterializedQueryResult> query(const std::string& sql) {
        return conn_.Query(sql);
    }

    // Execute query and verify success
    void exec(const std::string& sql) {
        auto result = query(sql);
        REQUIRE_FALSE(result->HasError());
    }

    // Create test table with data
    void createTable(const std::string& name,
                    const std::string& schema,
                    const std::vector<std::string>& rows) {
        exec("CREATE TABLE " + name + " (" + schema + ")");
        for (const auto& row : rows) {
            exec("INSERT INTO " + name + " VALUES " + row);
        }
    }

    // Assert view has expected row count
    void assertViewRowCount(const std::string& view_name, size_t expected) {
        auto result = query("SELECT COUNT(*) FROM dbsp_query('" + view_name + "')");
        REQUIRE_FALSE(result->HasError());
        auto count = result->GetValue(0, 0).GetValue<int64_t>();
        REQUIRE(count == expected);
    }

    // Get all rows from view as vector
    std::vector<std::vector<Value>> getViewRows(const std::string& view_name) {
        auto result = query("SELECT * FROM dbsp_query('" + view_name + "')");
        REQUIRE_FALSE(result->HasError());

        std::vector<std::vector<Value>> rows;
        for (size_t i = 0; i < result->RowCount(); i++) {
            std::vector<Value> row;
            for (size_t j = 0; j < result->ColumnCount(); j++) {
                row.push_back(result->GetValue(j, i));
            }
            rows.push_back(row);
        }
        return rows;
    }
};

} // namespace dbsp_test

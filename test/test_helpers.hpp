// Test utilities for DBSP tests
#pragma once

#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "../dbsp_duckdb_types.hpp"
#include "../dbsp_cdc.hpp"

// Forward declaration for extension entry point (extern "C" matches DUCKDB_CPP_EXTENSION_ENTRY)
extern "C" DUCKDB_EXTENSION_API void dbsp_duckdb_cpp_init(duckdb::ExtensionLoader &loader);

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
    REQUIRE(actual.size() == expected.size());
    for (const auto& [row, weight] : expected) {
        INFO("Checking row weight");
        REQUIRE(actual.get(row) == weight);
    }
}

// DuckDB test harness for integration tests
class DuckDBTestHarness {
private:
    DuckDB db_;
    Connection conn_;

public:
    DuckDBTestHarness() : db_(nullptr), conn_(db_) {
        // Reset global CDC manager state between tests
        dbsp_native::get_cdc_manager().reset();

        // Register extension functions directly (compiled into test binary)
        try {
            duckdb::ExtensionLoader loader(*db_.instance, "dbsp");
            dbsp_duckdb_cpp_init(loader);
        } catch (const std::exception& e) {
            // Registration failed - tests will fail with descriptive errors
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
        if (result->HasError()) {
            INFO("Query error: " << result->GetError());
        }
        REQUIRE_FALSE(result->HasError());
        auto count = result->GetValue(0, 0).GetValue<int64_t>();
        REQUIRE(count == expected);
    }

    // Get all rows from view as vector
    std::vector<std::vector<Value>> getViewRows(const std::string& view_name) {
        auto result = query("SELECT * FROM dbsp_query('" + view_name + "')");
        if (result->HasError()) {
            INFO("Query error: " << result->GetError());
        }
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

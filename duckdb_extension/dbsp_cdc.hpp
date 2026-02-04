// DBSP Change Data Capture (CDC) Manager
// Automatically tracks changes to DuckDB tables and propagates to views

#pragma once

#include "dbsp_duckdb_types.hpp"
#include "dbsp_sql_parser.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dbsp_native {

// CDC Manager - coordinates tracked tables, views, and change propagation
class CDCManager {
public:
    CDCManager() = default;

    // Track a DuckDB table for CDC
    // Creates a shadow tracking mechanism for the table
    bool track_table(duckdb::ClientContext& context, const std::string& table_name) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (tracked_tables_.count(table_name)) {
            return true;  // Already tracked
        }

        // Get table schema from DuckDB catalog
        TableSchema schema;
        if (!get_table_schema(context, table_name, schema)) {
            return false;
        }

        // Create tracked table
        tracked_tables_[table_name] = std::make_unique<TrackedTable>(table_name, schema);
        table_schemas_[table_name] = schema;

        // Initialize with current table contents
        sync_table(context, table_name);

        return true;
    }

    // Untrack a table
    void untrack_table(const std::string& table_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracked_tables_.erase(table_name);
        table_schemas_.erase(table_name);
    }

    // Create a materialized view from SQL
    bool create_view(duckdb::ClientContext& context, const std::string& view_name,
                     const std::string& sql) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Parse SQL
        DBSPSqlParser parser;
        auto result = parser.parse(sql, view_name);

        if (!result.success) {
            last_error_ = result.error;
            return false;
        }

        // Ensure all source tables are tracked
        for (const auto& table : result.view_def.source_tables) {
            if (!tracked_tables_.count(table)) {
                // Auto-track the table
                if (!track_table_internal(context, table)) {
                    last_error_ = "Could not track source table: " + table;
                    return false;
                }
            }
        }

        // Create the view
        auto view = ViewFactory::create_view(result.view_def, table_schemas_);
        if (!view) {
            last_error_ = "Could not create view from SQL";
            return false;
        }

        // Initialize view with current data from source tables
        for (const auto& table : result.view_def.source_tables) {
            auto it = tracked_tables_.find(table);
            if (it != tracked_tables_.end()) {
                view->apply_changes(table, it->second->current_state());
            }
        }

        views_[view_name] = std::move(view);
        return true;
    }

    // Drop a view
    bool drop_view(const std::string& view_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        return views_.erase(view_name) > 0;
    }

    // Record an INSERT
    void on_insert(const std::string& table_name, const DuckDBRow& row) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = tracked_tables_.find(table_name);
        if (it == tracked_tables_.end()) return;

        it->second->insert(row);
        propagate_changes(table_name);
    }

    // Record a DELETE
    void on_delete(const std::string& table_name, const DuckDBRow& row) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = tracked_tables_.find(table_name);
        if (it == tracked_tables_.end()) return;

        it->second->remove(row);
        propagate_changes(table_name);
    }

    // Record an UPDATE
    void on_update(const std::string& table_name, const DuckDBRow& old_row,
                   const DuckDBRow& new_row) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = tracked_tables_.find(table_name);
        if (it == tracked_tables_.end()) return;

        it->second->update(old_row, new_row);
        propagate_changes(table_name);
    }

    // Batch insert (for efficiency)
    void on_batch_insert(const std::string& table_name, const std::vector<DuckDBRow>& rows) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = tracked_tables_.find(table_name);
        if (it == tracked_tables_.end()) return;

        for (const auto& row : rows) {
            it->second->insert(row);
        }
        propagate_changes(table_name);
    }

    // Sync tracked table with actual DuckDB table
    // Call this to detect and propagate changes made outside CDC
    bool sync_table(duckdb::ClientContext& context, const std::string& table_name) {
        auto it = tracked_tables_.find(table_name);
        if (it == tracked_tables_.end()) return false;

        // Query all data from the table
        std::string query = "SELECT * FROM " + table_name;

        try {
            auto result = context.Query(query, false);
            if (result->HasError()) {
                return false;
            }

            // Build new state
            DuckDBZSet new_state;
            for (auto& row : *result) {
                DuckDBRow dbsp_row;
                for (idx_t i = 0; i < row.ColumnCount(); i++) {
                    dbsp_row.columns.push_back(row.GetValue(i));
                }
                new_state.insert(dbsp_row, 1);
            }

            // Compute delta: new_state - old_state
            DuckDBZSet delta;
            const auto& old_state = it->second->current_state();

            // Add new rows
            for (const auto& [row, weight] : new_state) {
                int64_t old_weight = old_state.get(row);
                int64_t diff = weight - old_weight;
                if (diff != 0) {
                    delta.insert(row, diff);
                }
            }

            // Remove deleted rows
            for (const auto& [row, weight] : old_state) {
                if (new_state.get(row) == 0) {
                    delta.insert(row, -weight);
                }
            }

            // Apply delta to tracked table and views
            for (const auto& [row, weight] : delta) {
                if (weight > 0) {
                    for (int i = 0; i < weight; i++) {
                        it->second->insert(row);
                    }
                } else {
                    for (int i = 0; i < -weight; i++) {
                        it->second->remove(row);
                    }
                }
            }

            if (!delta.empty()) {
                propagate_changes(table_name);
            }

            return true;

        } catch (...) {
            return false;
        }
    }

    // Sync all tracked tables
    void sync_all(duckdb::ClientContext& context) {
        std::vector<std::string> tables;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [name, _] : tracked_tables_) {
                tables.push_back(name);
            }
        }

        for (const auto& table : tables) {
            sync_table(context, table);
        }
    }

    // Query a view
    const DuckDBZSet* query_view(const std::string& view_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = views_.find(view_name);
        if (it == views_.end()) return nullptr;
        return &it->second->get_result();
    }

    // Get view schema
    const TableSchema* get_view_schema(const std::string& view_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = views_.find(view_name);
        if (it == views_.end()) return nullptr;
        return &it->second->result_schema();
    }

    // List views
    std::vector<std::string> list_views() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        for (const auto& [name, _] : views_) {
            names.push_back(name);
        }
        return names;
    }

    // List tracked tables
    std::vector<std::string> list_tracked_tables() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        for (const auto& [name, _] : tracked_tables_) {
            names.push_back(name);
        }
        return names;
    }

    // Get table schema
    const TableSchema* get_table_schema(const std::string& table_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = table_schemas_.find(table_name);
        return it != table_schemas_.end() ? &it->second : nullptr;
    }

    // Get view info
    struct ViewInfo {
        std::string name;
        std::string sql;
        size_t row_count;
        uint64_t version;
        std::vector<std::string> source_tables;
    };

    ViewInfo get_view_info(const std::string& view_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        ViewInfo info;
        auto it = views_.find(view_name);
        if (it != views_.end()) {
            info.name = view_name;
            info.sql = it->second->sql();
            info.row_count = it->second->get_result().size();
            info.version = it->second->version();
            info.source_tables = it->second->source_tables();
        }
        return info;
    }

    // Get last error
    const std::string& last_error() const { return last_error_; }

private:
    // Internal track without lock
    bool track_table_internal(duckdb::ClientContext& context, const std::string& table_name) {
        if (tracked_tables_.count(table_name)) {
            return true;
        }

        TableSchema schema;
        if (!get_table_schema(context, table_name, schema)) {
            return false;
        }

        tracked_tables_[table_name] = std::make_unique<TrackedTable>(table_name, schema);
        table_schemas_[table_name] = schema;

        // Load current data
        sync_table_internal(context, table_name);

        return true;
    }

    // Sync without propagation
    bool sync_table_internal(duckdb::ClientContext& context, const std::string& table_name) {
        auto it = tracked_tables_.find(table_name);
        if (it == tracked_tables_.end()) return false;

        std::string query = "SELECT * FROM " + table_name;

        try {
            auto result = context.Query(query, false);
            if (result->HasError()) {
                return false;
            }

            for (auto& row : *result) {
                DuckDBRow dbsp_row;
                for (idx_t i = 0; i < row.ColumnCount(); i++) {
                    dbsp_row.columns.push_back(row.GetValue(i));
                }
                it->second->insert(dbsp_row);
            }
            return true;

        } catch (...) {
            return false;
        }
    }

    // Get schema from DuckDB catalog
    bool get_table_schema(duckdb::ClientContext& context, const std::string& table_name,
                          TableSchema& schema) {
        try {
            // Use PRAGMA to get column info
            std::string query = "PRAGMA table_info('" + table_name + "')";
            auto result = context.Query(query, false);

            if (result->HasError()) {
                return false;
            }

            schema.table_name = table_name;
            schema.columns.clear();

            for (auto& row : *result) {
                ColumnInfo col;
                col.name = row.GetValue(1).ToString();  // Column name is index 1
                std::string type_str = row.GetValue(2).ToString();  // Type is index 2

                // Map type string to LogicalType
                col.type = parse_type(type_str);
                schema.columns.push_back(col);
            }

            return !schema.columns.empty();

        } catch (...) {
            return false;
        }
    }

    duckdb::LogicalType parse_type(const std::string& type_str) {
        std::string upper = duckdb::StringUtil::Upper(type_str);

        if (upper == "INTEGER" || upper == "INT" || upper == "INT4") {
            return duckdb::LogicalType::INTEGER;
        }
        if (upper == "BIGINT" || upper == "INT8") {
            return duckdb::LogicalType::BIGINT;
        }
        if (upper == "SMALLINT" || upper == "INT2") {
            return duckdb::LogicalType::SMALLINT;
        }
        if (upper == "TINYINT" || upper == "INT1") {
            return duckdb::LogicalType::TINYINT;
        }
        if (upper == "DOUBLE" || upper == "FLOAT8" || upper == "REAL") {
            return duckdb::LogicalType::DOUBLE;
        }
        if (upper == "FLOAT" || upper == "FLOAT4") {
            return duckdb::LogicalType::FLOAT;
        }
        if (upper == "BOOLEAN" || upper == "BOOL") {
            return duckdb::LogicalType::BOOLEAN;
        }
        if (upper.find("VARCHAR") != std::string::npos || upper == "TEXT" || upper == "STRING") {
            return duckdb::LogicalType::VARCHAR;
        }
        if (upper == "DATE") {
            return duckdb::LogicalType::DATE;
        }
        if (upper == "TIMESTAMP" || upper == "DATETIME") {
            return duckdb::LogicalType::TIMESTAMP;
        }

        return duckdb::LogicalType::VARCHAR;  // Default
    }

    // Propagate changes from a table to all dependent views
    void propagate_changes(const std::string& table_name) {
        auto it = tracked_tables_.find(table_name);
        if (it == tracked_tables_.end()) return;

        DuckDBZSet changes = it->second->consume_changes();
        if (changes.empty()) return;

        // Apply to all views that depend on this table
        for (auto& [view_name, view] : views_) {
            auto sources = view->source_tables();
            for (const auto& src : sources) {
                if (src == table_name) {
                    view->apply_changes(table_name, changes);
                    break;
                }
            }
        }
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<TrackedTable>> tracked_tables_;
    std::unordered_map<std::string, TableSchema> table_schemas_;
    std::unordered_map<std::string, std::unique_ptr<NativeMaterializedView>> views_;
    std::string last_error_;
};

// Global CDC manager instance
inline CDCManager& get_cdc_manager() {
    static CDCManager manager;
    return manager;
}

} // namespace dbsp_native

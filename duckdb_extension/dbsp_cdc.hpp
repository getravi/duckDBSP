// DBSP Change Data Capture (CDC) Manager
// Automatically tracks changes to DuckDB tables and propagates to views
// Supports cascading views (views on views) and persistence

#pragma once

#include "dbsp_duckdb_types.hpp"
#include "dbsp_sql_parser.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dbsp_native {

// Security validation functions

// Validate SQL identifier (table/view name) - only alphanumeric and underscore
inline bool is_valid_identifier(const std::string& name) {
    if (name.empty() || name.length() > 255) {
        return false;
    }
    // First character must be letter or underscore
    if (!std::isalpha(name[0]) && name[0] != '_') {
        return false;
    }
    // Remaining characters must be alphanumeric or underscore
    for (char c : name) {
        if (!std::isalnum(c) && c != '_') {
            return false;
        }
    }
    return true;
}

// Validate and canonicalize file path to prevent path traversal
// Returns canonicalized path if valid, empty string if invalid
inline std::string validate_filepath(const std::string& filepath) {
    // Check for null bytes
    if (filepath.find('\0') != std::string::npos) {
        return "";
    }

    // Check for path traversal patterns
    if (filepath.find("..") != std::string::npos) {
        return "";
    }

    // Must not be absolute path starting with /
    if (!filepath.empty() && filepath[0] == '/') {
        return "";
    }

    // Must not start with ~ (home directory)
    if (!filepath.empty() && filepath[0] == '~') {
        return "";
    }

    // On Windows, check for drive letters and backslashes
    #ifdef _WIN32
    if (filepath.length() >= 2 && filepath[1] == ':') {
        return "";  // Reject C:\ style paths
    }
    if (filepath.find('\\') != std::string::npos) {
        return "";  // Reject backslashes
    }
    #endif

    // Valid relative path
    return filepath;
}

// View definition for persistence
struct ViewDefinition {
    std::string name;
    std::string sql;
    std::vector<std::string> source_tables;  // Can include other views
    uint64_t created_at;
};

// Dependency graph for cascading views
class DependencyGraph {
public:
    // Add a dependency: dependent depends on dependency
    void add_dependency(const std::string& dependent, const std::string& dependency) {
        dependencies_[dependent].insert(dependency);
        dependents_[dependency].insert(dependent);
    }

    // Remove all dependencies for a node
    void remove_node(const std::string& node) {
        // Remove from dependents of its dependencies
        if (dependencies_.count(node)) {
            for (const auto& dep : dependencies_[node]) {
                dependents_[dep].erase(node);
            }
            dependencies_.erase(node);
        }

        // Remove from dependencies of its dependents
        if (dependents_.count(node)) {
            for (const auto& dep : dependents_[node]) {
                dependencies_[dep].erase(node);
            }
            dependents_.erase(node);
        }
    }

    // Get all nodes that depend on the given node (direct and transitive)
    std::vector<std::string> get_all_dependents(const std::string& node) const {
        std::vector<std::string> result;
        std::unordered_set<std::string> visited;
        std::queue<std::string> queue;

        auto it = dependents_.find(node);
        if (it != dependents_.end()) {
            for (const auto& dep : it->second) {
                queue.push(dep);
            }
        }

        while (!queue.empty()) {
            std::string current = queue.front();
            queue.pop();

            if (visited.count(current)) continue;
            visited.insert(current);
            result.push_back(current);

            auto dep_it = dependents_.find(current);
            if (dep_it != dependents_.end()) {
                for (const auto& dep : dep_it->second) {
                    if (!visited.count(dep)) {
                        queue.push(dep);
                    }
                }
            }
        }

        return result;
    }

    // Get topological order for updating dependents of a node
    // Returns nodes in order they should be updated
    std::vector<std::string> topological_order(const std::string& changed_node) const {
        std::vector<std::string> dependents = get_all_dependents(changed_node);
        if (dependents.empty()) return {};

        // Build in-degree map for affected nodes
        std::unordered_map<std::string, int> in_degree;
        std::unordered_set<std::string> affected(dependents.begin(), dependents.end());

        for (const auto& node : dependents) {
            in_degree[node] = 0;
        }

        for (const auto& node : dependents) {
            auto it = dependencies_.find(node);
            if (it != dependencies_.end()) {
                for (const auto& dep : it->second) {
                    if (affected.count(dep) || dep == changed_node) {
                        in_degree[node]++;
                    }
                }
            }
        }

        // Kahn's algorithm
        std::vector<std::string> result;
        std::queue<std::string> queue;

        for (const auto& [node, degree] : in_degree) {
            if (degree == 0) {
                queue.push(node);
            }
        }

        while (!queue.empty()) {
            std::string current = queue.front();
            queue.pop();
            result.push_back(current);

            auto it = dependents_.find(current);
            if (it != dependents_.end()) {
                for (const auto& dep : it->second) {
                    if (in_degree.count(dep)) {
                        in_degree[dep]--;
                        if (in_degree[dep] == 0) {
                            queue.push(dep);
                        }
                    }
                }
            }
        }

        return result;
    }

    // Check if adding a dependency would create a cycle
    bool would_create_cycle(const std::string& dependent, const std::string& dependency) const {
        // Check if dependent is reachable from dependency
        std::unordered_set<std::string> visited;
        std::queue<std::string> queue;
        queue.push(dependent);

        while (!queue.empty()) {
            std::string current = queue.front();
            queue.pop();

            if (current == dependency) return true;
            if (visited.count(current)) continue;
            visited.insert(current);

            auto it = dependents_.find(current);
            if (it != dependents_.end()) {
                for (const auto& dep : it->second) {
                    queue.push(dep);
                }
            }
        }

        return false;
    }

    // Get direct dependencies of a node
    std::vector<std::string> get_dependencies(const std::string& node) const {
        std::vector<std::string> result;
        auto it = dependencies_.find(node);
        if (it != dependencies_.end()) {
            result.assign(it->second.begin(), it->second.end());
        }
        return result;
    }

private:
    std::unordered_map<std::string, std::unordered_set<std::string>> dependencies_;  // node -> what it depends on
    std::unordered_map<std::string, std::unordered_set<std::string>> dependents_;    // node -> what depends on it
};

// CDC Manager - coordinates tracked tables, views, and change propagation
class CDCManager {
public:
    CDCManager() = default;

    // Track a DuckDB table for CDC
    bool track_table(duckdb::ClientContext& context, const std::string& table_name) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Validate table name to prevent SQL injection
        if (!is_valid_identifier(table_name)) {
            last_error_ = "Invalid table name (must be alphanumeric/underscore only): " + table_name;
            return false;
        }

        if (tracked_tables_.count(table_name)) {
            return true;  // Already tracked
        }

        TableSchema schema;
        if (!get_table_schema(context, table_name, schema)) {
            return false;
        }

        tracked_tables_[table_name] = std::make_unique<TrackedTable>(table_name, schema);
        table_schemas_[table_name] = schema;

        // Initialize with current table contents
        sync_table_internal(context, table_name);

        return true;
    }

    // Untrack a table
    void untrack_table(const std::string& table_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracked_tables_.erase(table_name);
        table_schemas_.erase(table_name);
        dep_graph_.remove_node(table_name);
    }

    // Create a materialized view from SQL
    // Supports referencing other views (cascading views)
    bool create_view(duckdb::ClientContext& context, const std::string& view_name,
                     const std::string& sql) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Validate view name to prevent SQL injection
        if (!is_valid_identifier(view_name)) {
            last_error_ = "Invalid view name (must be alphanumeric/underscore only): " + view_name;
            return false;
        }

        // Check if view already exists
        if (views_.count(view_name)) {
            last_error_ = "View already exists: " + view_name;
            return false;
        }

        // Parse SQL
        DBSPSqlParser parser;
        auto result = parser.parse(sql, view_name);

        if (!result.success) {
            last_error_ = result.error;
            return false;
        }

        // Resolve sources - can be tables or other views
        std::vector<std::string> resolved_sources;
        for (const auto& source : result.view_def.source_tables) {
            // Check for cycles
            if (dep_graph_.would_create_cycle(view_name, source)) {
                last_error_ = "Circular dependency detected: " + view_name + " -> " + source;
                return false;
            }

            // Check if source is a view
            if (views_.count(source)) {
                // Source is another view - add to schema from view's result schema
                table_schemas_[source] = views_[source]->result_schema();
                resolved_sources.push_back(source);
            } else if (tracked_tables_.count(source)) {
                // Source is a tracked table
                resolved_sources.push_back(source);
            } else {
                // Try to auto-track as a table
                if (!track_table_internal(context, source)) {
                    last_error_ = "Could not find source: " + source;
                    return false;
                }
                resolved_sources.push_back(source);
            }
        }

        // Create the view
        auto view = ViewFactory::create_view(result.view_def, table_schemas_);
        if (!view) {
            last_error_ = "Could not create view from SQL";
            return false;
        }

        // Add dependencies
        for (const auto& source : resolved_sources) {
            dep_graph_.add_dependency(view_name, source);
        }

        // Initialize view with current data from sources
        for (const auto& source : resolved_sources) {
            if (tracked_tables_.count(source)) {
                view->apply_changes(source, tracked_tables_[source]->current_state());
            } else if (views_.count(source)) {
                // Source is a view - use its result
                view->apply_changes(source, views_[source]->get_result());
            }
        }

        // Store view definition for persistence
        ViewDefinition def;
        def.name = view_name;
        def.sql = sql;
        def.source_tables = resolved_sources;
        def.created_at = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        view_definitions_[view_name] = def;

        // Register the view's result schema for dependent views
        table_schemas_[view_name] = view->result_schema();

        views_[view_name] = std::move(view);
        return true;
    }

    // Drop a view
    bool drop_view(const std::string& view_name) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check if other views depend on this one
        auto dependents = dep_graph_.get_all_dependents(view_name);
        if (!dependents.empty()) {
            last_error_ = "Cannot drop view: other views depend on it: " + dependents[0];
            return false;
        }

        dep_graph_.remove_node(view_name);
        view_definitions_.erase(view_name);
        table_schemas_.erase(view_name);
        return views_.erase(view_name) > 0;
    }

    // Force drop a view and all dependents
    bool drop_view_cascade(const std::string& view_name) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto dependents = dep_graph_.get_all_dependents(view_name);

        // Drop in reverse topological order
        std::reverse(dependents.begin(), dependents.end());
        for (const auto& dep : dependents) {
            dep_graph_.remove_node(dep);
            view_definitions_.erase(dep);
            table_schemas_.erase(dep);
            views_.erase(dep);
        }

        dep_graph_.remove_node(view_name);
        view_definitions_.erase(view_name);
        table_schemas_.erase(view_name);
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

    // Batch insert
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
    bool sync_table(duckdb::ClientContext& context, const std::string& table_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        return sync_table_locked(context, table_name);
    }

    // Sync all tracked tables
    void sync_all(duckdb::ClientContext& context) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, _] : tracked_tables_) {
            sync_table_locked(context, name);
        }
    }

    // ========================================================================
    // Persistence
    // ========================================================================

    // Save all view definitions to a DuckDB table
    bool save_to_table(duckdb::ClientContext& context, const std::string& storage_table = "_dbsp_views") {
        std::lock_guard<std::mutex> lock(mutex_);

        try {
            // Create storage table if not exists
            std::string create_sql = "CREATE TABLE IF NOT EXISTS " + storage_table + " ("
                "name VARCHAR PRIMARY KEY, "
                "sql VARCHAR, "
                "sources VARCHAR, "
                "created_at BIGINT"
                ")";
            auto result = context.Query(create_sql, false);
            if (result->HasError()) {
                last_error_ = "Failed to create storage table";
                return false;
            }

            // Clear existing entries
            context.Query("DELETE FROM " + storage_table, false);

            // Insert view definitions
            for (const auto& [name, def] : view_definitions_) {
                // Validate view name
                if (!is_valid_identifier(name)) {
                    last_error_ = "Invalid view name: " + name;
                    return false;
                }

                // Join sources with comma
                std::string sources;
                for (size_t i = 0; i < def.source_tables.size(); i++) {
                    if (i > 0) sources += ",";
                    sources += def.source_tables[i];
                }

                // Escape SQL strings
                std::string escaped_name = escape_string(name);
                std::string escaped_sql = escape_string(def.sql);
                std::string escaped_sources = escape_string(sources);

                std::string insert_sql = "INSERT INTO " + storage_table +
                    " VALUES ('" + escaped_name + "', '" + escaped_sql + "', '" +
                    escaped_sources + "', " + std::to_string(def.created_at) + ")";

                result = context.Query(insert_sql, false);
                if (result->HasError()) {
                    last_error_ = "Failed to save view: " + name;
                    return false;
                }
            }

            // Also save tracked tables
            std::string tables_table = storage_table + "_tables";
            create_sql = "CREATE TABLE IF NOT EXISTS " + tables_table + " (name VARCHAR PRIMARY KEY)";
            context.Query(create_sql, false);
            context.Query("DELETE FROM " + tables_table, false);

            for (const auto& [name, _] : tracked_tables_) {
                // Validate table name
                if (!is_valid_identifier(name)) {
                    last_error_ = "Invalid table name: " + name;
                    return false;
                }
                std::string escaped_name = escape_string(name);
                context.Query("INSERT INTO " + tables_table + " VALUES ('" + escaped_name + "')", false);
            }

            return true;

        } catch (...) {
            last_error_ = "Exception during save";
            return false;
        }
    }

    // Load view definitions from a DuckDB table and recreate views
    bool load_from_table(duckdb::ClientContext& context, const std::string& storage_table = "_dbsp_views") {
        std::lock_guard<std::mutex> lock(mutex_);

        try {
            // First, load tracked tables
            std::string tables_table = storage_table + "_tables";
            auto tables_result = context.Query("SELECT name FROM " + tables_table, false);
            if (!tables_result->HasError()) {
                for (auto& row : *tables_result) {
                    std::string table_name = row.GetValue(0).ToString();
                    track_table_internal(context, table_name);
                }
            }

            // Load view definitions
            auto result = context.Query(
                "SELECT name, sql, sources, created_at FROM " + storage_table +
                " ORDER BY created_at", false);

            if (result->HasError()) {
                // Table doesn't exist - that's ok
                return true;
            }

            // Collect definitions
            std::vector<ViewDefinition> defs;
            for (auto& row : *result) {
                ViewDefinition def;
                def.name = row.GetValue(0).ToString();
                def.sql = row.GetValue(1).ToString();

                std::string sources = row.GetValue(2).ToString();
                if (!sources.empty()) {
                    std::stringstream ss(sources);
                    std::string source;
                    while (std::getline(ss, source, ',')) {
                        def.source_tables.push_back(source);
                    }
                }

                def.created_at = row.GetValue(3).GetValue<int64_t>();
                defs.push_back(def);
            }

            // Sort by creation time to handle dependencies
            std::sort(defs.begin(), defs.end(),
                [](const ViewDefinition& a, const ViewDefinition& b) {
                    return a.created_at < b.created_at;
                });

            // Recreate views (unlock for create_view which takes lock)
            mutex_.unlock();
            for (const auto& def : defs) {
                if (!create_view(context, def.name, def.sql)) {
                    mutex_.lock();
                    // Continue loading other views even if one fails
                }
            }
            mutex_.lock();

            return true;

        } catch (...) {
            last_error_ = "Exception during load";
            return false;
        }
    }

    // Save to JSON file
    bool save_to_file(const std::string& filepath) {
        std::lock_guard<std::mutex> lock(mutex_);

        try {
            // Validate filepath to prevent path traversal
            std::string validated_path = validate_filepath(filepath);
            if (validated_path.empty()) {
                last_error_ = "Invalid file path (must be relative, no path traversal): " + filepath;
                return false;
            }

            std::ofstream file(validated_path);
            if (!file.is_open()) {
                last_error_ = "Could not open file: " + validated_path;
                return false;
            }

            file << "{\n";

            // Save tracked tables
            file << "  \"tracked_tables\": [";
            bool first = true;
            for (const auto& [name, _] : tracked_tables_) {
                if (!first) file << ", ";
                file << "\"" << name << "\"";
                first = false;
            }
            file << "],\n";

            // Save views
            file << "  \"views\": [\n";
            first = true;
            for (const auto& [name, def] : view_definitions_) {
                if (!first) file << ",\n";
                file << "    {\n";
                file << "      \"name\": \"" << def.name << "\",\n";
                file << "      \"sql\": \"" << escape_json(def.sql) << "\",\n";
                file << "      \"sources\": [";
                for (size_t i = 0; i < def.source_tables.size(); i++) {
                    if (i > 0) file << ", ";
                    file << "\"" << def.source_tables[i] << "\"";
                }
                file << "],\n";
                file << "      \"created_at\": " << def.created_at << "\n";
                file << "    }";
                first = false;
            }
            file << "\n  ]\n";

            file << "}\n";
            file.close();

            return true;

        } catch (...) {
            last_error_ = "Exception during file save";
            return false;
        }
    }

    // Load from JSON file
    bool load_from_file(duckdb::ClientContext& context, const std::string& filepath) {
        std::lock_guard<std::mutex> lock(mutex_);

        try {
            // Validate filepath to prevent path traversal
            std::string validated_path = validate_filepath(filepath);
            if (validated_path.empty()) {
                last_error_ = "Invalid file path (must be relative, no path traversal): " + filepath;
                return false;
            }

            std::ifstream file(validated_path);
            if (!file.is_open()) {
                last_error_ = "Could not open file: " + validated_path;
                return false;
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            file.close();

            // Simple JSON parsing for our specific format
            // Parse tracked tables
            size_t tables_start = content.find("\"tracked_tables\"");
            if (tables_start != std::string::npos) {
                size_t arr_start = content.find('[', tables_start);
                size_t arr_end = content.find(']', arr_start);
                std::string tables_arr = content.substr(arr_start + 1, arr_end - arr_start - 1);

                size_t pos = 0;
                while ((pos = tables_arr.find('"', pos)) != std::string::npos) {
                    size_t end = tables_arr.find('"', pos + 1);
                    if (end != std::string::npos) {
                        std::string table = tables_arr.substr(pos + 1, end - pos - 1);
                        track_table_internal(context, table);
                        pos = end + 1;
                    } else {
                        break;
                    }
                }
            }

            // Parse views - extract each view object
            std::vector<ViewDefinition> defs;
            size_t views_start = content.find("\"views\"");
            if (views_start != std::string::npos) {
                size_t arr_start = content.find('[', views_start);
                size_t search_pos = arr_start;

                while (true) {
                    size_t obj_start = content.find('{', search_pos);
                    if (obj_start == std::string::npos) break;

                    size_t obj_end = content.find('}', obj_start);
                    if (obj_end == std::string::npos) break;

                    std::string obj = content.substr(obj_start, obj_end - obj_start + 1);

                    ViewDefinition def;

                    // Extract name
                    size_t name_pos = obj.find("\"name\"");
                    if (name_pos != std::string::npos) {
                        size_t val_start = obj.find('"', name_pos + 6);
                        size_t val_end = obj.find('"', val_start + 1);
                        def.name = obj.substr(val_start + 1, val_end - val_start - 1);
                    }

                    // Extract sql
                    size_t sql_pos = obj.find("\"sql\"");
                    if (sql_pos != std::string::npos) {
                        size_t val_start = obj.find('"', sql_pos + 5);
                        size_t val_end = find_string_end(obj, val_start + 1);
                        def.sql = unescape_json(obj.substr(val_start + 1, val_end - val_start - 1));
                    }

                    // Extract created_at
                    size_t time_pos = obj.find("\"created_at\"");
                    if (time_pos != std::string::npos) {
                        size_t val_start = obj.find(':', time_pos) + 1;
                        while (obj[val_start] == ' ') val_start++;
                        size_t val_end = val_start;
                        while (val_end < obj.size() && (isdigit(obj[val_end]))) val_end++;
                        def.created_at = std::stoull(obj.substr(val_start, val_end - val_start));
                    }

                    if (!def.name.empty() && !def.sql.empty()) {
                        defs.push_back(def);
                    }

                    search_pos = obj_end + 1;
                }
            }

            // Sort by creation time
            std::sort(defs.begin(), defs.end(),
                [](const ViewDefinition& a, const ViewDefinition& b) {
                    return a.created_at < b.created_at;
                });

            // Recreate views
            mutex_.unlock();
            for (const auto& def : defs) {
                create_view(context, def.name, def.sql);
            }
            mutex_.lock();

            return true;

        } catch (...) {
            last_error_ = "Exception during file load";
            return false;
        }
    }

    // ========================================================================
    // Query methods
    // ========================================================================

    const DuckDBZSet* query_view(const std::string& view_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = views_.find(view_name);
        if (it == views_.end()) return nullptr;
        return &it->second->get_result();
    }

    const TableSchema* get_view_schema(const std::string& view_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = views_.find(view_name);
        if (it == views_.end()) return nullptr;
        return &it->second->result_schema();
    }

    std::vector<std::string> list_views() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        for (const auto& [name, _] : views_) {
            names.push_back(name);
        }
        return names;
    }

    std::vector<std::string> list_tracked_tables() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        for (const auto& [name, _] : tracked_tables_) {
            names.push_back(name);
        }
        return names;
    }

    const TableSchema* get_table_schema(const std::string& table_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = table_schemas_.find(table_name);
        return it != table_schemas_.end() ? &it->second : nullptr;
    }

    struct ViewInfo {
        std::string name;
        std::string sql;
        size_t row_count;
        uint64_t version;
        std::vector<std::string> source_tables;
        std::vector<std::string> dependent_views;
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
            info.dependent_views = dep_graph_.get_all_dependents(view_name);
        }
        return info;
    }

    // Get view dependencies
    std::vector<std::string> get_view_dependencies(const std::string& view_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        return dep_graph_.get_dependencies(view_name);
    }

    // Get views that depend on this view/table
    std::vector<std::string> get_dependents(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        return dep_graph_.get_all_dependents(name);
    }

    const std::string& last_error() const { return last_error_; }

private:
    // Sync table with lock already held
    bool sync_table_locked(duckdb::ClientContext& context, const std::string& table_name) {
        auto it = tracked_tables_.find(table_name);
        if (it == tracked_tables_.end()) return false;

        std::string query = "SELECT * FROM " + table_name;

        try {
            auto result = context.Query(query, false);
            if (result->HasError()) {
                return false;
            }

            DuckDBZSet new_state;
            for (auto& row : *result) {
                DuckDBRow dbsp_row;
                for (idx_t i = 0; i < row.ColumnCount(); i++) {
                    dbsp_row.columns.push_back(row.GetValue(i));
                }
                new_state.insert(dbsp_row, 1);
            }

            DuckDBZSet delta;
            const auto& old_state = it->second->current_state();

            for (const auto& [row, weight] : new_state) {
                int64_t old_weight = old_state.get(row);
                int64_t diff = weight - old_weight;
                if (diff != 0) {
                    delta.insert(row, diff);
                }
            }

            for (const auto& [row, weight] : old_state) {
                if (new_state.get(row) == 0) {
                    delta.insert(row, -weight);
                }
            }

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

        sync_table_internal(context, table_name);
        return true;
    }

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

    bool get_table_schema(duckdb::ClientContext& context, const std::string& table_name,
                          TableSchema& schema) {
        try {
            std::string query = "PRAGMA table_info('" + table_name + "')";
            auto result = context.Query(query, false);

            if (result->HasError()) {
                return false;
            }

            schema.table_name = table_name;
            schema.columns.clear();

            for (auto& row : *result) {
                ColumnInfo col;
                col.name = row.GetValue(1).ToString();
                std::string type_str = row.GetValue(2).ToString();
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

        return duckdb::LogicalType::VARCHAR;
    }

    // Propagate changes through dependency graph
    void propagate_changes(const std::string& source_name) {
        // Get changes from source
        DuckDBZSet changes;

        if (tracked_tables_.count(source_name)) {
            changes = tracked_tables_[source_name]->consume_changes();
        }

        if (changes.empty()) return;

        // Get topological order of dependent views
        auto update_order = dep_graph_.topological_order(source_name);

        // First, apply to direct dependents of the source
        for (auto& [view_name, view] : views_) {
            auto sources = view->source_tables();
            for (const auto& src : sources) {
                if (src == source_name) {
                    view->apply_changes(source_name, changes);
                    break;
                }
            }
        }

        // Then propagate through the graph in topological order
        for (const auto& view_name : update_order) {
            auto it = views_.find(view_name);
            if (it == views_.end()) continue;

            // Get the view's sources that are also views
            auto sources = it->second->source_tables();
            for (const auto& src : sources) {
                if (views_.count(src)) {
                    // Get changes from the source view
                    // Note: In a full implementation, we'd track deltas per view
                    // For now, we re-apply the full state
                    it->second->apply_changes(src, views_[src]->get_result());
                }
            }
        }
    }

    // String escaping utilities
    std::string escape_string(const std::string& s) {
        std::string result;
        for (char c : s) {
            if (c == '\'') result += "''";
            else result += c;
        }
        return result;
    }

    std::string escape_json(const std::string& s) {
        std::string result;
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c;
            }
        }
        return result;
    }

    std::string unescape_json(const std::string& s) {
        std::string result;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                switch (s[i + 1]) {
                    case '"': result += '"'; i++; break;
                    case '\\': result += '\\'; i++; break;
                    case 'n': result += '\n'; i++; break;
                    case 'r': result += '\r'; i++; break;
                    case 't': result += '\t'; i++; break;
                    default: result += s[i];
                }
            } else {
                result += s[i];
            }
        }
        return result;
    }

    size_t find_string_end(const std::string& s, size_t start) {
        for (size_t i = start; i < s.size(); i++) {
            if (s[i] == '"' && (i == 0 || s[i-1] != '\\')) {
                return i;
            }
        }
        return s.size();
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<TrackedTable>> tracked_tables_;
    std::unordered_map<std::string, TableSchema> table_schemas_;
    std::unordered_map<std::string, std::unique_ptr<NativeMaterializedView>> views_;
    std::unordered_map<std::string, ViewDefinition> view_definitions_;
    DependencyGraph dep_graph_;
    std::string last_error_;
};

// Global CDC manager instance
inline CDCManager& get_cdc_manager() {
    static CDCManager manager;
    return manager;
}

} // namespace dbsp_native

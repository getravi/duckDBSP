// DuckDB DBSP Extension - Plug-and-Play Real-Time Materialized Views
//
// Usage:
//   LOAD 'dbsp';
//   SELECT dbsp_create_view('view_name', 'source_table', 'view_type', 'options');
//   SELECT * FROM dbsp_query('view_name');
//   SELECT dbsp_insert('table_name', row_data...);
//   SELECT dbsp_delete('table_name', row_data...);

#define DUCKDB_EXTENSION_MAIN

#include "../include/dbsp_materialized_view.hpp"

#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Simplified DuckDB-like API (standalone version)
// In a real extension, this would use actual DuckDB headers
// ============================================================================

namespace duckdb_dbsp {

using namespace dbsp;

// Global state for the extension
class DBSPState {
public:
    static DBSPState& instance() {
        static DBSPState state;
        return state;
    }

    MaterializedViewManager& views() { return view_manager_; }

    // Track table schemas for proper row construction
    void register_table(const std::string& name, const std::vector<std::string>& columns) {
        std::lock_guard<std::mutex> lock(mutex_);
        table_schemas_[name] = columns;
    }

    std::vector<std::string> get_schema(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = table_schemas_.find(name);
        return it != table_schemas_.end() ? it->second : std::vector<std::string>{};
    }

    // Track source tables for each view
    void register_view_source(const std::string& view_name, const std::string& table_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        view_sources_[view_name] = table_name;
    }

    std::string get_view_source(const std::string& view_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = view_sources_.find(view_name);
        return it != view_sources_.end() ? it->second : "";
    }

private:
    DBSPState() = default;
    MaterializedViewManager view_manager_;
    std::unordered_map<std::string, std::vector<std::string>> table_schemas_;
    std::unordered_map<std::string, std::string> view_sources_;
    std::mutex mutex_;
};

// Helper to parse a simple value string into a variant
std::variant<int64_t, double, std::string, bool> parse_value(const std::string& val, const std::string& type_hint = "") {
    // Try to detect type
    if (val == "true" || val == "TRUE") return true;
    if (val == "false" || val == "FALSE") return false;

    // Check if it's a quoted string
    if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'') {
        return val.substr(1, val.size() - 2);
    }
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
        return val.substr(1, val.size() - 2);
    }

    // Try integer
    try {
        size_t pos;
        int64_t i = std::stoll(val, &pos);
        if (pos == val.size()) return i;
    } catch (...) {}

    // Try double
    try {
        size_t pos;
        double d = std::stod(val, &pos);
        if (pos == val.size()) return d;
    } catch (...) {}

    // Default to string
    return val;
}

// Parse comma-separated values into a Row
Row parse_row(const std::string& values_str) {
    Row row;
    std::stringstream ss(values_str);
    std::string item;

    while (std::getline(ss, item, ',')) {
        // Trim whitespace
        size_t start = item.find_first_not_of(" \t");
        size_t end = item.find_last_not_of(" \t");
        if (start != std::string::npos) {
            item = item.substr(start, end - start + 1);
        }
        row.columns.push_back(parse_value(item));
    }

    return row;
}

// ============================================================================
// Public API Functions
// ============================================================================

// Create a filter view: SELECT * FROM table WHERE column op value
std::string create_filter_view(
    const std::string& view_name,
    const std::string& source_table,
    int column_index,
    const std::string& op,
    const std::string& value_str
) {
    auto value = parse_value(value_str);

    auto predicate = [column_index, op, value](const Row& row) -> bool {
        if (column_index >= static_cast<int>(row.columns.size())) return false;

        const auto& col = row.columns[column_index];

        // Handle integer comparisons
        if (std::holds_alternative<int64_t>(col) && std::holds_alternative<int64_t>(value)) {
            int64_t lhs = std::get<int64_t>(col);
            int64_t rhs = std::get<int64_t>(value);
            if (op == "=") return lhs == rhs;
            if (op == ">" || op == "gt") return lhs > rhs;
            if (op == "<" || op == "lt") return lhs < rhs;
            if (op == ">=" || op == "gte") return lhs >= rhs;
            if (op == "<=" || op == "lte") return lhs <= rhs;
            if (op == "!=" || op == "<>") return lhs != rhs;
        }

        // Handle double comparisons
        if (std::holds_alternative<double>(col) && std::holds_alternative<double>(value)) {
            double lhs = std::get<double>(col);
            double rhs = std::get<double>(value);
            if (op == "=") return lhs == rhs;
            if (op == ">" || op == "gt") return lhs > rhs;
            if (op == "<" || op == "lt") return lhs < rhs;
            if (op == ">=" || op == "gte") return lhs >= rhs;
            if (op == "<=" || op == "lte") return lhs <= rhs;
        }

        // Handle string comparisons
        if (std::holds_alternative<std::string>(col) && std::holds_alternative<std::string>(value)) {
            const std::string& lhs = std::get<std::string>(col);
            const std::string& rhs = std::get<std::string>(value);
            if (op == "=") return lhs == rhs;
            if (op == "!=" || op == "<>") return lhs != rhs;
            if (op == "contains") return lhs.find(rhs) != std::string::npos;
            if (op == "starts_with") return lhs.rfind(rhs, 0) == 0;
        }

        return false;
    };

    auto view = std::make_unique<FilteredView>(view_name, source_table, predicate);
    DBSPState::instance().views().register_view(std::move(view));
    DBSPState::instance().register_view_source(view_name, source_table);

    return "Created filter view: " + view_name;
}

// Create an aggregate view: SELECT group_col, AGG(value_col) FROM table GROUP BY group_col
std::string create_aggregate_view(
    const std::string& view_name,
    const std::string& source_table,
    int group_column,
    int value_column,
    const std::string& agg_type
) {
    AggregateView::AggType agg;
    if (agg_type == "SUM" || agg_type == "sum") agg = AggregateView::AggType::SUM;
    else if (agg_type == "COUNT" || agg_type == "count") agg = AggregateView::AggType::COUNT;
    else if (agg_type == "AVG" || agg_type == "avg") agg = AggregateView::AggType::AVG;
    else if (agg_type == "MIN" || agg_type == "min") agg = AggregateView::AggType::MIN;
    else if (agg_type == "MAX" || agg_type == "max") agg = AggregateView::AggType::MAX;
    else return "Error: Unknown aggregate type: " + agg_type;

    auto key_fn = [group_column](const Row& row) -> Row {
        Row key;
        if (group_column < static_cast<int>(row.columns.size())) {
            key.columns.push_back(row.columns[group_column]);
        }
        return key;
    };

    auto value_fn = [value_column](const Row& row) -> int64_t {
        if (value_column < static_cast<int>(row.columns.size())) {
            const auto& col = row.columns[value_column];
            if (std::holds_alternative<int64_t>(col)) return std::get<int64_t>(col);
            if (std::holds_alternative<double>(col)) return static_cast<int64_t>(std::get<double>(col));
        }
        return 0;
    };

    auto view = std::make_unique<AggregateView>(view_name, source_table, key_fn, value_fn, agg);
    DBSPState::instance().views().register_view(std::move(view));
    DBSPState::instance().register_view_source(view_name, source_table);

    return "Created aggregate view: " + view_name;
}

// Create a distinct view
std::string create_distinct_view(
    const std::string& view_name,
    const std::string& source_table
) {
    auto view = std::make_unique<DistinctView>(view_name, source_table);
    DBSPState::instance().views().register_view(std::move(view));
    DBSPState::instance().register_view_source(view_name, source_table);

    return "Created distinct view: " + view_name;
}

// Create a join view
std::string create_join_view(
    const std::string& view_name,
    const std::string& left_table,
    const std::string& right_table,
    int left_key_column,
    int right_key_column
) {
    auto left_key_fn = [left_key_column](const Row& row) -> std::string {
        if (left_key_column < static_cast<int>(row.columns.size())) {
            return std::visit([](const auto& v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) return v;
                else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
                else return std::to_string(v);
            }, row.columns[left_key_column]);
        }
        return "";
    };

    auto right_key_fn = [right_key_column](const Row& row) -> std::string {
        if (right_key_column < static_cast<int>(row.columns.size())) {
            return std::visit([](const auto& v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) return v;
                else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
                else return std::to_string(v);
            }, row.columns[right_key_column]);
        }
        return "";
    };

    auto view = std::make_unique<JoinView>(view_name, left_table, right_table, left_key_fn, right_key_fn);
    DBSPState::instance().views().register_view(std::move(view));
    // For joins, we need to track both source tables
    DBSPState::instance().register_view_source(view_name, left_table + "," + right_table);

    return "Created join view: " + view_name;
}

// Insert data into a table (propagates to all views)
std::string insert_row(const std::string& table_name, const std::string& values_str) {
    Row row = parse_row(values_str);
    DBSPState::instance().views().insert_row(table_name, row);
    return "Inserted 1 row into " + table_name;
}

// Delete data from a table (propagates to all views)
std::string delete_row(const std::string& table_name, const std::string& values_str) {
    Row row = parse_row(values_str);
    DBSPState::instance().views().delete_row(table_name, row);
    return "Deleted 1 row from " + table_name;
}

// Query a view - returns the current state as formatted string
std::string query_view(const std::string& view_name) {
    auto* view = DBSPState::instance().views().get_view(view_name);
    if (!view) {
        return "Error: View not found: " + view_name;
    }

    const auto& result = view->get_result();
    std::ostringstream oss;

    oss << "View: " << view_name << " (version " << view->version() << ", " << view->row_count() << " rows)\n";
    oss << std::string(60, '-') << "\n";

    for (const auto& [row, weight] : result) {
        oss << "| ";
        for (size_t i = 0; i < row.columns.size(); ++i) {
            if (i > 0) oss << " | ";
            std::visit([&oss](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, bool>) {
                    oss << (v ? "true" : "false");
                } else {
                    oss << v;
                }
            }, row.columns[i]);
        }
        if (weight != 1) {
            oss << " | (x" << weight << ")";
        }
        oss << " |\n";
    }

    return oss.str();
}

// Query view delta - returns only changes since last query
std::string query_view_delta(const std::string& view_name) {
    auto* view = DBSPState::instance().views().get_view(view_name);
    if (!view) {
        return "Error: View not found: " + view_name;
    }

    const auto& delta = view->get_delta();
    std::ostringstream oss;

    oss << "Delta for: " << view_name << "\n";
    oss << std::string(60, '-') << "\n";

    for (const auto& [row, weight] : delta) {
        oss << (weight > 0 ? "+ " : "- ");
        for (size_t i = 0; i < row.columns.size(); ++i) {
            if (i > 0) oss << ", ";
            std::visit([&oss](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, bool>) {
                    oss << (v ? "true" : "false");
                } else {
                    oss << v;
                }
            }, row.columns[i]);
        }
        oss << "\n";
    }

    return oss.str();
}

// List all views
std::string list_views() {
    auto names = DBSPState::instance().views().list_views();
    std::ostringstream oss;

    oss << "Registered Views:\n";
    oss << std::string(40, '-') << "\n";

    for (const auto& name : names) {
        auto* view = DBSPState::instance().views().get_view(name);
        if (view) {
            oss << "  " << name << " (" << view->row_count() << " rows, v" << view->version() << ")\n";
        }
    }

    if (names.empty()) {
        oss << "  (no views registered)\n";
    }

    return oss.str();
}

// Drop a view
std::string drop_view(const std::string& view_name) {
    if (DBSPState::instance().views().remove_view(view_name)) {
        return "Dropped view: " + view_name;
    }
    return "Error: View not found: " + view_name;
}

// Get view statistics
std::string view_stats(const std::string& view_name) {
    auto* view = DBSPState::instance().views().get_view(view_name);
    if (!view) {
        return "Error: View not found: " + view_name;
    }

    std::ostringstream oss;
    oss << "View Statistics: " << view_name << "\n";
    oss << "  Rows: " << view->row_count() << "\n";
    oss << "  Version: " << view->version() << "\n";
    oss << "  Source: " << DBSPState::instance().get_view_source(view_name) << "\n";

    return oss.str();
}

} // namespace duckdb_dbsp

// ============================================================================
// Command-line Interface for standalone testing
// ============================================================================

#ifdef DBSP_EXTENSION_MAIN

#include <readline/readline.h>
#include <readline/history.h>

void print_help() {
    std::cout << R"(
DBSP Extension - Real-Time Materialized Views
==============================================

Commands:
  CREATE FILTER VIEW <name> ON <table> WHERE <col_idx> <op> <value>
      Create a filter view. col_idx is 0-based.
      Operators: =, >, <, >=, <=, !=, contains, starts_with
      Example: CREATE FILTER VIEW high_orders ON orders WHERE 2 > 100

  CREATE AGGREGATE VIEW <name> ON <table> GROUP BY <col_idx> AGG <agg_type> <val_col_idx>
      Create an aggregate view.
      Aggregate types: SUM, COUNT, AVG, MIN, MAX
      Example: CREATE AGGREGATE VIEW order_totals ON orders GROUP BY 1 AGG SUM 2

  CREATE DISTINCT VIEW <name> ON <table>
      Create a distinct view.
      Example: CREATE DISTINCT VIEW unique_customers ON customers

  CREATE JOIN VIEW <name> ON <left_table> JOIN <right_table> ON <left_col> = <right_col>
      Create a join view.
      Example: CREATE JOIN VIEW orders_customers ON orders JOIN customers ON 1 = 0

  INSERT INTO <table> VALUES (<val1>, <val2>, ...)
      Insert a row. Strings should be quoted with single quotes.
      Example: INSERT INTO orders VALUES (1, 101, 250, 'West')

  DELETE FROM <table> VALUES (<val1>, <val2>, ...)
      Delete a row (must match exactly).
      Example: DELETE FROM orders VALUES (1, 101, 250, 'West')

  SELECT * FROM <view>
      Query a view's current state.

  SHOW DELTA <view>
      Show changes since last update.

  DROP VIEW <view>
      Remove a view.

  SHOW VIEWS
      List all views.

  STATS <view>
      Show view statistics.

  HELP
      Show this help.

  EXIT / QUIT
      Exit the program.

)" << std::endl;
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_quotes = false;
    char quote_char = 0;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];

        if (in_quotes) {
            if (c == quote_char) {
                current += c;
                in_quotes = false;
            } else {
                current += c;
            }
        } else {
            if (c == '\'' || c == '"') {
                in_quotes = true;
                quote_char = c;
                current += c;
            } else if (c == ' ' || c == '\t' || c == '(' || c == ')' || c == ',') {
                if (!current.empty()) {
                    tokens.push_back(current);
                    current.clear();
                }
            } else {
                current += c;
            }
        }
    }

    if (!current.empty()) {
        tokens.push_back(current);
    }

    return tokens;
}

std::string to_upper(std::string s) {
    for (auto& c : s) c = toupper(c);
    return s;
}

void process_command(const std::string& line) {
    auto tokens = tokenize(line);
    if (tokens.empty()) return;

    std::string cmd = to_upper(tokens[0]);

    if (cmd == "HELP") {
        print_help();
    }
    else if (cmd == "EXIT" || cmd == "QUIT") {
        std::cout << "Goodbye!\n";
        exit(0);
    }
    else if (cmd == "SHOW" && tokens.size() >= 2) {
        std::string subcmd = to_upper(tokens[1]);
        if (subcmd == "VIEWS") {
            std::cout << duckdb_dbsp::list_views();
        }
        else if (subcmd == "DELTA" && tokens.size() >= 3) {
            std::cout << duckdb_dbsp::query_view_delta(tokens[2]);
        }
    }
    else if (cmd == "STATS" && tokens.size() >= 2) {
        std::cout << duckdb_dbsp::view_stats(tokens[1]);
    }
    else if (cmd == "DROP" && tokens.size() >= 3 && to_upper(tokens[1]) == "VIEW") {
        std::cout << duckdb_dbsp::drop_view(tokens[2]) << "\n";
    }
    else if (cmd == "SELECT" && tokens.size() >= 4 && to_upper(tokens[2]) == "FROM") {
        std::cout << duckdb_dbsp::query_view(tokens[3]);
    }
    else if (cmd == "CREATE" && tokens.size() >= 4) {
        std::string view_type = to_upper(tokens[1]);

        if (view_type == "FILTER" && to_upper(tokens[2]) == "VIEW") {
            // CREATE FILTER VIEW <name> ON <table> WHERE <col> <op> <val>
            if (tokens.size() >= 10 && to_upper(tokens[4]) == "ON" && to_upper(tokens[6]) == "WHERE") {
                std::string view_name = tokens[3];
                std::string table_name = tokens[5];
                int col_idx = std::stoi(tokens[7]);
                std::string op = tokens[8];
                std::string value = tokens[9];
                std::cout << duckdb_dbsp::create_filter_view(view_name, table_name, col_idx, op, value) << "\n";
            } else {
                std::cout << "Syntax: CREATE FILTER VIEW <name> ON <table> WHERE <col_idx> <op> <value>\n";
            }
        }
        else if (view_type == "AGGREGATE" && to_upper(tokens[2]) == "VIEW") {
            // CREATE AGGREGATE VIEW <name> ON <table> GROUP BY <col> AGG <type> <val_col>
            if (tokens.size() >= 12) {
                std::string view_name = tokens[3];
                std::string table_name = tokens[5];
                int group_col = std::stoi(tokens[8]);
                std::string agg_type = tokens[10];
                int val_col = std::stoi(tokens[11]);
                std::cout << duckdb_dbsp::create_aggregate_view(view_name, table_name, group_col, val_col, agg_type) << "\n";
            } else {
                std::cout << "Syntax: CREATE AGGREGATE VIEW <name> ON <table> GROUP BY <col> AGG <type> <val_col>\n";
            }
        }
        else if (view_type == "DISTINCT" && to_upper(tokens[2]) == "VIEW") {
            // CREATE DISTINCT VIEW <name> ON <table>
            if (tokens.size() >= 6 && to_upper(tokens[4]) == "ON") {
                std::cout << duckdb_dbsp::create_distinct_view(tokens[3], tokens[5]) << "\n";
            } else {
                std::cout << "Syntax: CREATE DISTINCT VIEW <name> ON <table>\n";
            }
        }
        else if (view_type == "JOIN" && to_upper(tokens[2]) == "VIEW") {
            // CREATE JOIN VIEW <name> ON <left> JOIN <right> ON <lcol> = <rcol>
            if (tokens.size() >= 12 && to_upper(tokens[4]) == "ON" && to_upper(tokens[6]) == "JOIN") {
                std::string view_name = tokens[3];
                std::string left_table = tokens[5];
                std::string right_table = tokens[7];
                int left_col = std::stoi(tokens[9]);
                int right_col = std::stoi(tokens[11]);
                std::cout << duckdb_dbsp::create_join_view(view_name, left_table, right_table, left_col, right_col) << "\n";
            } else {
                std::cout << "Syntax: CREATE JOIN VIEW <name> ON <left_table> JOIN <right_table> ON <left_col> = <right_col>\n";
            }
        }
    }
    else if (cmd == "INSERT" && tokens.size() >= 4 && to_upper(tokens[1]) == "INTO") {
        // INSERT INTO <table> VALUES <val1> <val2> ...
        std::string table_name = tokens[2];
        std::string values;
        bool in_values = false;
        for (size_t i = 3; i < tokens.size(); ++i) {
            if (to_upper(tokens[i]) == "VALUES") {
                in_values = true;
                continue;
            }
            if (in_values) {
                if (!values.empty()) values += ",";
                values += tokens[i];
            }
        }
        if (!values.empty()) {
            std::cout << duckdb_dbsp::insert_row(table_name, values) << "\n";
        }
    }
    else if (cmd == "DELETE" && tokens.size() >= 4 && to_upper(tokens[1]) == "FROM") {
        // DELETE FROM <table> VALUES <val1> <val2> ...
        std::string table_name = tokens[2];
        std::string values;
        bool in_values = false;
        for (size_t i = 3; i < tokens.size(); ++i) {
            if (to_upper(tokens[i]) == "VALUES") {
                in_values = true;
                continue;
            }
            if (in_values) {
                if (!values.empty()) values += ",";
                values += tokens[i];
            }
        }
        if (!values.empty()) {
            std::cout << duckdb_dbsp::delete_row(table_name, values) << "\n";
        }
    }
    else {
        std::cout << "Unknown command. Type HELP for usage.\n";
    }
}

int main(int argc, char* argv[]) {
    std::cout << "DBSP Extension - Real-Time Materialized Views\n";
    std::cout << "Type HELP for commands, EXIT to quit.\n\n";

    // Check if readline is available
    char* line;
    while ((line = readline("dbsp> ")) != nullptr) {
        if (strlen(line) > 0) {
            add_history(line);
            process_command(line);
        }
        free(line);
    }

    std::cout << "\nGoodbye!\n";
    return 0;
}

#endif // DBSP_EXTENSION_MAIN

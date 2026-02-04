// DBSP CLI - Simple command-line interface (no readline dependency)
// Build: g++ -std=c++17 -o dbsp_cli dbsp_cli.cpp -I../include

#include "../include/dbsp_materialized_view.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

using namespace dbsp;

// Global view manager
static MaterializedViewManager g_views;

// Helper to parse a value string
std::variant<int64_t, double, std::string, bool> parse_value(const std::string& val) {
    if (val == "true" || val == "TRUE") return true;
    if (val == "false" || val == "FALSE") return false;

    // Check for quoted string
    if (val.size() >= 2 && ((val.front() == '\'' && val.back() == '\'') ||
                            (val.front() == '"' && val.back() == '"'))) {
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

    return val;
}

// Parse comma-separated values
Row parse_row(const std::string& values_str) {
    Row row;
    std::stringstream ss(values_str);
    std::string item;

    while (std::getline(ss, item, ',')) {
        size_t start = item.find_first_not_of(" \t");
        size_t end = item.find_last_not_of(" \t");
        if (start != std::string::npos) {
            item = item.substr(start, end - start + 1);
        }
        row.columns.push_back(parse_value(item));
    }
    return row;
}

// Tokenize input line
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_quotes = false;
    char quote_char = 0;

    for (char c : line) {
        if (in_quotes) {
            current += c;
            if (c == quote_char) in_quotes = false;
        } else {
            if (c == '\'' || c == '"') {
                in_quotes = true;
                quote_char = c;
                current += c;
            } else if (std::isspace(c) || c == '(' || c == ')' || c == ',') {
                if (!current.empty()) {
                    tokens.push_back(current);
                    current.clear();
                }
            } else {
                current += c;
            }
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

void print_row(const Row& row, Weight weight) {
    std::cout << "| ";
    for (size_t i = 0; i < row.columns.size(); ++i) {
        if (i > 0) std::cout << " | ";
        std::visit([](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) std::cout << (v ? "true" : "false");
            else std::cout << v;
        }, row.columns[i]);
    }
    if (weight != 1) std::cout << " | (x" << weight << ")";
    std::cout << " |\n";
}

void print_help() {
    std::cout << R"(
DBSP - Real-Time Materialized Views
====================================

CREATE FILTER VIEW <name> ON <table> WHERE <col> <op> <value>
  Example: CREATE FILTER VIEW big_orders ON orders WHERE 2 > 100

CREATE AGGREGATE VIEW <name> ON <table> GROUP BY <col> AGG <SUM|COUNT> <val_col>
  Example: CREATE AGGREGATE VIEW totals ON orders GROUP BY 1 AGG SUM 2

CREATE DISTINCT VIEW <name> ON <table>
CREATE JOIN VIEW <name> ON <t1> JOIN <t2> ON <col1> = <col2>

INSERT INTO <table> VALUES <v1>, <v2>, ...
DELETE FROM <table> VALUES <v1>, <v2>, ...

SELECT * FROM <view>
SHOW DELTA <view>
SHOW VIEWS
DROP VIEW <view>
HELP | EXIT

)" << std::endl;
}

void process(const std::string& line) {
    auto tokens = tokenize(line);
    if (tokens.empty()) return;

    std::string cmd = to_upper(tokens[0]);

    if (cmd == "HELP") {
        print_help();
    }
    else if (cmd == "EXIT" || cmd == "QUIT") {
        exit(0);
    }
    else if (cmd == "SHOW") {
        if (tokens.size() < 2) return;
        std::string sub = to_upper(tokens[1]);

        if (sub == "VIEWS") {
            std::cout << "\nRegistered Views:\n";
            for (const auto& name : g_views.list_views()) {
                auto* v = g_views.get_view(name);
                std::cout << "  " << name << " (" << v->row_count() << " rows)\n";
            }
            std::cout << "\n";
        }
        else if (sub == "DELTA" && tokens.size() >= 3) {
            auto* v = g_views.get_view(tokens[2]);
            if (!v) { std::cout << "View not found\n"; return; }
            std::cout << "\nDelta:\n";
            for (const auto& [row, w] : v->get_delta()) {
                std::cout << (w > 0 ? "+ " : "- ");
                for (size_t i = 0; i < row.columns.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::visit([](const auto& v) { std::cout << v; }, row.columns[i]);
                }
                std::cout << "\n";
            }
        }
    }
    else if (cmd == "DROP" && tokens.size() >= 3 && to_upper(tokens[1]) == "VIEW") {
        if (g_views.remove_view(tokens[2]))
            std::cout << "Dropped: " << tokens[2] << "\n";
        else
            std::cout << "Not found\n";
    }
    else if (cmd == "SELECT" && tokens.size() >= 4 && to_upper(tokens[2]) == "FROM") {
        auto* v = g_views.get_view(tokens[3]);
        if (!v) { std::cout << "View not found\n"; return; }

        std::cout << "\n" << tokens[3] << " (" << v->row_count() << " rows):\n";
        std::cout << std::string(50, '-') << "\n";
        for (const auto& [row, w] : v->get_result()) {
            print_row(row, w);
        }
        std::cout << "\n";
    }
    else if (cmd == "CREATE" && tokens.size() >= 3) {
        std::string type = to_upper(tokens[1]);

        if (type == "FILTER" && to_upper(tokens[2]) == "VIEW" && tokens.size() >= 10) {
            // CREATE FILTER VIEW name ON table WHERE col op val
            std::string name = tokens[3];
            std::string table = tokens[5];
            int col = std::stoi(tokens[7]);
            std::string op = tokens[8];
            auto value = parse_value(tokens[9]);

            auto pred = [col, op, value](const Row& row) -> bool {
                if (col >= (int)row.columns.size()) return false;
                const auto& c = row.columns[col];

                if (std::holds_alternative<int64_t>(c) && std::holds_alternative<int64_t>(value)) {
                    int64_t lhs = std::get<int64_t>(c), rhs = std::get<int64_t>(value);
                    if (op == "=" || op == "==") return lhs == rhs;
                    if (op == ">" || op == "gt") return lhs > rhs;
                    if (op == "<" || op == "lt") return lhs < rhs;
                    if (op == ">=" || op == "gte") return lhs >= rhs;
                    if (op == "<=" || op == "lte") return lhs <= rhs;
                    if (op == "!=" || op == "<>") return lhs != rhs;
                }
                if (std::holds_alternative<std::string>(c) && std::holds_alternative<std::string>(value)) {
                    const auto& lhs = std::get<std::string>(c);
                    const auto& rhs = std::get<std::string>(value);
                    if (op == "=" || op == "==") return lhs == rhs;
                    if (op == "!=") return lhs != rhs;
                }
                return false;
            };

            g_views.register_view(std::make_unique<FilteredView>(name, table, pred));
            std::cout << "Created filter view: " << name << "\n";
        }
        else if (type == "AGGREGATE" && to_upper(tokens[2]) == "VIEW" && tokens.size() >= 12) {
            // CREATE AGGREGATE VIEW name ON table GROUP BY col AGG type val_col
            std::string name = tokens[3];
            std::string table = tokens[5];
            int gcol = std::stoi(tokens[8]);
            std::string agg = to_upper(tokens[10]);
            int vcol = std::stoi(tokens[11]);

            AggregateView::AggType at = AggregateView::AggType::SUM;
            if (agg == "COUNT") at = AggregateView::AggType::COUNT;
            else if (agg == "AVG") at = AggregateView::AggType::AVG;

            auto kfn = [gcol](const Row& r) -> Row {
                Row k; if (gcol < (int)r.columns.size()) k.columns.push_back(r.columns[gcol]); return k;
            };
            auto vfn = [vcol](const Row& r) -> int64_t {
                if (vcol < (int)r.columns.size()) {
                    if (auto* p = std::get_if<int64_t>(&r.columns[vcol])) return *p;
                    if (auto* p = std::get_if<double>(&r.columns[vcol])) return (int64_t)*p;
                }
                return 0;
            };

            g_views.register_view(std::make_unique<AggregateView>(name, table, kfn, vfn, at));
            std::cout << "Created aggregate view: " << name << "\n";
        }
        else if (type == "DISTINCT" && to_upper(tokens[2]) == "VIEW" && tokens.size() >= 6) {
            g_views.register_view(std::make_unique<DistinctView>(tokens[3], tokens[5]));
            std::cout << "Created distinct view: " << tokens[3] << "\n";
        }
        else if (type == "JOIN" && to_upper(tokens[2]) == "VIEW" && tokens.size() >= 12) {
            // CREATE JOIN VIEW name ON t1 JOIN t2 ON col1 = col2
            std::string name = tokens[3];
            std::string t1 = tokens[5];
            std::string t2 = tokens[7];
            int c1 = std::stoi(tokens[9]);
            int c2 = std::stoi(tokens[11]);

            auto k1 = [c1](const Row& r) -> std::string {
                if (c1 < (int)r.columns.size()) {
                    return std::visit([](const auto& v) -> std::string {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, std::string>) return v;
                        else if constexpr (std::is_same_v<T, bool>) return v ? "1" : "0";
                        else return std::to_string(v);
                    }, r.columns[c1]);
                }
                return "";
            };
            auto k2 = [c2](const Row& r) -> std::string {
                if (c2 < (int)r.columns.size()) {
                    return std::visit([](const auto& v) -> std::string {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, std::string>) return v;
                        else if constexpr (std::is_same_v<T, bool>) return v ? "1" : "0";
                        else return std::to_string(v);
                    }, r.columns[c2]);
                }
                return "";
            };

            g_views.register_view(std::make_unique<JoinView>(name, t1, t2, k1, k2));
            std::cout << "Created join view: " << name << "\n";
        }
    }
    else if (cmd == "INSERT" && tokens.size() >= 4 && to_upper(tokens[1]) == "INTO") {
        std::string table = tokens[2];
        std::string vals;
        bool in_vals = false;
        for (size_t i = 3; i < tokens.size(); ++i) {
            if (to_upper(tokens[i]) == "VALUES") { in_vals = true; continue; }
            if (in_vals) { if (!vals.empty()) vals += ","; vals += tokens[i]; }
        }
        if (!vals.empty()) {
            g_views.insert_row(table, parse_row(vals));
            std::cout << "Inserted row\n";
        }
    }
    else if (cmd == "DELETE" && tokens.size() >= 4 && to_upper(tokens[1]) == "FROM") {
        std::string table = tokens[2];
        std::string vals;
        bool in_vals = false;
        for (size_t i = 3; i < tokens.size(); ++i) {
            if (to_upper(tokens[i]) == "VALUES") { in_vals = true; continue; }
            if (in_vals) { if (!vals.empty()) vals += ","; vals += tokens[i]; }
        }
        if (!vals.empty()) {
            g_views.delete_row(table, parse_row(vals));
            std::cout << "Deleted row\n";
        }
    }
    else {
        std::cout << "Unknown command. Type HELP.\n";
    }
}

int main() {
    std::cout << "DBSP - Real-Time Materialized Views\n";
    std::cout << "Type HELP for commands, EXIT to quit.\n\n";

    std::string line;
    while (true) {
        std::cout << "dbsp> ";
        if (!std::getline(std::cin, line)) break;
        if (!line.empty()) process(line);
    }

    return 0;
}

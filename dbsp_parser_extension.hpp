// DBSP Parser Extension for CREATE/DROP MATERIALIZED VIEW
// Provides native SQL syntax for materialized views

#pragma once

#include "duckdb.hpp"
#include "duckdb/parser/parser_extension.hpp"
#include "duckdb/function/table_function.hpp"

namespace dbsp_native {

using namespace duckdb;

//===--------------------------------------------------------------------===//
// Parse Data Structures
//===--------------------------------------------------------------------===//

// Statement types for materialized views
enum class MaterializedViewStatementType {
    CREATE,
    DROP,
    REFRESH
};

// Parse data for CREATE MATERIALIZED VIEW
struct CreateMaterializedViewParseData : public ParserExtensionParseData {
    string view_name;
    string select_query;
    bool if_not_exists = false;

    unique_ptr<ParserExtensionParseData> Copy() const override {
        auto result = make_uniq<CreateMaterializedViewParseData>();
        result->view_name = view_name;
        result->select_query = select_query;
        result->if_not_exists = if_not_exists;
        return std::move(result);
    }

    string ToString() const override {
        string result = "CREATE MATERIALIZED VIEW";
        if (if_not_exists) {
            result += " IF NOT EXISTS";
        }
        result += " " + view_name + " AS " + select_query;
        return result;
    }
};

// Parse data for DROP MATERIALIZED VIEW
struct DropMaterializedViewParseData : public ParserExtensionParseData {
    string view_name;
    bool if_exists = false;
    bool cascade = false;

    unique_ptr<ParserExtensionParseData> Copy() const override {
        auto result = make_uniq<DropMaterializedViewParseData>();
        result->view_name = view_name;
        result->if_exists = if_exists;
        result->cascade = cascade;
        return std::move(result);
    }

    string ToString() const override {
        string result = "DROP MATERIALIZED VIEW";
        if (if_exists) {
            result += " IF EXISTS";
        }
        result += " " + view_name;
        if (cascade) {
            result += " CASCADE";
        }
        return result;
    }
};

// Parse data for REFRESH MATERIALIZED VIEW
struct RefreshMaterializedViewParseData : public ParserExtensionParseData {
    string view_name;

    unique_ptr<ParserExtensionParseData> Copy() const override {
        auto result = make_uniq<RefreshMaterializedViewParseData>();
        result->view_name = view_name;
        return std::move(result);
    }

    string ToString() const override {
        return "REFRESH MATERIALIZED VIEW " + view_name;
    }
};

//===--------------------------------------------------------------------===//
// Parser Functions
//===--------------------------------------------------------------------===//

// Parse CREATE MATERIALIZED VIEW statement
inline ParserExtensionParseResult ParseCreateMaterializedView(const string &query) {
    // Simple regex-based parsing for now
    // Format: CREATE MATERIALIZED VIEW [IF NOT EXISTS] name AS select_query

    auto query_upper = StringUtil::Upper(query);

    // Check for CREATE MATERIALIZED VIEW
    if (query_upper.find("CREATE MATERIALIZED VIEW") != 0) {
        return ParserExtensionParseResult(); // Not our statement
    }

    auto result = make_uniq<CreateMaterializedViewParseData>();

    // Extract IF NOT EXISTS
    size_t pos = strlen("CREATE MATERIALIZED VIEW");
    while (pos < query.length() && std::isspace(query[pos])) pos++;

    if (query_upper.find("IF NOT EXISTS", pos) == pos) {
        result->if_not_exists = true;
        pos += strlen("IF NOT EXISTS");
        while (pos < query.length() && std::isspace(query[pos])) pos++;
    }

    // Extract view name (up to AS keyword)
    size_t as_pos = query_upper.find(" AS ", pos);
    if (as_pos == string::npos) {
        return ParserExtensionParseResult("Missing AS keyword in CREATE MATERIALIZED VIEW");
    }

    result->view_name = query.substr(pos, as_pos - pos);
    result->view_name = StringUtil::Trim(result->view_name);

    // Extract SELECT query (everything after AS)
    pos = as_pos + 4; // Skip " AS "
    result->select_query = query.substr(pos);
    result->select_query = StringUtil::Trim(result->select_query);

    // Remove trailing semicolon if present
    if (!result->select_query.empty() && result->select_query.back() == ';') {
        result->select_query.pop_back();
    }

    return ParserExtensionParseResult(std::move(result));
}

// Parse DROP MATERIALIZED VIEW statement
inline ParserExtensionParseResult ParseDropMaterializedView(const string &query) {
    auto query_upper = StringUtil::Upper(query);

    // Check for DROP MATERIALIZED VIEW
    if (query_upper.find("DROP MATERIALIZED VIEW") != 0) {
        return ParserExtensionParseResult();
    }

    auto result = make_uniq<DropMaterializedViewParseData>();

    size_t pos = strlen("DROP MATERIALIZED VIEW");
    while (pos < query.length() && std::isspace(query[pos])) pos++;

    // Extract IF EXISTS
    if (query_upper.find("IF EXISTS", pos) == pos) {
        result->if_exists = true;
        pos += strlen("IF EXISTS");
        while (pos < query.length() && std::isspace(query[pos])) pos++;
    }

    // Extract view name (up to CASCADE/RESTRICT or end)
    size_t end_pos = query.length();
    size_t cascade_pos = query_upper.find("CASCADE", pos);
    size_t restrict_pos = query_upper.find("RESTRICT", pos);

    if (cascade_pos != string::npos) {
        end_pos = cascade_pos;
        result->cascade = true;
    } else if (restrict_pos != string::npos) {
        end_pos = restrict_pos;
        result->cascade = false;
    }

    result->view_name = query.substr(pos, end_pos - pos);
    result->view_name = StringUtil::Trim(result->view_name);

    // Remove trailing semicolon
    if (!result->view_name.empty() && result->view_name.back() == ';') {
        result->view_name.pop_back();
        result->view_name = StringUtil::Trim(result->view_name);
    }

    return ParserExtensionParseResult(std::move(result));
}

// Parse REFRESH MATERIALIZED VIEW statement
inline ParserExtensionParseResult ParseRefreshMaterializedView(const string &query) {
    auto query_upper = StringUtil::Upper(query);

    if (query_upper.find("REFRESH MATERIALIZED VIEW") != 0) {
        return ParserExtensionParseResult();
    }

    auto result = make_uniq<RefreshMaterializedViewParseData>();

    size_t pos = strlen("REFRESH MATERIALIZED VIEW");
    while (pos < query.length() && std::isspace(query[pos])) pos++;

    result->view_name = query.substr(pos);
    result->view_name = StringUtil::Trim(result->view_name);

    // Remove trailing semicolon
    if (!result->view_name.empty() && result->view_name.back() == ';') {
        result->view_name.pop_back();
        result->view_name = StringUtil::Trim(result->view_name);
    }

    return ParserExtensionParseResult(std::move(result));
}

// Main parse function - tries all statement types
inline ParserExtensionParseResult MaterializedViewParse(ParserExtensionInfo *info, const string &query) {
    auto query_upper = StringUtil::Upper(query);

    // Try CREATE MATERIALIZED VIEW
    if (query_upper.find("CREATE MATERIALIZED VIEW") == 0) {
        return ParseCreateMaterializedView(query);
    }

    // Try DROP MATERIALIZED VIEW
    if (query_upper.find("DROP MATERIALIZED VIEW") == 0) {
        return ParseDropMaterializedView(query);
    }

    // Try REFRESH MATERIALIZED VIEW
    if (query_upper.find("REFRESH MATERIALIZED VIEW") == 0) {
        return ParseRefreshMaterializedView(query);
    }

    // Not a materialized view statement
    return ParserExtensionParseResult();
}

//===--------------------------------------------------------------------===//
// Plan Function (Implementation in dbsp_extension.cpp)
//===--------------------------------------------------------------------===//

// Forward declaration - implementation will be in dbsp_extension.cpp
ParserExtensionPlanResult MaterializedViewPlan(ParserExtensionInfo *info,
                                               ClientContext &context,
                                               unique_ptr<ParserExtensionParseData> parse_data);

//===--------------------------------------------------------------------===//
// Extension Registration
//===--------------------------------------------------------------------===//

inline ParserExtension CreateMaterializedViewParserExtension() {
    ParserExtension extension;
    extension.parse_function = MaterializedViewParse;
    extension.plan_function = MaterializedViewPlan;
    extension.parser_info = nullptr; // No additional info needed
    return extension;
}

} // namespace dbsp_native

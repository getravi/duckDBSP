// DBSP Error Handling System
// Structured error codes with comprehensive error messages and documentation

#pragma once

#include <string>
#include <sstream>
#include <iomanip>
#include <unordered_map>

namespace dbsp_native {

enum class ErrorCode {
    // Parser errors (E1xx) - Unsupported SQL features
    HAVING_NOT_SUPPORTED = 101,
    ORDER_BY_NOT_SUPPORTED = 102,
    LIMIT_NOT_SUPPORTED = 103,
    WINDOW_FUNCTIONS_NOT_SUPPORTED = 104,
    SUBQUERY_NOT_SUPPORTED = 105,
    UNION_NOT_SUPPORTED = 106,
    INTERSECT_NOT_SUPPORTED = 107,
    EXCEPT_NOT_SUPPORTED = 108,
    CTE_NOT_SUPPORTED = 109,

    // Validation errors (E2xx) - Invalid input
    INVALID_IDENTIFIER = 201,
    PATH_TRAVERSAL = 202,
    CIRCULAR_DEPENDENCY = 203,
    IDENTIFIER_TOO_LONG = 204,
    RESERVED_KEYWORD = 205,

    // Runtime errors (E3xx) - Execution failures
    VIEW_UPDATE_FAILED = 301,
    TYPE_MISMATCH = 302,
    NULL_CONSTRAINT_VIOLATION = 303,
    CASCADE_UPDATE_FAILED = 304,

    // Resource errors (E4xx) - Limits exceeded
    MEMORY_LIMIT_EXCEEDED = 401,
    TOO_MANY_VIEWS = 402,
    VIEW_TOO_LARGE = 403,
    NESTING_DEPTH_EXCEEDED = 404,

    // Persistence errors (E5xx) - I/O failures
    FILE_READ_ERROR = 501,
    FILE_WRITE_ERROR = 502,
    SERIALIZATION_ERROR = 503,
    DESERIALIZATION_ERROR = 504,
};

struct ErrorInfo {
    ErrorCode code;
    std::string message;           // Brief description
    std::string sql;               // The SQL that caused error (if applicable)
    size_t error_position = 0;     // Character offset in SQL
    std::string context;           // Additional context (view name, etc.)
    std::string workaround;        // Suggested alternative
    std::string doc_link;          // Path to error documentation
};

// Format error code as DBSP-Exxx string
inline std::string format_error_code(ErrorCode code) {
    std::stringstream ss;
    ss << "DBSP-E" << std::setfill('0') << std::setw(3)
       << static_cast<int>(code);
    return ss.str();
}

// Get documentation link for error code
inline std::string get_doc_link(ErrorCode code) {
    int category = static_cast<int>(code) / 100;
    std::stringstream ss;
    ss << "docs/errors/E" << category << "xx/"
       << format_error_code(code) << ".md";
    return ss.str();
}

// Get workaround suggestion for error code
inline std::string get_workaround(ErrorCode code) {
    static const std::unordered_map<ErrorCode, std::string> workarounds = {
        // Parser errors (E1xx)
        {ErrorCode::HAVING_NOT_SUPPORTED,
         "Use a nested view: create a view with GROUP BY, then create another "
         "view with WHERE clause to filter the aggregated results. "
         "Tracked in TODO #3."},
        {ErrorCode::ORDER_BY_NOT_SUPPORTED,
         "Query the view using dbsp_query() and sort results client-side. "
         "ORDER BY support is tracked in TODO #4."},
        {ErrorCode::LIMIT_NOT_SUPPORTED,
         "Query the view and apply LIMIT in the outer query. "
         "LIMIT support is tracked in TODO #4."},
        {ErrorCode::WINDOW_FUNCTIONS_NOT_SUPPORTED,
         "Window functions are not yet supported for incremental computation. "
         "Tracked in TODO #8."},
        {ErrorCode::SUBQUERY_NOT_SUPPORTED,
         "Rewrite using JOINs or create intermediate views. "
         "Subquery support is tracked in TODO #6."},
        {ErrorCode::UNION_NOT_SUPPORTED,
         "Create separate views and query them individually. "
         "Set operations are tracked in TODO #7."},
        {ErrorCode::INTERSECT_NOT_SUPPORTED,
         "Use JOIN to find common rows between views. "
         "Set operations are tracked in TODO #7."},
        {ErrorCode::EXCEPT_NOT_SUPPORTED,
         "Use LEFT JOIN with NULL check to find differences. "
         "Set operations are tracked in TODO #7."},

        // Validation errors (E2xx)
        {ErrorCode::INVALID_IDENTIFIER,
         "Use only alphanumeric characters and underscores. "
         "Start with a letter or underscore."},
        {ErrorCode::PATH_TRAVERSAL,
         "Use relative paths without '..' or absolute paths. "
         "Ensure paths don't contain null bytes or backslashes."},
        {ErrorCode::CIRCULAR_DEPENDENCY,
         "Review view dependencies to break the cycle. "
         "A view cannot depend on itself directly or transitively."},
        {ErrorCode::IDENTIFIER_TOO_LONG,
         "Use shorter names (max 255 characters)."},

        // Runtime errors (E3xx)
        {ErrorCode::VIEW_UPDATE_FAILED,
         "Check view definition and source data types for compatibility. "
         "Ensure all source tables exist and have correct schema."},
        {ErrorCode::TYPE_MISMATCH,
         "Verify column types match between source and view definition. "
         "Check aggregate function input types."},
    };

    auto it = workarounds.find(code);
    return it != workarounds.end() ? it->second : "";
}

// Format complete error message with SQL highlighting
inline std::string format_error(const ErrorInfo& info) {
    std::stringstream ss;

    // Error code and message
    ss << format_error_code(info.code) << ": " << info.message << "\n\n";

    // Show SQL with position marker if available
    if (!info.sql.empty()) {
        ss << "SQL:\n" << info.sql << "\n";
        if (info.error_position > 0 && info.error_position < info.sql.length()) {
            ss << std::string(info.error_position, ' ') << "^\n";
        }
        ss << "\n";
    }

    // Context (view name, etc.)
    if (!info.context.empty()) {
        ss << "Context: " << info.context << "\n\n";
    }

    // Workaround
    if (!info.workaround.empty()) {
        ss << "Workaround:\n" << info.workaround << "\n\n";
    }

    // Documentation link
    std::string doc_link = info.doc_link.empty() ? get_doc_link(info.code) : info.doc_link;
    ss << "Documentation: " << doc_link << "\n";

    return ss.str();
}

} // namespace dbsp_native

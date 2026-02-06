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

} // namespace dbsp_native

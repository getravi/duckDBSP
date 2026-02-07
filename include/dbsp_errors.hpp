// DBSP Error Handling System
// Structured error codes with comprehensive error messages and documentation

#pragma once

#include <iomanip>
#include <sstream>
#include <string>
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
  std::string message;       // Brief description
  std::string sql;           // The SQL that caused error (if applicable)
  size_t error_position = 0; // Character offset in SQL
  std::string context;       // Additional context (view name, etc.)
  std::string workaround;    // Suggested alternative
  std::string doc_link;      // Path to error documentation
};

// Format error code as DBSP-Exxx string
inline std::string format_error_code(ErrorCode code) {
  std::stringstream ss;
  ss << "DBSP-E" << std::setfill('0') << std::setw(3) << static_cast<int>(code);
  return ss.str();
}

// Get documentation link for error code
inline std::string get_doc_link(ErrorCode code) {
  int category = static_cast<int>(code) / 100;
  std::stringstream ss;
  ss << "docs/errors/E" << category << "xx/" << format_error_code(code)
     << ".md";
  return ss.str();
}

// Get brief error message for error code
inline std::string get_message(ErrorCode code) {
  static const std::unordered_map<ErrorCode, std::string> messages = {
      // Parser errors (E1xx)
      {ErrorCode::HAVING_NOT_SUPPORTED, "HAVING clause not supported"},
      {ErrorCode::ORDER_BY_NOT_SUPPORTED, "ORDER BY not supported in views"},
      {ErrorCode::LIMIT_NOT_SUPPORTED, "LIMIT not supported in views"},
      {ErrorCode::WINDOW_FUNCTIONS_NOT_SUPPORTED,
       "Window functions not supported"},
      {ErrorCode::SUBQUERY_NOT_SUPPORTED, "Subqueries not supported"},
      {ErrorCode::UNION_NOT_SUPPORTED, "UNION not supported in views"},
      {ErrorCode::INTERSECT_NOT_SUPPORTED, "INTERSECT not supported"},
      {ErrorCode::EXCEPT_NOT_SUPPORTED, "EXCEPT not supported"},
      {ErrorCode::CTE_NOT_SUPPORTED, "Non-recursive CTEs not supported"},
      // Validation errors (E2xx)
      {ErrorCode::INVALID_IDENTIFIER, "Invalid identifier name"},
      {ErrorCode::PATH_TRAVERSAL, "Path traversal detected"},
      {ErrorCode::CIRCULAR_DEPENDENCY, "Circular view dependency"},
      {ErrorCode::IDENTIFIER_TOO_LONG, "Identifier too long"},
      {ErrorCode::RESERVED_KEYWORD, "Reserved keyword used"},
      // Runtime errors (E3xx)
      {ErrorCode::VIEW_UPDATE_FAILED, "View update failed"},
      {ErrorCode::TYPE_MISMATCH, "Type mismatch in view"},
      {ErrorCode::NULL_CONSTRAINT_VIOLATION, "NULL constraint violated"},
      {ErrorCode::CASCADE_UPDATE_FAILED, "Cascade update failed"},
      // Resource errors (E4xx)
      {ErrorCode::MEMORY_LIMIT_EXCEEDED, "Memory limit exceeded"},
      {ErrorCode::TOO_MANY_VIEWS, "Too many views"},
      {ErrorCode::VIEW_TOO_LARGE, "View result set too large"},
      {ErrorCode::NESTING_DEPTH_EXCEEDED, "View nesting too deep"},
      // Persistence errors (E5xx)
      {ErrorCode::FILE_READ_ERROR, "Cannot read file"},
      {ErrorCode::FILE_WRITE_ERROR, "Cannot write file"},
      {ErrorCode::SERIALIZATION_ERROR, "Serialization failed"},
      {ErrorCode::DESERIALIZATION_ERROR, "Deserialization failed"},
  };
  auto it = messages.find(code);
  return it != messages.end() ? it->second : "Unknown error";
}

// Get workaround suggestion for error code
inline std::string get_workaround(ErrorCode code) {
  static const std::unordered_map<ErrorCode, std::string> workarounds = {
      // Parser errors (E1xx)
      {ErrorCode::HAVING_NOT_SUPPORTED,
       "HAVING is now supported. Update to the latest version if you see "
       "this error."},
      {ErrorCode::ORDER_BY_NOT_SUPPORTED,
       "ORDER BY is not supported in materialized views. Query the view "
       "and sort results in the outer query: "
       "SELECT * FROM view ORDER BY col;"},
      {ErrorCode::LIMIT_NOT_SUPPORTED,
       "LIMIT is not supported in materialized views. Apply LIMIT when "
       "querying the view: SELECT * FROM view LIMIT 10;"},
      {ErrorCode::WINDOW_FUNCTIONS_NOT_SUPPORTED,
       "Window functions (ROW_NUMBER, RANK, etc.) are not yet supported. "
       "Consider using GROUP BY with aggregates instead."},
      {ErrorCode::SUBQUERY_NOT_SUPPORTED,
       "Rewrite subqueries as JOINs or create intermediate views."},
      {ErrorCode::UNION_NOT_SUPPORTED,
       "Create separate views and use UNION when querying: "
       "SELECT * FROM view1 UNION SELECT * FROM view2;"},
      {ErrorCode::INTERSECT_NOT_SUPPORTED,
       "Use JOIN to find common rows: "
       "SELECT a.* FROM view1 a JOIN view2 b ON a.id = b.id;"},
      {ErrorCode::EXCEPT_NOT_SUPPORTED,
       "Use LEFT JOIN with NULL check: "
       "SELECT a.* FROM view1 a LEFT JOIN view2 b ON a.id = b.id "
       "WHERE b.id IS NULL;"},
      {ErrorCode::CTE_NOT_SUPPORTED,
       "WITH RECURSIVE is now supported for recursive queries. "
       "Non-recursive CTEs can be rewritten as views."},

      // Validation errors (E2xx)
      {ErrorCode::INVALID_IDENTIFIER,
       "Use only letters, numbers, and underscores. "
       "Must start with a letter or underscore."},
      {ErrorCode::PATH_TRAVERSAL,
       "Invalid file path detected. Use safe paths without '..' or "
       "special characters."},
      {ErrorCode::CIRCULAR_DEPENDENCY,
       "View depends on itself. Review view definitions to break the "
       "circular reference."},
      {ErrorCode::IDENTIFIER_TOO_LONG,
       "Maximum identifier length is 255 characters."},
      {ErrorCode::RESERVED_KEYWORD,
       "Use a different name or quote the identifier."},

      // Runtime errors (E3xx)
      {ErrorCode::VIEW_UPDATE_FAILED,
       "Check that source tables exist and have compatible schemas."},
      {ErrorCode::TYPE_MISMATCH,
       "Verify column types match between source and view definition."},
      {ErrorCode::NULL_CONSTRAINT_VIOLATION,
       "A NOT NULL constraint was violated. Check input data for NULLs."},
      {ErrorCode::CASCADE_UPDATE_FAILED,
       "Failed to propagate update to dependent views. Check view "
       "definitions for errors."},

      // Resource errors (E4xx)
      {ErrorCode::MEMORY_LIMIT_EXCEEDED,
       "Reduce the result set size with more selective WHERE clauses "
       "or use LIMIT when querying."},
      {ErrorCode::TOO_MANY_VIEWS,
       "Maximum of 1000 views per database. Drop unused views."},
      {ErrorCode::VIEW_TOO_LARGE,
       "View exceeds memory limit. Use more selective filters or "
       "partition into multiple views."},
      {ErrorCode::NESTING_DEPTH_EXCEEDED,
       "Maximum view nesting depth is 100. Flatten some intermediate "
       "views."},

      // Persistence errors (E5xx)
      {ErrorCode::FILE_READ_ERROR,
       "Check file permissions and path. Ensure file exists and is "
       "readable."},
      {ErrorCode::FILE_WRITE_ERROR,
       "Check disk space and file permissions. Ensure directory is "
       "writable."},
      {ErrorCode::SERIALIZATION_ERROR,
       "Failed to save view state. Try recreating the view."},
      {ErrorCode::DESERIALIZATION_ERROR,
       "Failed to load view state. File may be corrupted."},
  };

  auto it = workarounds.find(code);
  return it != workarounds.end() ? it->second : "";
}

// Format complete error message with SQL highlighting
inline std::string format_error(const ErrorInfo &info) {
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
  std::string doc_link =
      info.doc_link.empty() ? get_doc_link(info.code) : info.doc_link;
  ss << "Documentation: " << doc_link << "\n";

  return ss.str();
}

} // namespace dbsp_native

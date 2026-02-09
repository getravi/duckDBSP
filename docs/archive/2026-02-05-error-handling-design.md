# Error Handling and Validation Design

**Date**: 2026-02-05
**Status**: Approved
**Related TODO**: Task #9 - Improve error handling and input validation

## Overview

This design enhances duckDBSP's error handling with:
- Structured error code system for categorization and documentation
- Comprehensive SQL parser error detection for unsupported features
- SQL snippet highlighting with error position indicators
- ACID-compliant runtime error handling
- Detailed error documentation with workarounds

## Goals

1. **Better Developer Experience**: Clear, actionable error messages instead of generic failures
2. **Professional Error Catalog**: Structured error codes with comprehensive documentation
3. **ACID Compliance**: Fail-fast error handling to maintain database consistency
4. **Maintainability**: Easy to add new error codes and documentation as features evolve

## Error Code System Architecture

### Hierarchical Code Format

Error codes follow the format `DBSP-{Category}{Number}`:

**Error Categories:**
- **E1xx**: Parser errors (unsupported SQL features)
- **E2xx**: Validation errors (invalid identifiers, security issues)
- **E3xx**: Runtime errors (view update failures, type mismatches)
- **E4xx**: Resource errors (memory limits, circular dependencies)
- **E5xx**: Persistence errors (file I/O, serialization)

### Implementation

```cpp
// New file: dbsp_errors.hpp
namespace dbsp_native {

enum class ErrorCode {
    // Parser errors (E1xx)
    HAVING_NOT_SUPPORTED = 101,
    ORDER_BY_NOT_SUPPORTED = 102,
    LIMIT_NOT_SUPPORTED = 103,
    WINDOW_FUNCTIONS_NOT_SUPPORTED = 104,
    SUBQUERY_NOT_SUPPORTED = 105,
    UNION_NOT_SUPPORTED = 106,
    INTERSECT_NOT_SUPPORTED = 107,
    EXCEPT_NOT_SUPPORTED = 108,
    CTE_NOT_SUPPORTED = 109,

    // Validation errors (E2xx)
    INVALID_IDENTIFIER = 201,
    PATH_TRAVERSAL = 202,
    CIRCULAR_DEPENDENCY = 203,
    IDENTIFIER_TOO_LONG = 204,
    RESERVED_KEYWORD = 205,

    // Runtime errors (E3xx)
    VIEW_UPDATE_FAILED = 301,
    TYPE_MISMATCH = 302,
    NULL_CONSTRAINT_VIOLATION = 303,
    CASCADE_UPDATE_FAILED = 304,

    // Resource errors (E4xx)
    MEMORY_LIMIT_EXCEEDED = 401,
    TOO_MANY_VIEWS = 402,
    VIEW_TOO_LARGE = 403,
    NESTING_DEPTH_EXCEEDED = 404,

    // Persistence errors (E5xx)
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
```

### Error Code Mapping

Each error code maps to:
1. A detailed error page in `docs/errors/E{category}xx/DBSP-E{code}.md`
2. A workaround suggestion embedded in the error message
3. Links to relevant TODO items
4. Code examples showing the issue and alternatives

## SQL Parser Error Detection

### Detection Strategy

Add proactive detection for unsupported SQL features by inspecting DuckDB's parsed AST:

#### 1. Check for Unsupported Modifiers

```cpp
// In DBSPSqlParser::parse_select(), after checking DISTINCT (line ~159)
for (auto &modifier : select.modifiers) {
    switch (modifier->type) {
        case ResultModifierType::ORDER_MODIFIER:
            return make_error(ErrorCode::ORDER_BY_NOT_SUPPORTED,
                "ORDER BY clause detected", sql);
        case ResultModifierType::LIMIT_MODIFIER:
            return make_error(ErrorCode::LIMIT_NOT_SUPPORTED,
                "LIMIT clause detected", sql);
        case ResultModifierType::LIMIT_PERCENT_MODIFIER:
            return make_error(ErrorCode::LIMIT_NOT_SUPPORTED,
                "LIMIT PERCENT clause detected", sql);
        default:
            break;
    }
}
```

#### 2. Check for HAVING Clause

```cpp
// After parsing GROUP BY (line ~181)
if (!select.groups.grouping_sets.empty()) {
    if (select.groups.having) {
        return make_error(ErrorCode::HAVING_NOT_SUPPORTED,
            "HAVING clause in GROUP BY", sql);
    }
    parse_group_by(select.groups, result.view_def);
}
```

#### 3. Check for Window Functions

```cpp
// In parse_select_list() when processing FunctionExpression
if (func_expr.type == ExpressionType::WINDOW_AGGREGATE ||
    func_expr.type == ExpressionType::WINDOW_RANK ||
    func_expr.type == ExpressionType::WINDOW_NTILE) {
    return make_error(ErrorCode::WINDOW_FUNCTIONS_NOT_SUPPORTED,
        "Window function detected: " + func_name, sql);
}
```

#### 4. Check for Subqueries

```cpp
// In parse_from_clause()
case TableReferenceType::SUBQUERY:
    return make_error(ErrorCode::SUBQUERY_NOT_SUPPORTED,
        "Subquery in FROM clause", sql);

// In parse_where_clause() when encountering subquery expressions
if (expr->type == ExpressionType::SUBQUERY) {
    return make_error(ErrorCode::SUBQUERY_NOT_SUPPORTED,
        "Subquery in WHERE clause", sql);
}
```

#### 5. Check for Set Operations

```cpp
// In parse(), check node type
if (node->type == QueryNodeType::SET_OPERATION_NODE) {
    auto &set_op = node->Cast<SetOperationNode>();
    switch (set_op.setop_type) {
        case SetOperationType::UNION:
            return make_error(ErrorCode::UNION_NOT_SUPPORTED,
                "UNION operation", sql);
        case SetOperationType::INTERSECT:
            return make_error(ErrorCode::INTERSECT_NOT_SUPPORTED,
                "INTERSECT operation", sql);
        case SetOperationType::EXCEPT:
            return make_error(ErrorCode::EXCEPT_NOT_SUPPORTED,
                "EXCEPT operation", sql);
    }
}
```

### Helper Function

```cpp
ParseResult make_error(ErrorCode code, const std::string& detail,
                      const std::string& sql, size_t pos = 0) {
    ParseResult result;
    result.success = false;
    result.error_code = code;

    ErrorInfo info;
    info.code = code;
    info.message = detail;
    info.sql = sql;
    info.error_position = pos;
    info.workaround = get_workaround(code);
    info.doc_link = get_doc_link(code);

    result.error = format_error(info);
    return result;
}
```

## Error Message Formatting

### Formatter Implementation

```cpp
// In dbsp_errors.hpp
std::string format_error(const ErrorInfo& info) {
    std::stringstream ss;

    // Error code and message
    ss << "DBSP-E" << std::setfill('0') << std::setw(3)
       << static_cast<int>(info.code) << ": " << info.message << "\n\n";

    // Show SQL with pointer if position available
    if (!info.sql.empty()) {
        ss << "SQL:\n" << info.sql << "\n";
        if (info.error_position > 0) {
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
    int category = static_cast<int>(info.code) / 100;
    ss << "Documentation: docs/errors/E" << category << "xx/DBSP-E"
       << std::setfill('0') << std::setw(3) << static_cast<int>(info.code)
       << ".md\n";

    return ss.str();
}

std::string get_workaround(ErrorCode code) {
    static const std::unordered_map<ErrorCode, std::string> workarounds = {
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
        // ... add all error codes
    };

    auto it = workarounds.find(code);
    return it != workarounds.end() ? it->second : "";
}

std::string get_doc_link(ErrorCode code) {
    int category = static_cast<int>(code) / 100;
    std::stringstream ss;
    ss << "docs/errors/E" << category << "xx/DBSP-E"
       << std::setfill('0') << std::setw(3) << static_cast<int>(code)
       << ".md";
    return ss.str();
}
```

### Example Error Output

```
DBSP-E102: ORDER BY clause is not supported

SQL:
SELECT * FROM orders ORDER BY created_at DESC
                      ^

Workaround:
Query the view using dbsp_query() and sort results client-side.
ORDER BY support is tracked in TODO #4.

Documentation: docs/errors/E1xx/DBSP-E102.md
```

## Documentation Structure

### Directory Layout

```
docs/errors/
├── README.md                    # Error catalog index
├── E1xx/                        # Parser errors
│   ├── DBSP-E101.md            # HAVING not supported
│   ├── DBSP-E102.md            # ORDER BY not supported
│   ├── DBSP-E103.md            # LIMIT not supported
│   ├── DBSP-E104.md            # Window functions
│   ├── DBSP-E105.md            # Subqueries
│   ├── DBSP-E106.md            # UNION
│   ├── DBSP-E107.md            # INTERSECT
│   └── DBSP-E108.md            # EXCEPT
├── E2xx/                        # Validation errors
│   ├── DBSP-E201.md            # Invalid identifier
│   ├── DBSP-E202.md            # Path traversal
│   ├── DBSP-E203.md            # Circular dependency
│   ├── DBSP-E204.md            # Identifier too long
│   └── DBSP-E205.md            # Reserved keyword
├── E3xx/                        # Runtime errors
│   ├── DBSP-E301.md            # View update failed
│   ├── DBSP-E302.md            # Type mismatch
│   └── DBSP-E303.md            # NULL constraint violation
├── E4xx/                        # Resource errors
│   ├── DBSP-E401.md            # Memory limit exceeded
│   └── DBSP-E402.md            # Too many views
├── E5xx/                        # Persistence errors
│   ├── DBSP-E501.md            # File read error
│   └── DBSP-E502.md            # File write error
└── templates/
    └── error-template.md        # Template for new errors
```

### Error Document Template

```markdown
# DBSP-E{code}: {Title}

**Category**: {Parser/Validation/Runtime/Resource/Persistence}
**Severity**: {Error/Warning}
**Since**: Version {X.Y}

## Description

{Detailed explanation of what this error means and when it occurs}

## Common Causes

- Cause 1 with explanation
- Cause 2 with explanation
- Cause 3 with explanation

## Example

### This will fail:
```sql
{SQL that triggers the error}
```

### Error message:
```
{Actual error output}
```

## Workaround

{Step-by-step workaround with code examples}

### Example solution:
```sql
{Alternative SQL that works}
```

## Related

- TODO #{number} - {description}
- Related error codes: DBSP-E{xxx}, DBSP-E{yyy}
- API documentation: {link}
- Architecture documentation: {link}

## Roadmap

This feature is planned for version {X.Y}. Track progress in TODO #{number}.

## Technical Details

{Optional: Deep dive into why this limitation exists, what would be needed
to implement it, challenges with incremental computation, etc.}
```

### Error Catalog Index (docs/errors/README.md)

```markdown
# duckDBSP Error Catalog

Complete reference for all error codes in duckDBSP.

## Error Categories

- [E1xx: Parser Errors](#e1xx-parser-errors) - Unsupported SQL features
- [E2xx: Validation Errors](#e2xx-validation-errors) - Invalid input
- [E3xx: Runtime Errors](#e3xx-runtime-errors) - Execution failures
- [E4xx: Resource Errors](#e4xx-resource-errors) - Limits exceeded
- [E5xx: Persistence Errors](#e5xx-persistence-errors) - I/O failures

## E1xx: Parser Errors

| Code | Description | Workaround Available | TODO |
|------|-------------|---------------------|------|
| E101 | HAVING clause not supported | Yes | #3 |
| E102 | ORDER BY not supported | Yes | #4 |
| E103 | LIMIT not supported | Yes | #4 |
| E104 | Window functions not supported | No | #8 |
| E105 | Subqueries not supported | Yes | #6 |
| E106 | UNION not supported | Yes | #7 |
| E107 | INTERSECT not supported | Yes | #7 |
| E108 | EXCEPT not supported | Yes | #7 |

## E2xx: Validation Errors

{...}

## Quick Reference

### Most Common Errors

1. **E102: ORDER BY not supported** - Sort results after querying
2. **E101: HAVING not supported** - Use nested views with WHERE
3. **E105: Subqueries not supported** - Rewrite with JOINs or intermediate views

### Getting Help

- Read the specific error documentation page
- Check TODO.md for roadmap and planned features
- Review docs/API.md for supported features
- See examples/ directory for working code patterns
```

## ACID-Compliant Runtime Error Handling

### Two-Phase Update Strategy

To maintain ACID properties, implement validation-before-application:

```cpp
// In dbsp_cdc.hpp CDCManager class

bool propagate_changes(const std::string& source_name,
                       const DuckDBZSet& changes) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Phase 1: VALIDATION - Check all views can accept changes
    // If ANY view fails validation, abort entire operation (Atomicity)
    auto update_order = dep_graph_.topological_order(source_name);
    std::vector<std::string> views_to_update;

    for (const auto& view_name : update_order) {
        auto it = views_.find(view_name);
        if (it == views_.end()) continue;

        // Validate changes are compatible with view
        auto validation = it->second->validate_changes(changes);
        if (!validation.success) {
            // FAIL FAST - Maintain consistency
            last_error_ = format_runtime_error(
                ErrorCode::VIEW_UPDATE_FAILED,
                view_name,
                validation.error_message,
                source_name
            );
            return false;
        }
        views_to_update.push_back(view_name);
    }

    // Phase 2: APPLICATION - Only if all validations passed
    // Now we know all updates will succeed
    for (const auto& view_name : views_to_update) {
        views_[view_name]->apply_changes(source_name, changes);
    }

    return true;
}

std::string format_runtime_error(ErrorCode code,
                                 const std::string& view_name,
                                 const std::string& details,
                                 const std::string& source_name) {
    ErrorInfo info;
    info.code = code;
    info.message = details;
    info.context = "View: " + view_name + ", Source: " + source_name;
    info.workaround = get_workaround(code);
    info.doc_link = get_doc_link(code);

    return format_error(info);
}
```

### Validation Interface

Add validation method to view base class:

```cpp
// In dbsp_materialized_view.hpp

struct ValidationResult {
    bool success = true;
    std::string error_message;
    ErrorCode error_code;
};

class MaterializedView {
public:
    // ... existing methods ...

    // New: Validate changes before applying
    virtual ValidationResult validate_changes(const DuckDBZSet& changes) {
        // Default: accept all changes
        // Subclasses can override for type checking, constraint validation, etc.
        return ValidationResult{true, "", ErrorCode::VIEW_UPDATE_FAILED};
    }
};
```

### Error Recovery

Since we validate before applying:
- **No rollback needed** - We don't apply if validation fails
- **Atomicity guaranteed** - Either all views update or none do
- **Consistency maintained** - Database always in valid state
- **Clear error reporting** - User knows exactly which view failed and why

## Testing Strategy

### Test Files Structure

```
test/
├── unit/
│   ├── test_parser_errors.cpp          # All E1xx errors
│   ├── test_validation_errors.cpp      # All E2xx errors
│   └── test_error_formatting.cpp       # Format validation
├── integration/
│   ├── test_runtime_errors.cpp         # E3xx errors
│   ├── test_acid_compliance.cpp        # ACID guarantees
│   └── test_error_documentation.cpp    # Doc coverage
└── scripts/
    └── verify_error_docs.sh            # CI check
```

### Parser Error Tests

```cpp
// test/unit/test_parser_errors.cpp
#include "catch.hpp"
#include "../dbsp_sql_parser.hpp"
#include "../dbsp_errors.hpp"

using namespace dbsp_native;

TEST_CASE("Parser errors - E1xx codes", "[parser][errors]") {
    DBSPSqlParser parser;

    SECTION("E101: HAVING clause") {
        auto result = parser.parse(
            "SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 10",
            "test_view");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::HAVING_NOT_SUPPORTED);
        REQUIRE(result.error.find("DBSP-E101") != std::string::npos);
        REQUIRE(result.error.find("Workaround") != std::string::npos);
        REQUIRE(result.error.find("nested view") != std::string::npos);
        REQUIRE(result.error.find("docs/errors/E1xx/DBSP-E101.md") != std::string::npos);
    }

    SECTION("E102: ORDER BY clause") {
        auto result = parser.parse(
            "SELECT * FROM orders ORDER BY created_at DESC",
            "sorted_orders");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::ORDER_BY_NOT_SUPPORTED);
        REQUIRE(result.error.find("DBSP-E102") != std::string::npos);
        REQUIRE(result.error.find("sort results client-side") != std::string::npos);
    }

    SECTION("E103: LIMIT clause") {
        auto result = parser.parse(
            "SELECT * FROM orders LIMIT 100",
            "limited_orders");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::LIMIT_NOT_SUPPORTED);
        REQUIRE(result.error.find("DBSP-E103") != std::string::npos);
    }

    SECTION("E104: Window functions") {
        auto result = parser.parse(
            "SELECT *, ROW_NUMBER() OVER (PARTITION BY dept ORDER BY salary) FROM emp",
            "ranked_emp");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::WINDOW_FUNCTIONS_NOT_SUPPORTED);
    }

    SECTION("E105: Subquery in WHERE") {
        auto result = parser.parse(
            "SELECT * FROM orders WHERE customer_id IN (SELECT id FROM customers)",
            "filtered_orders");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::SUBQUERY_NOT_SUPPORTED);
    }

    SECTION("E106: UNION operation") {
        auto result = parser.parse(
            "SELECT * FROM orders UNION SELECT * FROM archived_orders",
            "all_orders");

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_code == ErrorCode::UNION_NOT_SUPPORTED);
    }
}

TEST_CASE("Error message format validation", "[errors][format]") {
    SECTION("Contains all required elements") {
        ErrorInfo info;
        info.code = ErrorCode::HAVING_NOT_SUPPORTED;
        info.message = "HAVING clause detected";
        info.sql = "SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 10";
        info.error_position = 46; // Position of "HAVING"
        info.workaround = "Use nested views";

        std::string formatted = format_error(info);

        REQUIRE(formatted.find("DBSP-E101") != std::string::npos);
        REQUIRE(formatted.find("SQL:") != std::string::npos);
        REQUIRE(formatted.find("Workaround:") != std::string::npos);
        REQUIRE(formatted.find("Documentation:") != std::string::npos);
        REQUIRE(formatted.find("^") != std::string::npos); // Position marker
    }
}
```

### ACID Compliance Tests

```cpp
// test/integration/test_acid_compliance.cpp
TEST_CASE("ACID compliance - Atomicity", "[acid][runtime]") {
    // Create cascading views: table -> view1 -> view2 -> view3
    // Make view2 fail validation
    // Verify view1 and view3 are NOT updated (atomicity)

    SECTION("Cascade update fails atomically") {
        // Setup: Create chain of views
        // ... setup code ...

        // Inject validation failure in view2
        // ... injection code ...

        // Attempt to update source table
        auto result = manager.sync_table(context, "source_table");

        // Should fail
        REQUIRE_FALSE(result);

        // Verify NO partial updates occurred
        REQUIRE(view1.get_result().size() == original_size);
        REQUIRE(view3.get_result().size() == original_size);

        // Verify error message is informative
        REQUIRE(manager.last_error().find("DBSP-E301") != std::string::npos);
        REQUIRE(manager.last_error().find("view2") != std::string::npos);
    }
}
```

### Documentation Coverage Test

```bash
#!/bin/bash
# test/scripts/verify_error_docs.sh

# Extract all error codes from dbsp_errors.hpp
error_codes=$(grep -E "= [0-9]{3}" dbsp_errors.hpp | awk '{print $1}')

missing=0
for code in $error_codes; do
    # Calculate category
    category=$((code / 100))

    # Check if doc file exists
    doc_file="docs/errors/E${category}xx/DBSP-E$(printf '%03d' $code).md"

    if [ ! -f "$doc_file" ]; then
        echo "Missing documentation: $doc_file"
        ((missing++))
    fi
done

if [ $missing -gt 0 ]; then
    echo "Error: $missing error codes are missing documentation"
    exit 1
fi

echo "All error codes have documentation"
exit 0
```

## Implementation Plan

### Phase 1: Foundation (High Priority)
1. Create `dbsp_errors.hpp` with ErrorCode enum and ErrorInfo struct
2. Implement `format_error()` function with SQL highlighting
3. Create `docs/errors/` directory structure with template
4. Add error formatting tests

### Phase 2: Parser Errors (High Priority)
5. Update `DBSPSqlParser` to detect all E1xx unsupported features
6. Add `make_error()` helper function to parser
7. Update `ParseResult` to include `error_code` field
8. Write documentation for all E1xx codes
9. Add parser error tests

### Phase 3: Validation Errors (Medium Priority)
10. Enhance identifier validation with detailed error codes
11. Update path validation to use error codes
12. Add more validation edge cases (reserved keywords, length limits)
13. Write documentation for E2xx codes
14. Add validation error tests

### Phase 4: Runtime Errors (High Priority - ACID)
15. Add `validate_changes()` method to MaterializedView base class
16. Implement two-phase update in `CDCManager::propagate_changes()`
17. Add runtime error formatting
18. Write documentation for E3xx codes
19. Add ACID compliance tests

### Phase 5: Documentation & CI (Medium Priority)
20. Write docs/errors/README.md catalog index
21. Create error documentation for all initial codes
22. Add `verify_error_docs.sh` script
23. Integrate script into CI pipeline (when Task #15 is done)

## Benefits

1. **Developer Experience**: Clear, actionable errors instead of generic failures
2. **Debuggability**: SQL highlighting shows exactly where problems occur
3. **Discoverability**: Error codes link to comprehensive documentation
4. **Maintainability**: Structured system makes adding new errors easy
5. **Reliability**: ACID-compliant error handling prevents data corruption
6. **Professionalism**: Error catalog demonstrates production-quality software

## Future Enhancements

1. **Error Recovery Hints**: Suggest SQL rewrites automatically
2. **Error Analytics**: Track most common errors to prioritize feature development
3. **Interactive Error Explorer**: Web UI to browse error catalog
4. **Auto-fix Suggestions**: "Did you mean?" suggestions for common typos
5. **Error Localization**: Multi-language error messages

## References

- TODO.md Task #9: Improve error handling and input validation
- TODO.md Task #3: HAVING clause
- TODO.md Task #4: ORDER BY and LIMIT
- TODO.md Task #6: Subquery support
- TODO.md Task #7: Set operations
- TODO.md Task #8: Window functions
- docs/API.md: Current API documentation
- docs/ARCHITECTURE.md: System architecture

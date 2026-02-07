// DBSP SQL Parser
// Parses SQL view definitions and creates appropriate DBSP views

#pragma once

#include "dbsp_duckdb_types.hpp"
#include "dbsp_errors.hpp"
#include "dbsp_recursive.hpp"

#include "duckdb.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dbsp_native {

// Parsed view definition
struct ParsedViewDef {
  enum class ViewType {
    FILTER,
    PROJECT,
    AGGREGATE,
    JOIN,
    DISTINCT,
    RECURSIVE,
    UNKNOWN
  };

  ViewType type = ViewType::UNKNOWN;
  std::string view_name;
  std::string sql;

  // Source tables
  std::vector<std::string> source_tables;

  // For filter views
  struct FilterInfo {
    size_t column_idx;
    std::string column_name;
    std::string op; // =, >, <, >=, <=, !=
    duckdb::Value value;
  };
  std::vector<FilterInfo> filters;

  // For projection views
  std::vector<size_t> project_columns;
  std::vector<std::string> project_column_names;
  // Qualified column references (table_name/alias, column_name)
  std::vector<std::pair<std::string, std::string>> project_column_refs;
  // Map of alias -> table_name
  std::unordered_map<std::string, std::string> table_aliases;
  bool select_all = false;

  // For aggregate views
  struct AggInfo {
    std::string function; // SUM, COUNT, AVG, MIN, MAX
    std::string alias;    // e.g., "revenue" from SUM(...) as revenue
    size_t value_column_idx;
    std::string value_column_name;
    // For expression-based aggregates like SUM(quantity * price)
    // Stores column indices and operator for binary expressions
    bool is_expression = false;
    size_t left_col_idx = 0;
    size_t right_col_idx = 0;
    std::string left_col_name;
    std::string right_col_name;
    std::string op; // "*", "+", "-", "/"
  };
  std::vector<AggInfo> aggregates;
  std::vector<size_t> group_by_columns;
  std::vector<std::string> group_by_names;

  // For join views - supports multiple column pairs for compound ON clauses
  struct JoinInfo {
    std::string left_table;
    std::string right_table;
    // Multiple column pairs for compound ON clauses: ON a.x = b.x AND a.y = b.y
    std::vector<std::pair<std::string, std::string>>
        column_pairs; // (left_col, right_col)
    std::vector<std::pair<size_t, size_t>> column_idx_pairs; // resolved indices
  };
  std::optional<JoinInfo> join_info;

  // For recursive CTE views (WITH RECURSIVE)
  struct RecursiveCTEInfo {
    std::string cte_name;      // Name of the recursive CTE
    bool union_all = false;    // UNION vs UNION ALL
    std::string source_table;  // Base table referenced
    std::string anchor_sql;    // SQL for anchor (non-recursive) query
    std::string recursive_sql; // SQL for recursive step (references cte_name)
  };
  std::optional<RecursiveCTEInfo> recursive_cte;

  // Is DISTINCT present
  bool is_distinct = false;

  // For HAVING clause (post-aggregation filter)
  struct HavingFilter {
    enum RefType { GROUP_COL, AGGREGATE_RESULT };
    RefType ref_type = AGGREGATE_RESULT;
    std::string column_name;  // Column name (for group cols)
    std::string agg_function; // Aggregate function name (for agg refs)
    std::string
        agg_col_name; // Column in aggregate (for agg refs like SUM(amount))
    std::string op;   // =, >, <, >=, <=, !=
    duckdb::Value value;
  };
  std::vector<HavingFilter> having_filters;
};

// SQL Parser for DBSP views
class DBSPSqlParser {
public:
  struct ParseResult {
    bool success = false;
    std::string error;
    ErrorCode error_code =
        ErrorCode::HAVING_NOT_SUPPORTED; // Default, will be overwritten
    ParsedViewDef view_def;
  };

  // Parse a CREATE MATERIALIZED VIEW statement
  // Supports:
  //   CREATE MATERIALIZED VIEW name AS SELECT ...
  //   CREATE VIEW name AS SELECT ... (treated same as materialized)
  ParseResult parse(const std::string &sql, const std::string &view_name = "") {
    ParseResult result;
    result.view_def.sql = sql;

    try {
      duckdb::Parser parser;
      parser.ParseQuery(sql);

      if (parser.statements.empty()) {
        result.error = "No SQL statement found";
        return result;
      }

      auto &stmt = parser.statements[0];

      // Check if it's a SELECT statement (for simple queries)
      if (stmt->type == duckdb::StatementType::SELECT_STATEMENT) {
        auto &select = stmt->template Cast<duckdb::SelectStatement>();
        return parse_select(select,
                            view_name.empty() ? "unnamed_view" : view_name);
      }

      // Check for CREATE VIEW
      if (stmt->type == duckdb::StatementType::CREATE_STATEMENT) {
        // Extract the view info from CREATE statement
        result.error =
            "Use SELECT syntax directly, CREATE VIEW parsing coming soon";
        return result;
      }

      result.error = "Expected SELECT statement";
      return result;

    } catch (const std::exception &e) {
      result.error = std::string("Parse error: ") + e.what();
      return result;
    }
  }

  // Parse a simple SELECT statement
  ParseResult parse_select(const duckdb::SelectStatement &stmt,
                           const std::string &view_name) {
    ParseResult result;
    result.view_def.view_name = view_name;

    auto *node = stmt.node.get();

    // Check for recursive CTE (WITH RECURSIVE)
    if (node->type == duckdb::QueryNodeType::RECURSIVE_CTE_NODE) {
      auto &recursive = node->template Cast<duckdb::RecursiveCTENode>();
      ParsedViewDef::RecursiveCTEInfo cte_info;
      cte_info.cte_name = recursive.ctename;
      cte_info.union_all = recursive.union_all;
      // Will determine source table from left (anchor) query
      result.view_def.recursive_cte = cte_info;
      result.view_def.view_name = view_name;
      result.view_def.sql = stmt.ToString();
      result.success = true;
      determine_view_type(result.view_def);
      return result;
    }

    // Check for recursive CTEs in cte_map (before any other checks)
    // This works regardless of the outer query node type
    for (auto &cte_entry : node->cte_map.map) {
      auto &cte_info = cte_entry.second;
      if (cte_info && cte_info->query && cte_info->query->node) {
        auto *cte_node = cte_info->query->node.get();
        if (cte_node->type == duckdb::QueryNodeType::RECURSIVE_CTE_NODE) {
          auto &recursive = cte_node->template Cast<duckdb::RecursiveCTENode>();
          ParsedViewDef::RecursiveCTEInfo rec_info;
          rec_info.cte_name = recursive.ctename;
          rec_info.union_all = recursive.union_all;
          // Extract anchor and recursive SQL from left/right sub-queries
          if (recursive.left) {
            rec_info.anchor_sql = recursive.left->ToString();
          }
          if (recursive.right) {
            rec_info.recursive_sql = recursive.right->ToString();
          }
          result.view_def.recursive_cte = rec_info;
          result.view_def.view_name = view_name;
          result.view_def.sql = stmt.ToString();
          result.success = true;
          determine_view_type(result.view_def);
          return result;
        }
      }
    }

    // Check for set operations (UNION, INTERSECT, EXCEPT)
    if (node->type == duckdb::QueryNodeType::SET_OPERATION_NODE) {
      auto &set_op = node->template Cast<duckdb::SetOperationNode>();
      switch (set_op.setop_type) {
      case duckdb::SetOperationType::UNION:
      case duckdb::SetOperationType::UNION_BY_NAME:
        return make_error(ErrorCode::UNION_NOT_SUPPORTED, "UNION operation",
                          result.view_def.sql);
      case duckdb::SetOperationType::INTERSECT:
        return make_error(ErrorCode::INTERSECT_NOT_SUPPORTED,
                          "INTERSECT operation", result.view_def.sql);
      case duckdb::SetOperationType::EXCEPT:
        return make_error(ErrorCode::EXCEPT_NOT_SUPPORTED, "EXCEPT operation",
                          result.view_def.sql);
      default:
        result.error = "Unsupported set operation";
        return result;
      }
    }

    if (node->type != duckdb::QueryNodeType::SELECT_NODE) {
      result.error = "Complex queries not yet supported";
      return result;
    }

    auto &select = node->template Cast<duckdb::SelectNode>();

    // Check for unsupported modifiers
    for (auto &modifier : select.modifiers) {
      switch (modifier->type) {
      case duckdb::ResultModifierType::DISTINCT_MODIFIER:
        result.view_def.is_distinct = true;
        break;
      case duckdb::ResultModifierType::ORDER_MODIFIER:
        return make_error(ErrorCode::ORDER_BY_NOT_SUPPORTED,
                          "ORDER BY clause detected", result.view_def.sql);
      case duckdb::ResultModifierType::LIMIT_MODIFIER:
      case duckdb::ResultModifierType::LIMIT_PERCENT_MODIFIER:
        return make_error(ErrorCode::LIMIT_NOT_SUPPORTED,
                          "LIMIT clause detected", result.view_def.sql);
      default:
        // Other modifiers not yet encountered
        break;
      }
    }

    // Parse FROM clause
    if (!select.from_table) {
      result.error = "FROM clause required";
      return result;
    }

    // Check for subquery in FROM clause first
    if (select.from_table &&
        select.from_table->type == duckdb::TableReferenceType::SUBQUERY) {
      return make_error(ErrorCode::SUBQUERY_NOT_SUPPORTED,
                        "Subquery in FROM clause (derived table)",
                        result.view_def.sql);
    }

    if (!parse_from_clause(select.from_table.get(), result.view_def)) {
      result.error = "FROM clause required";
      return result;
    }

    // Check for window functions in SELECT list
    for (size_t i = 0; i < select.select_list.size(); i++) {
      auto &expr = select.select_list[i];
      if (expr->type == duckdb::ExpressionType::WINDOW_AGGREGATE ||
          expr->type == duckdb::ExpressionType::WINDOW_RANK_DENSE ||
          expr->type == duckdb::ExpressionType::WINDOW_RANK ||
          expr->type == duckdb::ExpressionType::WINDOW_PERCENT_RANK ||
          expr->type == duckdb::ExpressionType::WINDOW_ROW_NUMBER ||
          expr->type == duckdb::ExpressionType::WINDOW_FIRST_VALUE ||
          expr->type == duckdb::ExpressionType::WINDOW_LAST_VALUE ||
          expr->type == duckdb::ExpressionType::WINDOW_NTILE ||
          expr->type == duckdb::ExpressionType::WINDOW_LEAD ||
          expr->type == duckdb::ExpressionType::WINDOW_LAG) {
        return make_error(ErrorCode::WINDOW_FUNCTIONS_NOT_SUPPORTED,
                          "Window function detected in SELECT list",
                          result.view_def.sql);
      }
    }

    // Parse SELECT columns
    parse_select_list(select.select_list, result.view_def);

    // Parse WHERE clause
    if (select.where_clause) {
      if (has_subquery_expression(select.where_clause.get())) {
        return make_error(ErrorCode::SUBQUERY_NOT_SUPPORTED,
                          "Subquery in WHERE clause", result.view_def.sql);
      }
      parse_where_clause(select.where_clause.get(), result.view_def);
    }

    // Parse GROUP BY
    if (!select.groups.grouping_sets.empty()) {
      parse_group_by(select.groups, result.view_def);

      // Parse HAVING clause (post-aggregation filter)
      if (select.having) {
        parse_having_clause(select.having.get(), result.view_def);
      }
    }

    // Determine view type
    determine_view_type(result.view_def);

    result.success = true;
    return result;
  }

private:
  bool parse_from_clause(duckdb::TableRef *ref, ParsedViewDef &def) {
    if (!ref)
      return false;

    switch (ref->type) {
    case duckdb::TableReferenceType::BASE_TABLE: {
      auto &base = ref->template Cast<duckdb::BaseTableRef>();
      def.source_tables.push_back(base.table_name);
      // Capture alias mapping
      if (!base.alias.empty()) {
        def.table_aliases[base.alias] = base.table_name;
      } else {
        def.table_aliases[base.table_name] = base.table_name;
      }
      return true;
    }

    case duckdb::TableReferenceType::SUBQUERY:
      // Subquery in FROM clause (derived table) - not supported
      return false;

    case duckdb::TableReferenceType::JOIN: {
      auto &join = ref->template Cast<duckdb::JoinRef>();

      // Parse left and right tables
      if (!parse_from_clause(join.left.get(), def))
        return false;
      if (!parse_from_clause(join.right.get(), def))
        return false;

      // Parse join condition
      if (join.condition && def.source_tables.size() >= 2) {
        ParsedViewDef::JoinInfo info;
        info.left_table = def.source_tables[def.source_tables.size() - 2];
        info.right_table = def.source_tables[def.source_tables.size() - 1];

        parse_join_condition(join.condition.get(), info);
        def.join_info = info;
      }
      return true;
    }

    default:
      return false;
    }
  }

  void parse_join_condition(duckdb::ParsedExpression *expr,
                            ParsedViewDef::JoinInfo &info) {
    if (!expr)
      return;

    // Handle compound conditions: ON a.x = b.x AND a.y = b.y
    if (expr->type == duckdb::ExpressionType::CONJUNCTION_AND) {
      auto &conj = expr->template Cast<duckdb::ConjunctionExpression>();
      for (auto &child : conj.children) {
        parse_join_condition(child.get(), info);
      }
    } else if (expr->type == duckdb::ExpressionType::COMPARE_EQUAL) {
      auto &cmp = expr->template Cast<duckdb::ComparisonExpression>();

      // Extract column references from both sides
      if (cmp.left->type == duckdb::ExpressionType::COLUMN_REF &&
          cmp.right->type == duckdb::ExpressionType::COLUMN_REF) {
        auto &left_col = cmp.left->template Cast<duckdb::ColumnRefExpression>();
        auto &right_col =
            cmp.right->template Cast<duckdb::ColumnRefExpression>();

        // Add column pair to the list
        info.column_pairs.push_back(
            {left_col.GetColumnName(), right_col.GetColumnName()});
      }
    }
  }

  template <typename VectorType>
  void parse_select_list(VectorType &select_list, ParsedViewDef &def) {
    for (size_t i = 0; i < select_list.size(); i++) {
      auto &expr = select_list[i];
      switch (expr->type) {
      case duckdb::ExpressionType::STAR: {
        def.select_all = true;
        break;
      }

      case duckdb::ExpressionType::COLUMN_REF: {
        auto &col = expr->template Cast<duckdb::ColumnRefExpression>();
        def.project_column_names.push_back(col.GetColumnName());
        def.project_columns.push_back(i);
        // Capture qualified reference (table name only if qualified)
        std::string table_name = col.IsQualified() ? col.GetTableName() : "";
        def.project_column_refs.push_back(
            {table_name, col.GetColumnName()});
        break;
      }

      case duckdb::ExpressionType::FUNCTION: {
        auto &func = expr->template Cast<duckdb::FunctionExpression>();
        std::string func_name = duckdb::StringUtil::Upper(func.function_name);
        // DuckDB translates COUNT(*) to count_star with no children
        if (func_name == "COUNT_STAR") {
          func_name = "COUNT";
        }

        if (func_name == "SUM" || func_name == "COUNT" || func_name == "AVG" ||
            func_name == "MIN" || func_name == "MAX") {
          ParsedViewDef::AggInfo agg;
          agg.function = func_name;

          // Get the column/expression being aggregated
          if (!func.children.empty()) {
            auto &child = func.children[0];
            if (child->type == duckdb::ExpressionType::COLUMN_REF) {
              auto &col = child->template Cast<duckdb::ColumnRefExpression>();
              agg.value_column_name = col.GetColumnName();
            } else if (child->type == duckdb::ExpressionType::FUNCTION &&
                       child->GetExpressionClass() ==
                           duckdb::ExpressionClass::FUNCTION) {
              // Binary expression like quantity * price parsed as function(*,
              // quantity, price)
              auto &child_func =
                  child->template Cast<duckdb::FunctionExpression>();
              std::string op_name = child_func.function_name;
              if ((op_name == "*" || op_name == "+" || op_name == "-" ||
                   op_name == "/") &&
                  child_func.children.size() == 2 &&
                  child_func.children[0]->type ==
                      duckdb::ExpressionType::COLUMN_REF &&
                  child_func.children[1]->type ==
                      duckdb::ExpressionType::COLUMN_REF) {
                auto &left_col =
                    child_func.children[0]
                        ->template Cast<duckdb::ColumnRefExpression>();
                auto &right_col =
                    child_func.children[1]
                        ->template Cast<duckdb::ColumnRefExpression>();
                agg.is_expression = true;
                agg.left_col_name = left_col.GetColumnName();
                agg.right_col_name = right_col.GetColumnName();
                agg.op = op_name;
              }
            }
          }
          // Capture alias (e.g., SUM(x) as total)
          if (!expr->alias.empty()) {
            agg.alias = expr->alias;
          }
          def.aggregates.push_back(agg);
        }
        break;
      }

      default:
        break;
      }
    }
  }

  void parse_where_clause(duckdb::ParsedExpression *expr, ParsedViewDef &def) {
    if (!expr)
      return;

    switch (expr->type) {
    case duckdb::ExpressionType::COMPARE_EQUAL:
    case duckdb::ExpressionType::COMPARE_GREATERTHAN:
    case duckdb::ExpressionType::COMPARE_LESSTHAN:
    case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
    case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
    case duckdb::ExpressionType::COMPARE_NOTEQUAL: {
      auto &cmp = expr->template Cast<duckdb::ComparisonExpression>();
      ParsedViewDef::FilterInfo filter;

      // Get operator
      filter.op = get_comparison_op(expr->type);

      // Get column (usually left side)
      if (cmp.left->type == duckdb::ExpressionType::COLUMN_REF) {
        auto &col = cmp.left->template Cast<duckdb::ColumnRefExpression>();
        filter.column_name = col.GetColumnName();
      }

      // Get value (usually right side)
      if (cmp.right->type == duckdb::ExpressionType::VALUE_CONSTANT) {
        auto &val = cmp.right->template Cast<duckdb::ConstantExpression>();
        filter.value = val.value;
      }

      def.filters.push_back(filter);
      break;
    }

    case duckdb::ExpressionType::CONJUNCTION_AND: {
      auto &conj = expr->template Cast<duckdb::ConjunctionExpression>();
      for (auto &child : conj.children) {
        parse_where_clause(child.get(), def);
      }
      break;
    }

    default:
      break;
    }
  }

  void parse_group_by(const duckdb::GroupByNode &groups, ParsedViewDef &def) {
    for (auto &group_set : groups.grouping_sets) {
      for (auto idx : group_set) {
        def.group_by_columns.push_back(idx);
      }
    }
  }

  void parse_having_clause(duckdb::ParsedExpression *expr, ParsedViewDef &def) {
    if (!expr)
      return;

    switch (expr->type) {
    case duckdb::ExpressionType::COMPARE_EQUAL:
    case duckdb::ExpressionType::COMPARE_GREATERTHAN:
    case duckdb::ExpressionType::COMPARE_LESSTHAN:
    case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
    case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
    case duckdb::ExpressionType::COMPARE_NOTEQUAL: {
      auto &cmp = expr->template Cast<duckdb::ComparisonExpression>();
      ParsedViewDef::HavingFilter filter;
      filter.op = get_comparison_op(expr->type);

      // Left side: could be aggregate function or group column
      if (cmp.left->type == duckdb::ExpressionType::FUNCTION) {
        auto &func = cmp.left->template Cast<duckdb::FunctionExpression>();
        std::string func_name = duckdb::StringUtil::Upper(func.function_name);
        if (func_name == "COUNT_STAR")
          func_name = "COUNT";

        filter.ref_type = ParsedViewDef::HavingFilter::AGGREGATE_RESULT;
        filter.agg_function = func_name;
        if (!func.children.empty() &&
            func.children[0]->type == duckdb::ExpressionType::COLUMN_REF) {
          auto &col =
              func.children[0]->template Cast<duckdb::ColumnRefExpression>();
          filter.agg_col_name = col.GetColumnName();
        }
      } else if (cmp.left->type == duckdb::ExpressionType::COLUMN_REF) {
        auto &col = cmp.left->template Cast<duckdb::ColumnRefExpression>();
        filter.ref_type = ParsedViewDef::HavingFilter::GROUP_COL;
        filter.column_name = col.GetColumnName();
      }

      // Right side: constant value
      if (cmp.right->type == duckdb::ExpressionType::VALUE_CONSTANT) {
        auto &val = cmp.right->template Cast<duckdb::ConstantExpression>();
        filter.value = val.value;
      }

      def.having_filters.push_back(filter);
      break;
    }

    case duckdb::ExpressionType::CONJUNCTION_AND: {
      auto &conj = expr->template Cast<duckdb::ConjunctionExpression>();
      for (auto &child : conj.children) {
        parse_having_clause(child.get(), def);
      }
      break;
    }

    default:
      break;
    }
  }

  std::string get_comparison_op(duckdb::ExpressionType type) {
    switch (type) {
    case duckdb::ExpressionType::COMPARE_EQUAL:
      return "=";
    case duckdb::ExpressionType::COMPARE_GREATERTHAN:
      return ">";
    case duckdb::ExpressionType::COMPARE_LESSTHAN:
      return "<";
    case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
      return ">=";
    case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
      return "<=";
    case duckdb::ExpressionType::COMPARE_NOTEQUAL:
      return "!=";
    default:
      return "=";
    }
  }

  void determine_view_type(ParsedViewDef &def) {
    // Priority: RECURSIVE > JOIN > AGGREGATE > DISTINCT > FILTER > PROJECT
    if (def.recursive_cte.has_value()) {
      def.type = ParsedViewDef::ViewType::RECURSIVE;
    } else if (def.join_info.has_value()) {
      def.type = ParsedViewDef::ViewType::JOIN;
    } else if (!def.aggregates.empty()) {
      def.type = ParsedViewDef::ViewType::AGGREGATE;
    } else if (def.is_distinct) {
      def.type = ParsedViewDef::ViewType::DISTINCT;
    } else if (!def.filters.empty()) {
      def.type = ParsedViewDef::ViewType::FILTER;
    } else if (!def.project_column_names.empty() && !def.select_all) {
      def.type = ParsedViewDef::ViewType::PROJECT;
    } else {
      // Default to filter with no conditions (pass-through)
      def.type = ParsedViewDef::ViewType::FILTER;
    }
  }

  // Helper to check for subqueries in expressions
  bool has_subquery_expression(duckdb::ParsedExpression *expr) {
    if (!expr)
      return false;

    // Check this expression
    if (expr->type == duckdb::ExpressionType::SUBQUERY) {
      return true;
    }

    // Note: For simplicity, just check the expression type
    // A full recursive check would require more complex traversal
    return false;
  }

  // Helper to create error results with proper formatting
  ParseResult make_error(ErrorCode code, const std::string &detail,
                         const std::string &sql, size_t pos = 0) {
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
};

// View factory - creates DBSP views from parsed definitions
class ViewFactory {
public:
  // Create a view from parsed definition
  // Requires schema information to resolve column indices
  static std::unique_ptr<NativeMaterializedView> create_view(
      const ParsedViewDef &def,
      const std::unordered_map<std::string, TableSchema> &table_schemas) {

    switch (def.type) {
    case ParsedViewDef::ViewType::FILTER:
      return create_filter_view(def, table_schemas);

    case ParsedViewDef::ViewType::PROJECT:
      return create_project_view(def, table_schemas);

    case ParsedViewDef::ViewType::AGGREGATE:
      return create_aggregate_view(def, table_schemas);

    case ParsedViewDef::ViewType::JOIN:
      return create_join_view(def, table_schemas);

    case ParsedViewDef::ViewType::DISTINCT:
      return create_distinct_view(def, table_schemas);

    case ParsedViewDef::ViewType::RECURSIVE:
      return create_recursive_view(def, table_schemas);

    default:
      return nullptr;
    }
  }

private:
  static std::unique_ptr<NativeMaterializedView> create_filter_view(
      const ParsedViewDef &def,
      const std::unordered_map<std::string, TableSchema> &table_schemas) {

    if (def.source_tables.empty())
      return nullptr;
    const std::string &table = def.source_tables[0];

    auto schema_it = table_schemas.find(table);
    TableSchema schema;
    if (schema_it != table_schemas.end()) {
      schema = schema_it->second;
    }

    // Resolve column indices for filters
    std::vector<std::pair<size_t, ParsedViewDef::FilterInfo>> resolved_filters;
    for (const auto &filter : def.filters) {
      size_t col_idx = find_column_index(schema, filter.column_name);
      resolved_filters.push_back({col_idx, filter});
    }

    // Create predicate function
    auto predicate = [resolved_filters](const DuckDBRow &row) -> bool {
      for (const auto &[col_idx, filter] : resolved_filters) {
        if (col_idx >= row.columns.size())
          return false;

        const auto &col_val = row.columns[col_idx];
        bool matches = compare_values(col_val, filter.op, filter.value);
        if (!matches)
          return false;
      }
      return true;
    };

    return std::make_unique<NativeFilterView>(def.view_name, def.sql, table,
                                              schema, predicate);
  }

  static std::unique_ptr<NativeMaterializedView> create_project_view(
      const ParsedViewDef &def,
      const std::unordered_map<std::string, TableSchema> &table_schemas) {

    if (def.source_tables.empty())
      return nullptr;
    const std::string &table = def.source_tables[0];

    auto schema_it = table_schemas.find(table);
    TableSchema source_schema;
    if (schema_it != table_schemas.end()) {
      source_schema = schema_it->second;
    }

    // Resolve column indices
    std::vector<size_t> col_indices;
    TableSchema result_schema;
    result_schema.table_name = def.view_name;

    for (const auto &col_name : def.project_column_names) {
      size_t idx = find_column_index(source_schema, col_name);
      col_indices.push_back(idx);

      if (idx < source_schema.columns.size()) {
        result_schema.columns.push_back(source_schema.columns[idx]);
      } else {
        result_schema.columns.push_back(
            {col_name, duckdb::LogicalType::VARCHAR});
      }
    }

    // Create projection function
    auto project = [col_indices](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow result;
      for (size_t idx : col_indices) {
        if (idx < row.columns.size()) {
          result.columns.push_back(row.columns[idx]);
        } else {
          result.columns.push_back(duckdb::Value());
        }
      }
      return result;
    };

    return std::make_unique<NativeProjectView>(def.view_name, def.sql, table,
                                               result_schema, project);
  }

  static std::unique_ptr<NativeMaterializedView> create_aggregate_view(
      const ParsedViewDef &def,
      const std::unordered_map<std::string, TableSchema> &table_schemas) {

    if (def.source_tables.empty() || def.aggregates.empty())
      return nullptr;
    const std::string &table = def.source_tables[0];

    auto schema_it = table_schemas.find(table);
    TableSchema source_schema;
    if (schema_it != table_schemas.end()) {
      source_schema = schema_it->second;
    }

    // Get first aggregate (simplified - only supports one aggregate for now)
    const auto &agg = def.aggregates[0];
    size_t value_col_idx =
        find_column_index(source_schema, agg.value_column_name);

    // For expression-based aggregates, resolve both column indices
    bool is_expr = agg.is_expression;
    size_t left_col_idx = 0, right_col_idx = 0;
    std::string expr_op;
    if (is_expr) {
      left_col_idx = find_column_index(source_schema, agg.left_col_name);
      right_col_idx = find_column_index(source_schema, agg.right_col_name);
      expr_op = agg.op;
    }

    // Resolve group by columns
    std::vector<size_t> group_cols;
    for (const auto &col_name : def.group_by_names) {
      group_cols.push_back(find_column_index(source_schema, col_name));
    }

    // If no group by, try to infer from non-aggregated columns
    if (group_cols.empty()) {
      for (const auto &col_name : def.project_column_names) {
        bool is_agg = false;
        for (const auto &a : def.aggregates) {
          if (a.value_column_name == col_name) {
            is_agg = true;
            break;
          }
        }
        if (!is_agg) {
          group_cols.push_back(find_column_index(source_schema, col_name));
        }
      }
    }

    // Create result schema
    TableSchema result_schema;
    result_schema.table_name = def.view_name;
    for (size_t idx : group_cols) {
      if (idx < source_schema.columns.size()) {
        result_schema.columns.push_back(source_schema.columns[idx]);
      }
    }
    result_schema.columns.push_back(
        {agg.alias.empty() ? agg.function : agg.alias,
         duckdb::LogicalType::BIGINT});

    // Key function
    auto key_fn = [group_cols](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      for (size_t idx : group_cols) {
        if (idx < row.columns.size()) {
          key.columns.push_back(row.columns[idx]);
        }
      }
      return key;
    };

    // Value function
    auto value_fn = [value_col_idx, is_expr, left_col_idx, right_col_idx,
                     expr_op](const DuckDBRow &row) -> duckdb::Value {
      if (is_expr) {
        // Expression-based aggregate: evaluate binary op on two columns
        if (left_col_idx < row.columns.size() &&
            right_col_idx < row.columns.size()) {
          int64_t left_val = row.columns[left_col_idx].GetValue<int64_t>();
          int64_t right_val = row.columns[right_col_idx].GetValue<int64_t>();
          int64_t result = 0;
          if (expr_op == "*")
            result = left_val * right_val;
          else if (expr_op == "+")
            result = left_val + right_val;
          else if (expr_op == "-")
            result = left_val - right_val;
          else if (expr_op == "/" && right_val != 0)
            result = left_val / right_val;
          return duckdb::Value::BIGINT(result);
        }
        return duckdb::Value::BIGINT(0);
      }
      if (value_col_idx < row.columns.size()) {
        return row.columns[value_col_idx];
      }
      return duckdb::Value::BIGINT(0);
    };

    // Map aggregate type
    NativeAggregateView::AggType agg_type = NativeAggregateView::AggType::SUM;
    if (agg.function == "COUNT")
      agg_type = NativeAggregateView::AggType::COUNT;
    else if (agg.function == "AVG")
      agg_type = NativeAggregateView::AggType::AVG;
    else if (agg.function == "MIN")
      agg_type = NativeAggregateView::AggType::MIN;
    else if (agg.function == "MAX")
      agg_type = NativeAggregateView::AggType::MAX;

    // Build HAVING predicate if present
    NativeAggregateView::HavingPredicate having_pred = nullptr;
    if (!def.having_filters.empty()) {
      // Capture the having filters for the lambda
      auto having_filters = def.having_filters;
      size_t num_group_cols = group_cols.size();
      size_t agg_col_idx =
          num_group_cols; // Aggregate result is after group columns

      // Resolve group column names to result row indices
      std::vector<std::pair<size_t, ParsedViewDef::HavingFilter>>
          resolved_having;
      for (const auto &hf : having_filters) {
        size_t col_idx = agg_col_idx; // Default: aggregate column
        if (hf.ref_type == ParsedViewDef::HavingFilter::GROUP_COL) {
          // Find position of this group column in result row
          for (size_t i = 0; i < def.group_by_names.size(); i++) {
            if (duckdb::StringUtil::CIEquals(def.group_by_names[i],
                                             hf.column_name)) {
              col_idx = i;
              break;
            }
          }
          // Also check project_column_names for non-group-by columns
          for (size_t i = 0; i < def.project_column_names.size(); i++) {
            if (duckdb::StringUtil::CIEquals(def.project_column_names[i],
                                             hf.column_name)) {
              col_idx = i;
              break;
            }
          }
        }
        resolved_having.push_back({col_idx, hf});
      }

      having_pred = [resolved_having](const DuckDBRow &result_row) -> bool {
        for (const auto &[col_idx, filter] : resolved_having) {
          if (col_idx >= result_row.columns.size())
            return false;
          const auto &col_val = result_row.columns[col_idx];
          if (!compare_values(col_val, filter.op, filter.value))
            return false;
        }
        return true;
      };
    }

    return std::make_unique<NativeAggregateView>(
        def.view_name, def.sql, table, result_schema, key_fn, value_fn,
        agg_type, having_pred);
  }

  static std::unique_ptr<NativeMaterializedView> create_join_view(
      const ParsedViewDef &def,
      const std::unordered_map<std::string, TableSchema> &table_schemas) {

    if (!def.join_info.has_value() || def.join_info->column_pairs.empty())
      return nullptr;
    const auto &join = def.join_info.value();

    auto left_schema_it = table_schemas.find(join.left_table);
    auto right_schema_it = table_schemas.find(join.right_table);

    TableSchema left_schema, right_schema;
    if (left_schema_it != table_schemas.end()) {
      left_schema = left_schema_it->second;
    }
    if (right_schema_it != table_schemas.end()) {
      right_schema = right_schema_it->second;
    }

    // Build column index pairs for composite key
    std::vector<size_t> left_key_indices, right_key_indices;
    for (const auto &[left_col, right_col] : join.column_pairs) {
      left_key_indices.push_back(find_column_index(left_schema, left_col));
      right_key_indices.push_back(find_column_index(right_schema, right_col));
    }

    // Composite key functions returning DuckDBRow
    auto left_key = [left_key_indices](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      for (size_t idx : left_key_indices) {
        if (idx < row.columns.size()) {
          key.columns.push_back(row.columns[idx]);
        } else {
          key.columns.push_back(duckdb::Value());
        }
      }
      return key;
    };

    auto right_key = [right_key_indices](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow key;
      for (size_t idx : right_key_indices) {
        if (idx < row.columns.size()) {
          key.columns.push_back(row.columns[idx]);
        } else {
          key.columns.push_back(duckdb::Value());
        }
      }
      return key;
    };

    // Handle projection if specified
    NativeJoinView::ProjectFn project = nullptr;
    TableSchema result_schema;
    result_schema.table_name = def.view_name;

    if (!def.project_column_refs.empty() && !def.select_all) {
      // Map (table/alias, col) -> combined index
      std::map<std::pair<std::string, std::string>, size_t> col_map;

      // Add left columns
      for (size_t i = 0; i < left_schema.columns.size(); i++) {
        std::string col = left_schema.columns[i].name;
        col_map[{join.left_table, col}] = i;
        // Add aliases for left table
        for (const auto &[alias, table] : def.table_aliases) {
          if (table == join.left_table) {
            col_map[{alias, col}] = i;
          }
        }
      }

      // Add right columns (offset by left size)
      size_t offset = left_schema.columns.size();
      for (size_t i = 0; i < right_schema.columns.size(); i++) {
        std::string col = right_schema.columns[i].name;
        col_map[{join.right_table, col}] = offset + i;
        // Add aliases for right table
        for (const auto &[alias, table] : def.table_aliases) {
          if (table == join.right_table) {
            col_map[{alias, col}] = offset + i;
          }
        }
      }

      std::vector<size_t> proj_indices;
      for (size_t i = 0; i < def.project_column_refs.size(); i++) {
        const auto &ref = def.project_column_refs[i];
        std::string table = ref.first;
        std::string col = ref.second;

        // Try exact match first
        auto it = col_map.find({table, col});
        if (it != col_map.end()) {
          proj_indices.push_back(it->second);
          // Add to result schema
          duckdb::LogicalType type = duckdb::LogicalType::VARCHAR; // Default
          if (it->second < left_schema.columns.size()) {
            type = left_schema.columns[it->second].type;
          } else {
            type = right_schema.columns[it->second - offset].type;
          }
          result_schema.columns.push_back({def.project_column_names[i], type});
        } else if (table.empty()) {
          // Unqualified name - try to resolve
          bool found = false;
          // Check left
          for (size_t j = 0; j < left_schema.columns.size(); j++) {
            if (duckdb::StringUtil::CIEquals(left_schema.columns[j].name,
                                             col)) {
              proj_indices.push_back(j);
              result_schema.columns.push_back(
                  {def.project_column_names[i], left_schema.columns[j].type});
              found = true;
              break;
            }
          }
          if (found)
            continue;

          // Check right
          for (size_t j = 0; j < right_schema.columns.size(); j++) {
            if (duckdb::StringUtil::CIEquals(right_schema.columns[j].name,
                                             col)) {
              proj_indices.push_back(offset + j);
              result_schema.columns.push_back(
                  {def.project_column_names[i], right_schema.columns[j].type});
              found = true;
              break;
            }
          }
          if (!found) {
            // Fallback: push null or error? For now, index 0 safely
            proj_indices.push_back(0);
            result_schema.columns.push_back(
                {def.project_column_names[i], duckdb::LogicalType::INTEGER});
          }
        } else {
          // Explicit table/alias not found - fallback
          proj_indices.push_back(0);
          result_schema.columns.push_back(
              {def.project_column_names[i], duckdb::LogicalType::INTEGER});
        }
      }

      project = [proj_indices](const DuckDBRow &row) -> DuckDBRow {
        DuckDBRow res;
        for (size_t idx : proj_indices) {
          if (idx < row.columns.size()) {
            res.columns.push_back(row.columns[idx]);
          } else {
            res.columns.push_back(duckdb::Value());
          }
        }
        return res;
      };

    } else {
      // Default: All columns
      for (const auto &col : left_schema.columns) {
        result_schema.columns.push_back(col);
      }
      for (const auto &col : right_schema.columns) {
        result_schema.columns.push_back(col);
      }
    }

    return std::make_unique<NativeJoinView>(
        def.view_name, def.sql, join.left_table, join.right_table,
        result_schema, left_key, right_key, project);
  }

  static std::unique_ptr<NativeMaterializedView> create_distinct_view(
      const ParsedViewDef &def,
      const std::unordered_map<std::string, TableSchema> &table_schemas) {

    if (def.source_tables.empty())
      return nullptr;
    const std::string &table = def.source_tables[0];

    auto schema_it = table_schemas.find(table);
    TableSchema source_schema;
    if (schema_it != table_schemas.end()) {
      source_schema = schema_it->second;
    }

    // Build projection function and result schema based on SELECT columns
    NativeDistinctView::ProjectFn project = nullptr;
    TableSchema result_schema;
    result_schema.table_name = def.view_name;

    if (!def.project_column_names.empty() && !def.select_all) {
      // DISTINCT on specific columns: SELECT DISTINCT col1, col2 FROM t
      std::vector<size_t> col_indices;
      for (const auto &col_name : def.project_column_names) {
        size_t idx = find_column_index(source_schema, col_name);
        col_indices.push_back(idx);

        if (idx < source_schema.columns.size()) {
          result_schema.columns.push_back(source_schema.columns[idx]);
        } else {
          result_schema.columns.push_back(
              {col_name, duckdb::LogicalType::VARCHAR});
        }
      }

      project = [col_indices](const DuckDBRow &row) -> DuckDBRow {
        DuckDBRow result;
        for (size_t idx : col_indices) {
          if (idx < row.columns.size()) {
            result.columns.push_back(row.columns[idx]);
          } else {
            result.columns.push_back(duckdb::Value());
          }
        }
        return result;
      };
    } else {
      // DISTINCT on all columns: SELECT DISTINCT * FROM t
      result_schema = source_schema;
    }

    return std::make_unique<NativeDistinctView>(def.view_name, def.sql, table,
                                                result_schema, project);
  }

  // Create a recursive view for WITH RECURSIVE queries
  // Note: Full recursive execution requires complex query compilation
  // This is a placeholder that detects the query structure
  static std::unique_ptr<NativeMaterializedView> create_recursive_view(
      const ParsedViewDef &def,
      const std::unordered_map<std::string, TableSchema> &table_schemas) {

    if (!def.recursive_cte.has_value()) {
      return nullptr;
    }

    const auto &rec = def.recursive_cte.value();

    // Get source schema (from first source table)
    TableSchema schema;
    if (!def.source_tables.empty()) {
      auto it = table_schemas.find(def.source_tables[0]);
      if (it != table_schemas.end()) {
        schema = it->second;
      }
    }

    // Parse and create anchor view
    // Note: DBSPSqlParser is defined before ViewFactory, so we can use it here
    DBSPSqlParser parser;

    // Parse anchor query
    auto anchor_res = parser.parse(rec.anchor_sql, def.view_name + "_anchor");
    if (!anchor_res.success) {
      return nullptr;
    }

    // Create anchor view
    auto anchor_view =
        ViewFactory::create_view(anchor_res.view_def, table_schemas);
    if (!anchor_view) {
      return nullptr;
    }

    // Prepare schemas for recursive step
    // The recursive step depends on the anchor's output schema (for the CTE
    // table)
    auto recursive_schemas = table_schemas;
    recursive_schemas[rec.cte_name] = anchor_view->result_schema();
    recursive_schemas[rec.cte_name].table_name = rec.cte_name;

    // Parse recursive query
    auto step_res = parser.parse(rec.recursive_sql, def.view_name + "_step");
    if (!step_res.success) {
      return nullptr;
    }

    // Create recursive step view
    auto step_view =
        ViewFactory::create_view(step_res.view_def, recursive_schemas);
    if (!step_view) {
      return nullptr;
    }

    std::string source_table =
        def.source_tables.empty() ? "" : def.source_tables[0];

    return std::make_unique<NativeRecursiveView>(
        def.view_name, rec.cte_name, def.sql, source_table, schema,
        std::move(anchor_view), std::move(step_view), rec.union_all,
        1000 /* max_iterations */);
  }

  static size_t find_column_index(const TableSchema &schema,
                                  const std::string &name) {
    for (size_t i = 0; i < schema.columns.size(); i++) {
      if (duckdb::StringUtil::CIEquals(schema.columns[i].name, name)) {
        return i;
      }
    }
    return 0; // Default to first column
  }

  static bool compare_values(const duckdb::Value &left, const std::string &op,
                             const duckdb::Value &right) {
    try {
      if (op == "=")
        return left == right;
      if (op == "!=")
        return left != right;

      // For numeric comparisons
      if (left.type().IsNumeric() && right.type().IsNumeric()) {
        double l = left.GetValue<double>();
        double r = right.GetValue<double>();
        if (op == ">")
          return l > r;
        if (op == "<")
          return l < r;
        if (op == ">=")
          return l >= r;
        if (op == "<=")
          return l <= r;
      }

      // String comparison
      if (op == ">")
        return left.ToString() > right.ToString();
      if (op == "<")
        return left.ToString() < right.ToString();
      if (op == ">=")
        return left.ToString() >= right.ToString();
      if (op == "<=")
        return left.ToString() <= right.ToString();

    } catch (...) {
      return false;
    }
    return false;
  }
};

} // namespace dbsp_native

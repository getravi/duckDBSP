// DBSP SQL Parser
// Parses SQL view definitions and creates appropriate DBSP views

#pragma once

#include "dbsp_distinct_on.hpp"
#include "dbsp_duckdb_types.hpp"
#include "dbsp_errors.hpp"
#include "dbsp_recursive.hpp"
#include "dbsp_set_ops.hpp"
#include "dbsp_window_view.hpp"

#include "duckdb.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/cte_node.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace dbsp_native {

// Parsed view definition
// Parsed view definition
struct ParsedViewDef {
  enum class ViewType {
    FILTER,
    PROJECT,
    FILTER_PROJECT, // Fused filter+project (optimizer-generated)
    AGGREGATE,
    JOIN,
    DISTINCT,
    RECURSIVE,
    SORT,
    LIMIT,
    WINDOW,
    SET_OP,
    DISTINCT_ON,
    CTE, // Non-recursive CTE (WITH ... AS ...)
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

    // Non-equi predicates: ON a.x = b.x AND a.y > b.z
    struct NonEquiPredicate {
      std::string left_col;
      std::string right_col;
      std::string op; // ">", "<", ">=", "<=", "!="
      size_t left_idx = 0;
      size_t right_idx = 0;
    };
    std::vector<NonEquiPredicate> non_equi_predicates;
  };
  std::optional<JoinInfo> join_info;

  // For recursive CTE views (WITH RECURSIVE)
  struct RecursiveCTEInfo {
    std::string cte_name;      // Name of the recursive CTE
    bool union_all = false;    // UNION vs UNION ALL
    std::string source_table;  // Base table referenced
    std::string anchor_sql;    // SQL for anchor (non-recursive) query
    std::string recursive_sql; // SQL for recursive step (references cte_name)
    std::vector<std::string> column_aliases; // t(col1, col2) aliases
  };
  std::optional<RecursiveCTEInfo> recursive_cte;

  // Is DISTINCT present
  bool is_distinct = false;

  // For DISTINCT ON
  std::vector<std::string> distinct_on_names;
  std::vector<size_t> distinct_on_columns;

  // For arithmetic expressions in projections (e.g., SELECT a + b as c)
  struct ProjectionExpression {
    std::string alias;
    std::string left_col;
    std::string right_col;
    std::string op; // +, -, *, /
    size_t left_idx = 0;
    size_t right_idx = 0;
    bool left_is_const = false;
    bool right_is_const = false;
    duckdb::Value left_const;
    duckdb::Value right_const;
  };
  std::vector<ProjectionExpression> projection_exprs;

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

  // For ORDER BY
  struct SortInfo {
    size_t column_idx; // If known, otherwise use name
    std::string column_name;
    bool ascending = true;
    bool nulls_first = false;
  };
  std::vector<SortInfo> sort_columns;

  // For LIMIT / OFFSET
  std::optional<int64_t> limit;
  std::optional<int64_t> offset;

  // For window functions
  struct WindowInfo {
    std::string function; // ROW_NUMBER, RANK, SUM, etc.
    std::string alias;
    std::vector<std::string> partition_by;
    std::vector<SortInfo> order_by;
    std::string arg_column;
    int offset = 1;       // For LAG/LEAD offset
    int start_offset = 0; // For ROWS BETWEEN N PRECEDING
    int end_offset = 0;   // For ROWS BETWEEN M FOLLOWING
    // Frame info
    duckdb::WindowBoundary start = duckdb::WindowBoundary::UNBOUNDED_PRECEDING;
    duckdb::WindowBoundary end = duckdb::WindowBoundary::CURRENT_ROW_ROWS;
  };
  std::vector<WindowInfo> windows;

  // For set operations (UNION, INTERSECT, EXCEPT)
  struct SetOperationInfo {
    duckdb::SetOperationType type;
    bool all = false;
    std::string left_view;
    std::string right_view;
  };
  std::optional<SetOperationInfo> set_op;

  // For non-recursive CTEs (WITH cte AS (...) SELECT ... FROM cte)
  struct CTEInfo {
    std::string cte_name;
    std::string cte_sql; // SQL for the CTE definition
    std::vector<std::string> column_aliases;
  };
  std::vector<CTEInfo> ctes;

  // For subqueries in FROM (derived tables)
  struct DerivedTableInfo {
    std::string alias;
    std::string subquery_sql;
  };
  std::vector<DerivedTableInfo> derived_tables;

  // ========================================================================
  // Optimizer metadata (populated by DBSPOptimizer)
  // ========================================================================
  bool optimized = false; // Whether optimization passes have been applied

  // Pushed-down filters (for join queries)
  std::vector<FilterInfo> left_pushed_filters; // Filters pushed to left source
  std::vector<FilterInfo>
      right_pushed_filters; // Filters pushed to right source

  // Required columns (after projection pruning)
  std::vector<std::string> required_columns;
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
    std::string sql_to_parse = sql;

    // Pre-process: strip "MATERIALIZED" from "CREATE MATERIALIZED VIEW"
    // simplistic check for now
    size_t mat_pos = sql_to_parse.find("MATERIALIZED");
    if (mat_pos != std::string::npos) {
      // Check if it looks like CREATE MATERIALIZED VIEW
      // strict check: replace "MATERIALIZED VIEW" with "VIEW"
      size_t pos = sql_to_parse.find("MATERIALIZED VIEW");
      if (pos != std::string::npos) {
        sql_to_parse.replace(pos, 17, "VIEW");
      }
    }

    try {
      duckdb::Parser parser;
      parser.ParseQuery(sql_to_parse);

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

      // Check for CREATE VIEW / CREATE MATERIALIZED VIEW
      if (stmt->type == duckdb::StatementType::CREATE_STATEMENT) {
        auto &create = stmt->template Cast<duckdb::CreateStatement>();

        if (create.info->type == duckdb::CatalogType::VIEW_ENTRY) {
          auto &view_info =
              create.info->template Cast<duckdb::CreateViewInfo>();

          // Extract query from view definition
          std::string view_query_sql;
          if (view_info.query) {
            return parse_select(*view_info.query, view_name.empty()
                                                      ? view_info.view_name
                                                      : view_name);
          } else {
            result.error = "View definition missing query";
            return result;
          }
        } else {
          result.error =
              "Only CREATE VIEW / MATERIALIZED VIEW statements are supported";
          return result;
        }
      }

      // Check for Extension Statement (some DDL might end up here)
      if (stmt->type == duckdb::StatementType::EXTENSION_STATEMENT) {
        std::cerr << "Got EXTENSION_STATEMENT" << std::endl;
      }

      std::cerr << "Unexpected statement type: " << (int)stmt->type
                << std::endl;
      result.error = "Expected SELECT or CREATE VIEW statement. Got type: " +
                     std::to_string((int)stmt->type);
      return result;
    } catch (const std::exception &e) {
      std::cerr << "Exception in parse: " << e.what() << std::endl;
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
      extract_sources_from_node(recursive.left.get(), result.view_def);
      extract_sources_from_node(recursive.right.get(), result.view_def);

      auto &sources = result.view_def.source_tables;
      sources.erase(
          std::remove(sources.begin(), sources.end(), cte_info.cte_name),
          sources.end());

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
          rec_info.column_aliases = cte_info->aliases; // Capture aliases
          // Extract anchor and recursive SQL from left/right sub-queries
          if (recursive.left) {
            rec_info.anchor_sql = recursive.left->ToString();
            extract_sources_from_node(recursive.left.get(), result.view_def);
          }
          if (recursive.right) {
            rec_info.recursive_sql = recursive.right->ToString();
            extract_sources_from_node(recursive.right.get(), result.view_def);
          }

          // Remove CTE name from source tables (it's internal)
          auto &sources = result.view_def.source_tables;
          sources.erase(
              std::remove(sources.begin(), sources.end(), rec_info.cte_name),
              sources.end());

          result.view_def.recursive_cte = rec_info;
          result.view_def.view_name = view_name;
          result.view_def.sql = stmt.ToString();
          result.success = true;
          determine_view_type(result.view_def);
          return result;
        }
      }
    }

    // Handle CTE_NODE wrapper (non-recursive CTEs wrapped by DuckDB parser)
    // Must come before cte_map check to avoid duplicates
    // CTENode fields: query = CTE body, child = main query using the CTE
    if (node->type == duckdb::QueryNodeType::CTE_NODE) {
      // Unwrap all CTE layers, collecting CTE info
      while (node->type == duckdb::QueryNodeType::CTE_NODE) {
        auto &cte = node->template Cast<duckdb::CTENode>();
        ParsedViewDef::CTEInfo info;
        info.cte_name = cte.ctename;
        info.column_aliases = cte.aliases;
        if (cte.query) {
          info.cte_sql = cte.query->ToString();
        }
        result.view_def.ctes.push_back(info);
        // Add CTE name to source tables (the outer query references it)
        result.view_def.source_tables.push_back(cte.ctename);
        // Move to the child (main query) node
        node = cte.child.get();
      }
      // Now node points to the actual SELECT/SET_OPERATION inside the CTE
      // Fall through to handle it below
    }

    // Handle non-recursive CTEs from cte_map (supplements CTE_NODE extraction)
    // Skip CTEs already extracted by CTE_NODE unwrapping above
    if (!node->cte_map.map.empty()) {
      // Build set of already-extracted CTE names to avoid duplicates
      std::unordered_set<std::string> existing_ctes;
      for (const auto &c : result.view_def.ctes) {
        existing_ctes.insert(c.cte_name);
      }

      for (auto &cte_entry : node->cte_map.map) {
        if (existing_ctes.count(cte_entry.first))
          continue; // Already extracted from CTE_NODE
        auto &cte_info = cte_entry.second;
        if (!cte_info || !cte_info->query || !cte_info->query->node)
          continue;
        auto *cte_node = cte_info->query->node.get();
        if (cte_node->type == duckdb::QueryNodeType::RECURSIVE_CTE_NODE)
          continue;

        ParsedViewDef::CTEInfo info;
        info.cte_name = cte_entry.first;
        info.cte_sql = cte_info->query->ToString();
        info.column_aliases = cte_info->aliases;
        result.view_def.ctes.push_back(info);
        result.view_def.source_tables.push_back(cte_entry.first);
      }
    }

    // Check for set operations (UNION, INTERSECT, EXCEPT)
    if (node->type == duckdb::QueryNodeType::SET_OPERATION_NODE) {
      auto &set_op = node->template Cast<duckdb::SetOperationNode>();
      ParsedViewDef::SetOperationInfo info;
      info.type = set_op.setop_type;
      info.all = set_op.setop_all;

      if (set_op.left) {
        info.left_view = set_op.left->ToString();
        // Recursively extract sources from left child
        extract_sources_from_node(set_op.left.get(), result.view_def);
      }
      if (set_op.right) {
        info.right_view = set_op.right->ToString();
        // Recursively extract sources from right child
        extract_sources_from_node(set_op.right.get(), result.view_def);
      }

      result.view_def.type =
          ParsedViewDef::ViewType::SET_OP; // Explicitly set type
      result.view_def.set_op = info;
      result.view_def.view_name = view_name;
      result.view_def.sql = stmt.ToString();
      result.success = true;
      // determine_view_type(result.view_def); // Already set to SET_OP
      return result;
    }

    if (node->type != duckdb::QueryNodeType::SELECT_NODE) {
      result.error = "Complex queries not yet supported. Node type: " +
                     std::to_string((int)node->type);
      return result;
    }

    auto &select = node->template Cast<duckdb::SelectNode>();

    // Check for modifiers
    for (auto &modifier : select.modifiers) {
      switch (modifier->type) {
      case duckdb::ResultModifierType::DISTINCT_MODIFIER: {
        auto &distinct = modifier->template Cast<duckdb::DistinctModifier>();
        if (!distinct.distinct_on_targets.empty()) {
          for (auto &target : distinct.distinct_on_targets) {
            if (target->type == duckdb::ExpressionType::COLUMN_REF) {
              auto &col = target->template Cast<duckdb::ColumnRefExpression>();
              result.view_def.distinct_on_names.push_back(col.GetColumnName());
            }
          }
        } else {
          result.view_def.is_distinct = true;
        }
        break;
      }
      case duckdb::ResultModifierType::LIMIT_MODIFIER: {
        auto &limit = modifier->template Cast<duckdb::LimitModifier>();
        // Check if limit/offset are constant values
        if (limit.limit) {
          if (limit.limit->type == duckdb::ExpressionType::VALUE_CONSTANT) {
            auto &val =
                limit.limit->template Cast<duckdb::ConstantExpression>();
            result.view_def.limit = val.value.GetValue<int64_t>();
          }
        }
        if (limit.offset) {
          if (limit.offset->type == duckdb::ExpressionType::VALUE_CONSTANT) {
            auto &val =
                limit.offset->template Cast<duckdb::ConstantExpression>();
            result.view_def.offset = val.value.GetValue<int64_t>();
          }
        }
        break;
      }
      case duckdb::ResultModifierType::ORDER_MODIFIER: {
        auto &order = modifier->template Cast<duckdb::OrderModifier>();
        for (auto &node : order.orders) {
          ParsedViewDef::SortInfo sort_info;
          if (node.expression->type == duckdb::ExpressionType::COLUMN_REF) {
            auto &col =
                node.expression->template Cast<duckdb::ColumnRefExpression>();
            sort_info.column_name = col.GetColumnName();
          }
          sort_info.ascending = (node.type != duckdb::OrderType::DESCENDING);
          if (node.null_order == duckdb::OrderByNullType::ORDER_DEFAULT) {
            // DuckDB default: ASC NULLS LAST, DESC NULLS FIRST
            sort_info.nulls_first = !sort_info.ascending;
          } else {
            sort_info.nulls_first =
                (node.null_order == duckdb::OrderByNullType::NULLS_FIRST);
          }
          result.view_def.sort_columns.push_back(sort_info);
        }
        break;
      }
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

    if (!parse_from_clause(select.from_table.get(), result.view_def)) {
      result.error = "FROM clause required";
      return result;
    }

    // Check for window functions in SELECT list
    for (size_t i = 0; i < select.select_list.size(); i++) {
      auto &expr = select.select_list[i];
      if (expr->IsWindow()) {
        // Valid window function found, will be parsed in parse_select_list
      }
    }

    // Parse SELECT columns
    parse_select_list(select.select_list, result);

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

  bool is_window_function(const duckdb::ParsedExpression &expr) {
    if (expr.expression_class == duckdb::ExpressionClass::WINDOW)
      return true;
    if (expr.IsWindow())
      return true;
    // Fallback: check ExpressionType range for window functions in DuckDB
    // 110 (WINDOW_AGGREGATE) to 135 (WINDOW_FILL)
    auto type_int = static_cast<uint8_t>(expr.type);
    if (type_int >= 110 && type_int <= 135)
      return true;

    // Last resort check for string representation
    if (expr.ToString().find(" OVER ") != std::string::npos)
      return true;

    return false;
  }

  void parse_window_expression(duckdb::ParsedExpression *expr,
                               ParsedViewDef &def) {
    if (!expr)
      return;
    // cast to WindowExpression
    auto &win = expr->template Cast<duckdb::WindowExpression>();

    ParsedViewDef::WindowInfo info;
    info.alias = win.alias;

    // Function name
    // For now handle specific types or generic
    if (win.type == duckdb::ExpressionType::WINDOW_ROW_NUMBER)
      info.function = "ROW_NUMBER";
    else if (win.type == duckdb::ExpressionType::WINDOW_RANK)
      info.function = "RANK";
    else if (win.type == duckdb::ExpressionType::WINDOW_RANK_DENSE)
      info.function = "DENSE_RANK";
    else if (win.type == duckdb::ExpressionType::WINDOW_LAG)
      info.function = "LAG";
    else if (win.type == duckdb::ExpressionType::WINDOW_LEAD)
      info.function = "LEAD";
    else if (win.type == duckdb::ExpressionType::WINDOW_FIRST_VALUE)
      info.function = "FIRST_VALUE";
    else if (win.type == duckdb::ExpressionType::WINDOW_LAST_VALUE)
      info.function = "LAST_VALUE";
    else if (win.type == duckdb::ExpressionType::WINDOW_NTH_VALUE)
      info.function = "NTH_VALUE";
    else if (win.type == duckdb::ExpressionType::WINDOW_NTILE)
      info.function = "NTILE";
    else if (win.type == duckdb::ExpressionType::WINDOW_AGGREGATE) {
      info.function = duckdb::StringUtil::Upper(win.function_name);
    } else {
      info.function = "UNKNOWN";
    }

    // Partition By
    for (auto &part : win.partitions) {
      if (part->type == duckdb::ExpressionType::COLUMN_REF) {
        auto &col = part->template Cast<duckdb::ColumnRefExpression>();
        info.partition_by.push_back(col.GetColumnName());
      }
    }

    // Order By
    for (auto &ord : win.orders) {
      ParsedViewDef::SortInfo sort;
      if (ord.expression->type == duckdb::ExpressionType::COLUMN_REF) {
        auto &col =
            ord.expression->template Cast<duckdb::ColumnRefExpression>();
        sort.column_name = col.GetColumnName();
      }
      sort.ascending = (ord.type != duckdb::OrderType::DESCENDING);
      if (ord.null_order == duckdb::OrderByNullType::ORDER_DEFAULT) {
        // DuckDB default: ASC NULLS LAST, DESC NULLS FIRST
        sort.nulls_first = !sort.ascending;
      } else {
        sort.nulls_first =
            (ord.null_order == duckdb::OrderByNullType::NULLS_FIRST);
      }
      info.order_by.push_back(sort);
    }

    // Arguments
    if (!win.children.empty()) {
      if (win.children[0]->type == duckdb::ExpressionType::COLUMN_REF) {
        auto &col =
            win.children[0]->template Cast<duckdb::ColumnRefExpression>();
        info.arg_column = col.GetColumnName();
      }
    }

    // Offset for LAG/LEAD
    if (win.offset_expr) {
      if (win.offset_expr->type == duckdb::ExpressionType::VALUE_CONSTANT) {
        auto &val =
            win.offset_expr->template Cast<duckdb::ConstantExpression>();
        try {
          info.offset = val.value.GetValue<int32_t>();
        } catch (...) {
          info.offset = 1;
        }
      }
    }

    // Frame boundaries
    if (win.start != duckdb::WindowBoundary::INVALID) {
      info.start = win.start;
      if (win.start_expr &&
          win.start_expr->type == duckdb::ExpressionType::VALUE_CONSTANT) {
        auto &val = win.start_expr->template Cast<duckdb::ConstantExpression>();
        try {
          info.start_offset = val.value.GetValue<int32_t>();
        } catch (...) {
          info.start_offset = 0;
        }
      }
    }
    if (win.end != duckdb::WindowBoundary::INVALID) {
      info.end = win.end;
      if (win.end_expr &&
          win.end_expr->type == duckdb::ExpressionType::VALUE_CONSTANT) {
        auto &val = win.end_expr->template Cast<duckdb::ConstantExpression>();
        try {
          info.end_offset = val.value.GetValue<int32_t>();
        } catch (...) {
          info.end_offset = 0;
        }
      }
    }

    def.windows.push_back(info);
  }

  // Parse FROM clause
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

    case duckdb::TableReferenceType::SUBQUERY: {
      // Subquery in FROM clause (derived table)
      auto &subquery = ref->template Cast<duckdb::SubqueryRef>();
      ParsedViewDef::DerivedTableInfo dt;
      dt.alias = subquery.alias.empty()
                     ? "_derived_" + std::to_string(def.derived_tables.size())
                     : subquery.alias;
      if (subquery.subquery) {
        dt.subquery_sql = subquery.subquery->ToString();
      }
      def.derived_tables.push_back(dt);
      // Use the alias as a source table name
      def.source_tables.push_back(dt.alias);
      if (!subquery.alias.empty()) {
        def.table_aliases[subquery.alias] = dt.alias;
      }
      return true;
    }

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

        // TODO: Implement parse_join_condition to extract join predicates
        // parse_join_condition(join.condition.get(), info);
        def.join_info = info;
      }
      return true;
    }

    default:
      return false;
    }
  }

  static void extract_sources_from_node(duckdb::QueryNode *node,
                                        ParsedViewDef &def) {
    if (!node)
      return;

    if (node->type == duckdb::QueryNodeType::SELECT_NODE) {
      auto &select = node->template Cast<duckdb::SelectNode>();
      if (select.from_table) {
        ParsedViewDef temp_def;
        DBSPSqlParser parser;
        if (parser.parse_from_clause(select.from_table.get(), temp_def)) {
          def.source_tables.insert(def.source_tables.end(),
                                   temp_def.source_tables.begin(),
                                   temp_def.source_tables.end());
        }
      }
    } else if (node->type == duckdb::QueryNodeType::SET_OPERATION_NODE) {
      auto &set_op = node->template Cast<duckdb::SetOperationNode>();
      extract_sources_from_node(set_op.left.get(), def);
      extract_sources_from_node(set_op.right.get(), def);
    } else if (node->type == duckdb::QueryNodeType::RECURSIVE_CTE_NODE) {
      auto &rec = node->template Cast<duckdb::RecursiveCTENode>();
      extract_sources_from_node(rec.left.get(), def);
      extract_sources_from_node(rec.right.get(), def);
    } else if (node->type == duckdb::QueryNodeType::CTE_NODE) {
      auto &cte = node->template Cast<duckdb::CTENode>();
      extract_sources_from_node(cte.child.get(), def);
    }
  }

private:
  static void parse_join_condition(duckdb::ParsedExpression *expr,
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

        // Add column pair to the equi-join list
        info.column_pairs.push_back(
            {left_col.GetColumnName(), right_col.GetColumnName()});
      }
    } else if (is_non_equi_comparison(expr->type)) {
      // Handle non-equi predicates: >, <, >=, <=, !=
      auto &cmp = expr->template Cast<duckdb::ComparisonExpression>();

      if (cmp.left->type == duckdb::ExpressionType::COLUMN_REF &&
          cmp.right->type == duckdb::ExpressionType::COLUMN_REF) {
        auto &left_col = cmp.left->template Cast<duckdb::ColumnRefExpression>();
        auto &right_col =
            cmp.right->template Cast<duckdb::ColumnRefExpression>();

        ParsedViewDef::JoinInfo::NonEquiPredicate pred;
        pred.left_col = left_col.GetColumnName();
        pred.right_col = right_col.GetColumnName();
        pred.op = comparison_op_to_string(expr->type);
        info.non_equi_predicates.push_back(pred);
      }
    }
  }

  static bool is_non_equi_comparison(duckdb::ExpressionType type) {
    return type == duckdb::ExpressionType::COMPARE_GREATERTHAN ||
           type == duckdb::ExpressionType::COMPARE_LESSTHAN ||
           type == duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
           type == duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO ||
           type == duckdb::ExpressionType::COMPARE_NOTEQUAL;
  }

  static std::string comparison_op_to_string(duckdb::ExpressionType type) {
    switch (type) {
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

  template <typename VectorType>
  void parse_select_list(VectorType &select_list, ParseResult &result) {
    ParsedViewDef &def = result.view_def;
    for (size_t i = 0; i < select_list.size(); i++) {
      auto &expr = select_list[i];

      // Handle window functions first
      if (is_window_function(*expr)) {
        parse_window_expression(expr.get(), def);
        continue;
      }

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
        def.project_column_refs.push_back({table_name, col.GetColumnName()});
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
        } else if ((func_name == "*" || func_name == "+" || func_name == "-" ||
                    func_name == "/") &&
                   func.children.size() == 2) {
          // Handle binary expressions: Col op Col, Col op Const, Const op Col

          bool left_is_col =
              func.children[0]->type == duckdb::ExpressionType::COLUMN_REF;
          bool right_is_col =
              func.children[1]->type == duckdb::ExpressionType::COLUMN_REF;
          bool left_is_const = (func.children[0]->type ==
                                duckdb::ExpressionType::VALUE_CONSTANT);
          bool right_is_const = (func.children[1]->type ==
                                 duckdb::ExpressionType::VALUE_CONSTANT);

          if ((left_is_col || left_is_const) &&
              (right_is_col || right_is_const) &&
              !(left_is_const && right_is_const)) {

            ParsedViewDef::ProjectionExpression expr_info;
            expr_info.op = func_name;
            expr_info.alias =
                expr->alias.empty() ? expr->ToString() : expr->alias;

            if (left_is_col) {
              auto &col = func.children[0]
                              ->template Cast<duckdb::ColumnRefExpression>();
              expr_info.left_col = col.GetColumnName();
            } else {
              expr_info.left_is_const = true;
              auto &val =
                  func.children[0]->template Cast<duckdb::ConstantExpression>();
              expr_info.left_const = val.value;
            }

            if (right_is_col) {
              auto &col = func.children[1]
                              ->template Cast<duckdb::ColumnRefExpression>();
              expr_info.right_col = col.GetColumnName();
            } else {
              expr_info.right_is_const = true;
              auto &val =
                  func.children[1]->template Cast<duckdb::ConstantExpression>();
              expr_info.right_const = val.value;
            }

            def.projection_exprs.push_back(expr_info);
            def.project_column_names.push_back(expr_info.alias);
            def.project_columns.push_back(i);
          }
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
    // Priority: RECURSIVE > CTE > JOIN > WINDOW > AGGREGATE > DISTINCT >
    // FILTER_PROJECT > FILTER > PROJECT
    if (def.recursive_cte.has_value()) {
      def.type = ParsedViewDef::ViewType::RECURSIVE;
    } else if (def.set_op.has_value()) {
      def.type = ParsedViewDef::ViewType::SET_OP;
    } else if (!def.distinct_on_names.empty()) {
      def.type = ParsedViewDef::ViewType::DISTINCT_ON;
    } else if (!def.windows.empty()) {
      def.type = ParsedViewDef::ViewType::WINDOW;
    } else if (def.join_info.has_value()) {
      def.type = ParsedViewDef::ViewType::JOIN;
    } else if (!def.aggregates.empty()) {
      def.type = ParsedViewDef::ViewType::AGGREGATE;
    } else if (def.is_distinct) {
      def.type = ParsedViewDef::ViewType::DISTINCT;
    } else if (!def.filters.empty()) {
      // Check if we also have projection expressions - use fused FILTER_PROJECT
      if (!def.projection_exprs.empty() ||
          (!def.project_column_names.empty() && !def.select_all)) {
        def.type = ParsedViewDef::ViewType::FILTER_PROJECT;
      } else {
        def.type = ParsedViewDef::ViewType::FILTER;
      }
    } else if (def.limit.has_value() || def.offset.has_value()) {
      def.type = ParsedViewDef::ViewType::LIMIT;
    } else if (!def.sort_columns.empty()) {
      def.type = ParsedViewDef::ViewType::SORT;
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

    case ParsedViewDef::ViewType::FILTER_PROJECT:
      return create_filter_project_view(def, table_schemas);

    case ParsedViewDef::ViewType::PROJECT:
      return create_project_view(def, table_schemas);

    case ParsedViewDef::ViewType::AGGREGATE:
      return create_aggregate_view(def, table_schemas);

    case ParsedViewDef::ViewType::JOIN:
      return create_join_view(def, table_schemas);

    case ParsedViewDef::ViewType::DISTINCT:
      return create_distinct_view(def, table_schemas);

    case ParsedViewDef::ViewType::SORT:
      return create_sort_view(def, table_schemas);

    case ParsedViewDef::ViewType::LIMIT:
      return create_limit_view(def, table_schemas);

    case ParsedViewDef::ViewType::WINDOW:
      return create_window_view(def, table_schemas);

    case ParsedViewDef::ViewType::RECURSIVE:
      return create_recursive_view(def, table_schemas);

    case ParsedViewDef::ViewType::SET_OP:
      return create_set_op_view(def, table_schemas);

    case ParsedViewDef::ViewType::DISTINCT_ON:
      return create_distinct_on_view(def, table_schemas);

    case ParsedViewDef::ViewType::CTE:
      // CTE views are handled by the CDCManager which creates
      // intermediate views. If we get here, treat as the underlying type.
      // Re-determine type ignoring CTEs
      {
        ParsedViewDef def_copy = def;
        def_copy.ctes.clear();
        // Fall through to the actual view type
        if (def_copy.join_info.has_value())
          return create_join_view(def_copy, table_schemas);
        if (!def_copy.aggregates.empty())
          return create_aggregate_view(def_copy, table_schemas);
        if (def_copy.is_distinct)
          return create_distinct_view(def_copy, table_schemas);
        if (!def_copy.filters.empty())
          return create_filter_view(def_copy, table_schemas);
        if (!def_copy.project_column_names.empty() && !def_copy.select_all)
          return create_project_view(def_copy, table_schemas);
        return create_filter_view(def_copy, table_schemas);
      }

    default:
      return nullptr;
    }
  }

  static std::unique_ptr<NativeMaterializedView> create_distinct_on_view(
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

    std::vector<size_t> partition_keys;
    for (const auto &name : def.distinct_on_names) {
      partition_keys.push_back(find_column_index(source_schema, name));
    }

    std::vector<NativeDistinctOnView::SortColumn> sort_cols;
    for (const auto &sort : def.sort_columns) {
      sort_cols.push_back({sort.column_idx, sort.ascending, sort.nulls_first});
    }

    // Result schema for DISTINCT ON (picks whole rows)
    TableSchema result_schema = source_schema;
    result_schema.table_name = def.view_name;

    return std::make_unique<NativeDistinctOnView>(def.view_name, def.sql, table,
                                                  result_schema, partition_keys,
                                                  sort_cols);
  }

  // Parse FROM clause
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

    case duckdb::TableReferenceType::SUBQUERY: {
      // Subquery in FROM clause (derived table)
      auto &subquery = ref->template Cast<duckdb::SubqueryRef>();
      ParsedViewDef::DerivedTableInfo dt;
      dt.alias = subquery.alias.empty()
                     ? "_derived_" + std::to_string(def.derived_tables.size())
                     : subquery.alias;
      if (subquery.subquery) {
        dt.subquery_sql = subquery.subquery->ToString();
      }
      def.derived_tables.push_back(dt);
      // Use the alias as a source table name
      def.source_tables.push_back(dt.alias);
      if (!subquery.alias.empty()) {
        def.table_aliases[subquery.alias] = dt.alias;
      }
      return true;
    }

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

        // TODO: Implement parse_join_condition to extract join predicates
        // parse_join_condition(join.condition.get(), info);
        def.join_info = info;
      }
      return true;
    }

    default:
      return false;
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

  // Fused filter+project: applies filter then projects in a single pass
  static std::unique_ptr<NativeMaterializedView> create_filter_project_view(
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

    // Build filter predicate
    std::vector<std::pair<size_t, ParsedViewDef::FilterInfo>> resolved_filters;
    for (const auto &filter : def.filters) {
      size_t col_idx = find_column_index(source_schema, filter.column_name);
      resolved_filters.push_back({col_idx, filter});
    }

    auto predicate = [resolved_filters](const DuckDBRow &row) -> bool {
      for (const auto &[col_idx, filter] : resolved_filters) {
        if (col_idx >= row.columns.size())
          return false;
        const auto &col_val = row.columns[col_idx];
        if (!compare_values(col_val, filter.op, filter.value))
          return false;
      }
      return true;
    };

    // Add arithmetic expressions to projection
    struct ResolvedExpr {
      size_t left_idx;
      size_t right_idx;
      std::string op;
      bool left_is_const;
      bool right_is_const;
      duckdb::Value left_const;
      duckdb::Value right_const;
    };

    std::unordered_map<std::string, ResolvedExpr> expr_map;
    for (auto &expr : def.projection_exprs) {
      ResolvedExpr res;
      res.left_idx = find_column_index(source_schema, expr.left_col);
      res.right_idx = find_column_index(source_schema, expr.right_col);
      res.op = expr.op;
      res.left_is_const = expr.left_is_const;
      res.right_is_const = expr.right_is_const;
      res.left_const = expr.left_const;
      res.right_const = expr.right_const;
      expr_map[expr.alias] = res;
    }

    // Build projection items map to preserve order
    std::vector<std::pair<bool, std::variant<size_t, ResolvedExpr>>>
        projection_items;
    TableSchema result_schema;
    result_schema.table_name = def.view_name;

    for (const auto &col_name : def.project_column_names) {
      if (expr_map.count(col_name)) {
        // Expression
        projection_items.push_back({true, expr_map[col_name]});
        result_schema.columns.push_back(
            {col_name, duckdb::LogicalType::DOUBLE});
      } else {
        // Column
        size_t idx = find_column_index(source_schema, col_name);
        projection_items.push_back({false, idx});
        if (idx < source_schema.columns.size()) {
          result_schema.columns.push_back(source_schema.columns[idx]);
        } else {
          result_schema.columns.push_back(
              {col_name, duckdb::LogicalType::VARCHAR});
        }
      }
    }

    auto project = [projection_items](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow result;
      for (const auto &item : projection_items) {
        if (item.first) {
          // Expression
          const auto &expr = std::get<ResolvedExpr>(item.second);
          duckdb::Value v1, v2;

          if (expr.left_is_const) {
            v1 = expr.left_const;
          } else if (expr.left_idx < row.columns.size()) {
            v1 = row.columns[expr.left_idx];
          }

          if (expr.right_is_const) {
            v2 = expr.right_const;
          } else if (expr.right_idx < row.columns.size()) {
            v2 = row.columns[expr.right_idx];
          }

          if (v1.IsNull() || v2.IsNull()) {
            result.columns.push_back(
                duckdb::Value(duckdb::LogicalType::DOUBLE));
          } else {
            double d1 = v1.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                            .GetValue<double>();
            double d2 = v2.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                            .GetValue<double>();

            if (expr.op == "+")
              result.columns.push_back(duckdb::Value::DOUBLE(d1 + d2));
            else if (expr.op == "-")
              result.columns.push_back(duckdb::Value::DOUBLE(d1 - d2));
            else if (expr.op == "*")
              result.columns.push_back(duckdb::Value::DOUBLE(d1 * d2));
            else if (expr.op == "/")
              result.columns.push_back(
                  d2 != 0 ? duckdb::Value::DOUBLE(d1 / d2)
                          : duckdb::Value(duckdb::LogicalType::DOUBLE));
            else
              result.columns.push_back(duckdb::Value());
          }
        } else {
          // Column
          size_t idx = std::get<size_t>(item.second);
          if (idx < row.columns.size()) {
            result.columns.push_back(row.columns[idx]);
          } else {
            result.columns.push_back(duckdb::Value());
          }
        }
      }
      return result;
    };

    return std::make_unique<NativeFilterProjectView>(
        def.view_name, def.sql, table, result_schema, predicate, project);
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

    // Add arithmetic expressions to projection
    struct ResolvedExpr {
      size_t left_idx;
      size_t right_idx;
      std::string op;
      bool left_is_const;
      bool right_is_const;
      duckdb::Value left_const;
      duckdb::Value right_const;
    };

    std::unordered_map<std::string, ResolvedExpr> expr_map;
    for (auto &expr : def.projection_exprs) {
      ResolvedExpr res;
      res.left_idx = find_column_index(source_schema, expr.left_col);
      res.right_idx = find_column_index(source_schema, expr.right_col);
      res.op = expr.op;
      res.left_is_const = expr.left_is_const;
      res.right_is_const = expr.right_is_const;
      res.left_const = expr.left_const;
      res.right_const = expr.right_const;
      expr_map[expr.alias] = res;
    }

    // Build projection items map to preserve order
    std::vector<std::pair<bool, std::variant<size_t, ResolvedExpr>>>
        projection_items;
    TableSchema result_schema;
    result_schema.table_name = def.view_name;

    for (const auto &col_name : def.project_column_names) {
      if (expr_map.count(col_name)) {
        // Expression
        projection_items.push_back({true, expr_map[col_name]});
        result_schema.columns.push_back(
            {col_name, duckdb::LogicalType::DOUBLE});
      } else {
        // Column
        size_t idx = find_column_index(source_schema, col_name);
        projection_items.push_back({false, idx});
        if (idx < source_schema.columns.size()) {
          result_schema.columns.push_back(source_schema.columns[idx]);
        } else {
          result_schema.columns.push_back(
              {col_name, duckdb::LogicalType::VARCHAR});
        }
      }
    }

    auto project = [projection_items](const DuckDBRow &row) -> DuckDBRow {
      DuckDBRow result;
      for (const auto &item : projection_items) {
        if (item.first) {
          // Expression
          const auto &expr = std::get<ResolvedExpr>(item.second);
          duckdb::Value v1, v2;

          if (expr.left_is_const) {
            v1 = expr.left_const;
          } else if (expr.left_idx < row.columns.size()) {
            v1 = row.columns[expr.left_idx];
          }

          if (expr.right_is_const) {
            v2 = expr.right_const;
          } else if (expr.right_idx < row.columns.size()) {
            v2 = row.columns[expr.right_idx];
          }

          if (v1.IsNull() || v2.IsNull()) {
            result.columns.push_back(
                duckdb::Value(duckdb::LogicalType::DOUBLE));
          } else {
            double d1 = v1.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                            .GetValue<double>();
            double d2 = v2.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                            .GetValue<double>();
            if (expr.op == "+")
              result.columns.push_back(duckdb::Value::DOUBLE(d1 + d2));
            else if (expr.op == "-")
              result.columns.push_back(duckdb::Value::DOUBLE(d1 - d2));
            else if (expr.op == "*")
              result.columns.push_back(duckdb::Value::DOUBLE(d1 * d2));
            else if (expr.op == "/")
              result.columns.push_back(
                  d2 != 0 ? duckdb::Value::DOUBLE(d1 / d2)
                          : duckdb::Value(duckdb::LogicalType::DOUBLE));
            else
              result.columns.push_back(duckdb::Value());
          }
        } else {
          // Column
          size_t idx = std::get<size_t>(item.second);
          if (idx < row.columns.size()) {
            result.columns.push_back(row.columns[idx]);
          } else {
            result.columns.push_back(duckdb::Value());
          }
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

    // Apply pushed-down filters if present
    // Note: We need to wrap the source tables in filter views conceptually
    // But since we are building the join view which takes table names, we
    // might need to create intermediate filter views. However, the current
    // NativeJoinView takes 'left_table' and 'right_table' strings. If we want
    // to filter simply, we can't easily do it without creating intermediate
    // views registered in the manager.
    //
    // ALTERNATIVE: Just include the pushed down filters in the JoinPredicate?
    // No, that's not "push down". Push down means filtering BEFORE join (or
    // during scan).
    //
    // If we can't create intermediate views easily here (because we don't
    // have the manager), we might have to rely on the fact that
    // NativeJoinView *can* take a filter. BUT that filter is applied *during*
    // the join (or after matching). To truly push down, we want to filter the
    // inputs.
    //
    // A true pushdown in this architecture (views on views) means creating a
    // FilterView for the source, and then joining that FilterView. But
    // create_join_view returns a unique_ptr, it doesn't register views. And
    // it takes table names as strings.
    //
    // If we change the input table name to a new (intermediate) view name, we
    // need to actually CREATE that intermediate view and register it. But we
    // don't have the manager here.
    //
    // OPTION 2: Modify NativeJoinView to accept input-side filters?
    // This is probably the cleanest "local" change without architectural
    // refactoring. We can pass left_filter and right_filter to
    // NativeJoinView.

    // Let's check NativeJoinView again.
    // It has `apply_changes(table_name, changes)`.
    // It can filter changes from left_table before processing.

    // So plan:
    // 1. Modify NativeJoinView to accept left_predicate and right_predicate.
    // 2. In apply_changes, if table == left_table, apply left_predicate to
    // changes.
    //
    // This requires modifying dbsp_duckdb_types.hpp first.

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

    // Build filter lambda for non-equi predicates AND remaining filters
    NativeJoinView::JoinPredicate filter = nullptr;
    if (!join.non_equi_predicates.empty() || !def.filters.empty()) {
      // Resolve column indices for non-equi predicates
      struct ResolvedPredicate {
        size_t left_idx;
        size_t right_idx;
        std::string op;
      };

      // 1. Join non-equi predicates
      std::vector<ResolvedPredicate> resolved_preds;
      for (const auto &pred : join.non_equi_predicates) {
        ResolvedPredicate rp;
        rp.left_idx = find_column_index(left_schema, pred.left_col);
        rp.right_idx = find_column_index(right_schema, pred.right_col);
        rp.op = pred.op;
        resolved_preds.push_back(rp);
      }

      // 2. Remaining filters (post-join filters)
      // These need to be mapped to left or right columns if possible
      // Note: In a join view, we have access to left and right rows
      // separately We need to resolve which side the filter applies to. If
      // it's a simple column ref, find it in left or right schema. Note: We
      // use the original column_name (which might be qualified)
      struct ResolvedFilter {
        bool is_left; // true = left, false = right
        size_t col_idx;
        std::string op;
        duckdb::Value value;
      };
      std::vector<ResolvedFilter> resolved_filters;

      for (const auto &f : def.filters) {
        std::string col = f.column_name;
        // Strip prefix if present
        auto dot = col.find('.');
        if (dot != std::string::npos)
          col = col.substr(dot + 1);

        bool found = false;
        // Check left
        size_t l_idx = find_column_index(left_schema, col);
        if (duckdb::StringUtil::CIEquals(left_schema.columns[l_idx].name,
                                         col)) {
          resolved_filters.push_back({true, l_idx, f.op, f.value});
          found = true;
        } else {
          // Check right
          size_t r_idx = find_column_index(right_schema, col);
          if (duckdb::StringUtil::CIEquals(right_schema.columns[r_idx].name,
                                           col)) {
            resolved_filters.push_back({false, r_idx, f.op, f.value});
            found = true;
          }
        }

        // If not found in either, we ignore it here (should error or handle
        // otherwise) But for now, we assume valid SQL.
      }

      filter = [resolved_preds, resolved_filters](
                   const DuckDBRow &left, const DuckDBRow &right) -> bool {
        // Check non-equi predicates
        for (const auto &pred : resolved_preds) {
          if (pred.left_idx >= left.columns.size() ||
              pred.right_idx >= right.columns.size()) {
            return false;
          }
          const auto &left_val = left.columns[pred.left_idx];
          const auto &right_val = right.columns[pred.right_idx];

          if (left_val.IsNull() || right_val.IsNull()) {
            return false;
          }

          if (!compare_values(left_val, pred.op, right_val)) {
            return false;
          }
        }

        // Check remaining filters
        for (const auto &rf : resolved_filters) {
          const auto &row = rf.is_left ? left : right;
          if (rf.col_idx >= row.columns.size())
            return false;

          const auto &val = row.columns[rf.col_idx];
          if (!compare_values(val, rf.op, rf.value))
            return false;
        }

        return true;
      };
    }

    // Build left pushed-down filter
    NativeJoinView::FilterFn left_filter = nullptr;
    if (!def.left_pushed_filters.empty()) {
      std::vector<std::pair<size_t, ParsedViewDef::FilterInfo>>
          resolved_filters;
      for (const auto &filter : def.left_pushed_filters) {
        size_t col_idx = find_column_index(left_schema, filter.column_name);
        resolved_filters.push_back({col_idx, filter});
      }

      left_filter = [resolved_filters](const DuckDBRow &row) -> bool {
        for (const auto &[col_idx, filter] : resolved_filters) {
          if (col_idx >= row.columns.size())
            return false;
          const auto &col_val = row.columns[col_idx];
          if (!compare_values(col_val, filter.op, filter.value))
            return false;
        }
        return true;
      };
    }

    // Build right pushed-down filter
    NativeJoinView::FilterFn right_filter = nullptr;
    if (!def.right_pushed_filters.empty()) {
      std::vector<std::pair<size_t, ParsedViewDef::FilterInfo>>
          resolved_filters;
      for (const auto &filter : def.right_pushed_filters) {
        size_t col_idx = find_column_index(right_schema, filter.column_name);
        resolved_filters.push_back({col_idx, filter});
      }

      right_filter = [resolved_filters](const DuckDBRow &row) -> bool {
        for (const auto &[col_idx, filter] : resolved_filters) {
          if (col_idx >= row.columns.size())
            return false;
          const auto &col_val = row.columns[col_idx];
          if (!compare_values(col_val, filter.op, filter.value))
            return false;
        }
        return true;
      };
    }

    return std::make_unique<NativeJoinView>(
        def.view_name, def.sql, join.left_table, join.right_table,
        result_schema, left_key, right_key, project, filter, left_filter,
        right_filter);
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
      // Default: All columns
      result_schema = source_schema;
      result_schema.table_name = def.view_name;
    }

    return std::make_unique<NativeDistinctView>(def.view_name, def.sql, table,
                                                result_schema, project);
  }

  static std::unique_ptr<NativeMaterializedView> create_sort_view(
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

    std::vector<NativeSortView::SortColumn> sort_columns;
    for (const auto &sc : def.sort_columns) {
      size_t idx = find_column_index(source_schema, sc.column_name);
      sort_columns.push_back({idx, sc.ascending, sc.nulls_first});
    }

    // Handle projection
    NativeSortView::ProjectFn project = nullptr;
    TableSchema result_schema;
    result_schema.table_name = def.view_name;

    if (!def.project_column_names.empty() && !def.select_all) {
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
      result_schema = source_schema;
      result_schema.table_name = def.view_name;
    }

    return std::make_unique<NativeSortView>(
        def.view_name, def.sql, table, result_schema, sort_columns, project);
  }

  static std::unique_ptr<NativeMaterializedView> create_limit_view(
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

    std::vector<NativeSortView::SortColumn> sort_columns;
    for (const auto &sc : def.sort_columns) {
      size_t idx = find_column_index(source_schema, sc.column_name);
      sort_columns.push_back({idx, sc.ascending, sc.nulls_first});
    }

    // Handle projection
    NativeLimitView::ProjectFn project = nullptr;
    TableSchema result_schema;
    result_schema.table_name = def.view_name;

    if (!def.project_column_names.empty() && !def.select_all) {
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
      result_schema = source_schema;
      result_schema.table_name = def.view_name;
    }

    int64_t limit = def.limit.value_or(-1);
    int64_t offset = def.offset.value_or(0);

    return std::make_unique<NativeLimitView>(def.view_name, def.sql, table,
                                             result_schema, limit, offset,
                                             sort_columns, project);
  }

  static std::unique_ptr<NativeMaterializedView> create_window_view(
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

    std::vector<NativeWindowView::WindowDef> windows;
    for (const auto &win : def.windows) {
      NativeWindowView::WindowDef wdef;
      wdef.function = win.function;
      wdef.alias = win.alias;
      wdef.start = win.start;
      wdef.end = win.end;

      for (const auto &col : win.partition_by) {
        wdef.partition_indices.push_back(find_column_index(source_schema, col));
      }

      for (const auto &ord : win.order_by) {
        NativeSortView::SortColumn sc;
        sc.column_idx = find_column_index(source_schema, ord.column_name);
        sc.ascending = ord.ascending;
        sc.nulls_first = ord.nulls_first;
        wdef.sort_columns.push_back(sc);
      }

      if (!win.arg_column.empty()) {
        wdef.arg_column_idx = find_column_index(source_schema, win.arg_column);
      }
      wdef.offset = win.offset;
      wdef.start_offset = win.start_offset;
      wdef.end_offset = win.end_offset;
      windows.push_back(wdef);
    }

    // Result schema = Source Schema + Window Columns
    TableSchema result_schema = source_schema;
    result_schema.table_name = def.view_name;
    for (size_t i = 0; i < def.windows.size(); ++i) {
      const auto &win = def.windows[i];
      duckdb::LogicalType res_type = duckdb::LogicalType::BIGINT;

      if (win.function == "SUM" || win.function == "MIN" ||
          win.function == "MAX") {
        if (!win.arg_column.empty()) {
          size_t arg_idx = find_column_index(source_schema, win.arg_column);
          if (arg_idx < source_schema.columns.size()) {
            res_type = source_schema.columns[arg_idx].type;
          }
        }
      } else if (win.function == "AVG") {
        res_type = duckdb::LogicalType::DOUBLE;
      }

      result_schema.columns.push_back({win.alias, res_type});
    }

    return std::make_unique<NativeWindowView>(
        def.view_name, def.sql, table, result_schema, source_schema, windows);
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
    // Note: DBSPSqlParser is defined before ViewFactory, so we can use it
    // here
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

    // Apply aliases if present
    if (!rec.column_aliases.empty()) {
      auto &result_schema = recursive_schemas[rec.cte_name];
      for (size_t i = 0; i < rec.column_aliases.size(); ++i) {
        if (i < result_schema.columns.size()) {
          result_schema.columns[i].name = rec.column_aliases[i];
        }
      }
    }

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

  static std::unique_ptr<NativeMaterializedView> create_set_op_view(
      const ParsedViewDef &def,
      const std::unordered_map<std::string, TableSchema> &table_schemas) {
    if (!def.set_op.has_value())
      return nullptr;

    const auto &set_op = def.set_op.value();
    DBSPSqlParser parser;

    // Create left child view
    auto left_res = parser.parse(set_op.left_view, def.view_name + "_L");
    if (!left_res.success)
      return nullptr;
    auto left_view = create_view(left_res.view_def, table_schemas);
    if (!left_view)
      return nullptr;

    // Create right child view
    auto right_res = parser.parse(set_op.right_view, def.view_name + "_R");
    if (!right_res.success)
      return nullptr;
    auto right_view = create_view(right_res.view_def, table_schemas);
    if (!right_view)
      return nullptr;

    // Use left view's schema as result schema
    TableSchema result_schema = left_view->result_schema();
    result_schema.table_name = def.view_name;

    return std::make_unique<NativeSetView>(
        def.view_name, def.sql, std::move(left_view), std::move(right_view),
        result_schema, set_op.type, set_op.all);
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

// DBSP SQL Parser
// Parses SQL view definitions and creates appropriate DBSP views

#pragma once

#include "dbsp_duckdb_types.hpp"

#include "duckdb.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
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
  enum class ViewType { FILTER, PROJECT, AGGREGATE, JOIN, DISTINCT, UNKNOWN };

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
  bool select_all = false;

  // For aggregate views
  struct AggInfo {
    std::string function; // SUM, COUNT, AVG, MIN, MAX
    std::string alias;   // e.g., "revenue" from SUM(...) as revenue
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

  // For join views
  struct JoinInfo {
    std::string left_table;
    std::string right_table;
    std::string left_column;
    std::string right_column;
    size_t left_column_idx;
    size_t right_column_idx;
  };
  std::optional<JoinInfo> join_info;

  // Is DISTINCT present
  bool is_distinct = false;
};

// SQL Parser for DBSP views
class DBSPSqlParser {
public:
  struct ParseResult {
    bool success = false;
    std::string error;
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
    if (node->type != duckdb::QueryNodeType::SELECT_NODE) {
      result.error = "Complex queries not yet supported";
      return result;
    }

    auto &select = node->template Cast<duckdb::SelectNode>();

    // Check for DISTINCT
    for (auto &modifier : select.modifiers) {
      if (modifier->type == duckdb::ResultModifierType::DISTINCT_MODIFIER) {
        result.view_def.is_distinct = true;
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

    // Parse SELECT columns
    parse_select_list(select.select_list, result.view_def);

    // Parse WHERE clause
    if (select.where_clause) {
      parse_where_clause(select.where_clause.get(), result.view_def);
    }

    // Parse GROUP BY
    if (!select.groups.grouping_sets.empty()) {
      parse_group_by(select.groups, result.view_def);
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
    if (expr->type == duckdb::ExpressionType::COMPARE_EQUAL) {
      auto &cmp = expr->template Cast<duckdb::ComparisonExpression>();

      // Extract column references from both sides
      if (cmp.left->type == duckdb::ExpressionType::COLUMN_REF &&
          cmp.right->type == duckdb::ExpressionType::COLUMN_REF) {
        auto &left_col = cmp.left->template Cast<duckdb::ColumnRefExpression>();
        auto &right_col = cmp.right->template Cast<duckdb::ColumnRefExpression>();

        info.left_column = left_col.GetColumnName();
        info.right_column = right_col.GetColumnName();
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
                       child->GetExpressionClass() == duckdb::ExpressionClass::FUNCTION) {
              // Binary expression like quantity * price parsed as function(*, quantity, price)
              auto &child_func = child->template Cast<duckdb::FunctionExpression>();
              std::string op_name = child_func.function_name;
              if ((op_name == "*" || op_name == "+" || op_name == "-" || op_name == "/") &&
                  child_func.children.size() == 2 &&
                  child_func.children[0]->type == duckdb::ExpressionType::COLUMN_REF &&
                  child_func.children[1]->type == duckdb::ExpressionType::COLUMN_REF) {
                auto &left_col = child_func.children[0]->template Cast<duckdb::ColumnRefExpression>();
                auto &right_col = child_func.children[1]->template Cast<duckdb::ColumnRefExpression>();
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
    // Priority: JOIN > AGGREGATE > DISTINCT > FILTER > PROJECT
    if (def.join_info.has_value()) {
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
        {agg.alias.empty() ? agg.function : agg.alias, duckdb::LogicalType::BIGINT});

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
    auto value_fn = [value_col_idx, is_expr, left_col_idx, right_col_idx, expr_op](const DuckDBRow &row) -> duckdb::Value {
      if (is_expr) {
        // Expression-based aggregate: evaluate binary op on two columns
        if (left_col_idx < row.columns.size() && right_col_idx < row.columns.size()) {
          int64_t left_val = row.columns[left_col_idx].GetValue<int64_t>();
          int64_t right_val = row.columns[right_col_idx].GetValue<int64_t>();
          int64_t result = 0;
          if (expr_op == "*") result = left_val * right_val;
          else if (expr_op == "+") result = left_val + right_val;
          else if (expr_op == "-") result = left_val - right_val;
          else if (expr_op == "/" && right_val != 0) result = left_val / right_val;
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

    return std::make_unique<NativeAggregateView>(def.view_name, def.sql, table,
                                                 result_schema, key_fn,
                                                 value_fn, agg_type);
  }

  static std::unique_ptr<NativeMaterializedView> create_join_view(
      const ParsedViewDef &def,
      const std::unordered_map<std::string, TableSchema> &table_schemas) {

    if (!def.join_info.has_value())
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

    size_t left_key_idx = find_column_index(left_schema, join.left_column);
    size_t right_key_idx = find_column_index(right_schema, join.right_column);

    // Create result schema (all columns from both tables)
    TableSchema result_schema;
    result_schema.table_name = def.view_name;
    for (const auto &col : left_schema.columns) {
      result_schema.columns.push_back(col);
    }
    for (const auto &col : right_schema.columns) {
      result_schema.columns.push_back(col);
    }

    // Key functions
    auto left_key = [left_key_idx](const DuckDBRow &row) -> duckdb::Value {
      if (left_key_idx < row.columns.size()) {
        return row.columns[left_key_idx];
      }
      return duckdb::Value();
    };

    auto right_key = [right_key_idx](const DuckDBRow &row) -> duckdb::Value {
      if (right_key_idx < row.columns.size()) {
        return row.columns[right_key_idx];
      }
      return duckdb::Value();
    };

    return std::make_unique<NativeJoinView>(def.view_name, def.sql,
                                            join.left_table, join.right_table,
                                            result_schema, left_key, right_key);
  }

  static std::unique_ptr<NativeMaterializedView> create_distinct_view(
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

    return std::make_unique<NativeDistinctView>(def.view_name, def.sql, table,
                                                schema);
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

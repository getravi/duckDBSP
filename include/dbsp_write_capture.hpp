// Statement-shape vetting and capture-query construction for the O(Δ)
// UPDATE/DELETE auto-sync fast path (docs/DESIGN_WRITE_CAPTURE.md).
//
// A capturable statement is rewritten into ONE SELECT that reads the
// statement's old row images and, for UPDATE, computes the new images by
// projecting the SET expressions cast to their column types:
//   UPDATE t SET c2 = e WHERE p
//     -> SELECT rowid, c1, c2, ..., CAST((e) AS <type(c2)>) FROM t WHERE (p)
//   DELETE FROM t WHERE p
//     -> SELECT rowid, c1, c2, ... FROM t WHERE (p)
// The capture query runs on an internal connection against committed
// state, so callers must ensure the target table has no transaction-local
// changes (DBSPContextState poisons capture otherwise). Rowids feed only
// the commit guard, never the delta: UPDATE shapes DuckDB would execute
// as delete+re-append (SET column in an index, or a type without regular
// update support) are rejected here because their rowids are unstable.

#pragma once

#include "duckdb.hpp"
// canonical_table_key (dbsp_qualified_name.hpp) walks entry->schema->catalog:
// the schema entry must be complete before that header is parsed
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

#include "dbsp_qualified_name.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dbsp_native {

struct WriteCapturePlan {
  enum class Kind { Update, Delete };
  Kind kind;
  std::string capture_sql;
  size_t n_cols = 0; // physical column count of the target table
  // UPDATE only: (physical column index, capture-projection index) per SET
  // column; projection layout is [0]=rowid, [1..n_cols]=old columns, then
  // one new value per SET column.
  std::vector<std::pair<size_t, size_t>> set_cols;
};

// Subqueries read other tables (whose transaction-local state the capture
// SELECT cannot see), parameters have no value at parse time, DEFAULT
// needs default-expression resolution: all uncapturable.
inline bool parsed_expr_capturable(const duckdb::ParsedExpression &expr) {
  const auto cls = expr.GetExpressionClass();
  if (cls == duckdb::ExpressionClass::SUBQUERY ||
      cls == duckdb::ExpressionClass::PARAMETER ||
      cls == duckdb::ExpressionClass::DEFAULT) {
    return false;
  }
  bool ok = true;
  duckdb::ParsedExpressionIterator::EnumerateChildren(
      expr, [&](const duckdb::ParsedExpression &child) {
        if (ok && !parsed_expr_capturable(child)) {
          ok = false;
        }
      });
  return ok;
}

// True when every scalar function in a bound plan is CONSISTENT — i.e.
// re-evaluating it in a different query yields identical results.
// CONSISTENT_WITHIN_QUERY (now() and friends) is NOT enough: the capture
// SELECT is a different query from the user's statement.
inline bool bound_expr_consistent(const duckdb::Expression &expr) {
  if (expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_FUNCTION) {
    auto &fn = expr.Cast<duckdb::BoundFunctionExpression>();
    if (fn.function.GetStability() != duckdb::FunctionStability::CONSISTENT) {
      return false;
    }
  }
  bool ok = true;
  duckdb::ExpressionIterator::EnumerateChildren(
      expr, [&](const duckdb::Expression &child) {
        if (ok && !bound_expr_consistent(child)) {
          ok = false;
        }
      });
  return ok;
}

inline bool bound_plan_consistent(const duckdb::LogicalOperator &op) {
  for (auto &expr : op.expressions) {
    if (!bound_expr_consistent(*expr)) {
      return false;
    }
  }
  for (auto &child : op.children) {
    if (!bound_plan_consistent(*child)) {
      return false;
    }
  }
  return true;
}

inline std::string quote_ident(const std::string &name) {
  return "\"" + duckdb::KeywordHelper::EscapeQuotes(name, '"') + "\"";
}

// nullptr = shape not capturable (caller falls back to scan-and-diff).
// `context` needs an active transaction (GetStorageInfo reads the catalog).
inline std::unique_ptr<WriteCapturePlan>
plan_write_capture(duckdb::ClientContext &context, duckdb::SQLStatement &stmt,
                   duckdb::TableCatalogEntry &entry,
                   const std::string &table_key) {
  auto plan = std::make_unique<WriteCapturePlan>();
  const duckdb::ParsedExpression *condition = nullptr;
  const duckdb::TableRef *target = nullptr;
  duckdb::UpdateSetInfo *set_info = nullptr;

  switch (stmt.type) {
  case duckdb::StatementType::UPDATE_STATEMENT: {
    auto &upd = stmt.Cast<duckdb::UpdateStatement>();
    if (upd.from_table || !upd.returning_list.empty() ||
        !upd.cte_map.map.empty() || !upd.set_info) {
      return nullptr;
    }
    plan->kind = WriteCapturePlan::Kind::Update;
    target = upd.table.get();
    set_info = upd.set_info.get();
    condition = set_info->condition.get();
    break;
  }
  case duckdb::StatementType::DELETE_STATEMENT: {
    auto &del = stmt.Cast<duckdb::DeleteStatement>();
    if (!del.using_clauses.empty() || !del.returning_list.empty() ||
        !del.cte_map.map.empty()) {
      return nullptr;
    }
    plan->kind = WriteCapturePlan::Kind::Delete;
    target = del.table.get();
    condition = del.condition.get();
    break;
  }
  default:
    return nullptr;
  }

  if (!target || target->type != duckdb::TableReferenceType::BASE_TABLE) {
    return nullptr;
  }
  if (condition && !parsed_expr_capturable(*condition)) {
    return nullptr;
  }

  // The guard re-verifies captured rowids; a user column named "rowid"
  // shadows the pseudo-column, so the guard could not run.
  for (auto &col : entry.GetColumns().Physical()) {
    if (duckdb::StringUtil::Lower(col.Name()) == "rowid") {
      return nullptr;
    }
  }

  std::string sql = "SELECT rowid";
  size_t n_cols = 0;
  for (auto &col : entry.GetColumns().Physical()) {
    sql += ", " + quote_ident(col.Name());
    n_cols++;
  }
  plan->n_cols = n_cols;

  if (set_info) {
    if (set_info->columns.size() != set_info->expressions.size() ||
        set_info->columns.empty()) {
      return nullptr;
    }
    duckdb::TableStorageInfo storage_info = entry.GetStorageInfo(context);
    for (size_t j = 0; j < set_info->columns.size(); j++) {
      if (!parsed_expr_capturable(*set_info->expressions[j])) {
        return nullptr;
      }
      if (!entry.ColumnExists(set_info->columns[j])) {
        return nullptr;
      }
      auto &col = entry.GetColumn(set_info->columns[j]);
      if (col.Generated()) {
        return nullptr;
      }
      // DuckDB executes these shapes as delete+re-append
      // (update_is_del_and_insert): rowids are unstable, the guard cannot
      // re-verify them.
      if (!col.Type().SupportsRegularUpdate()) {
        return nullptr;
      }
      const auto physical = col.Physical().index;
      for (auto &index : storage_info.index_info) {
        if (index.column_set.find(physical) != index.column_set.end()) {
          return nullptr;
        }
      }
      sql += ", CAST((" + set_info->expressions[j]->ToString() + ") AS " +
             col.Type().ToString() + ")";
      plan->set_cols.emplace_back(physical, n_cols + 1 + j);
    }
  }

  sql += " FROM " + quote_table_key(table_key);
  // SET/WHERE expressions may qualify columns with the statement's alias
  const auto &alias = target->alias;
  if (!alias.empty()) {
    sql += " AS " + quote_ident(alias);
  }
  if (condition) {
    sql += " WHERE (" + condition->ToString() + ")";
  }
  plan->capture_sql = std::move(sql);
  return plan;
}

} // namespace dbsp_native

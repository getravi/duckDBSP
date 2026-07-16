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
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dbsp_native {

struct WriteCapturePlan {
  enum class Kind { Update, Delete, Insert, Upsert };
  Kind kind;
  std::string capture_sql;
  size_t n_cols = 0; // physical column count of the target table
  // UPDATE/Upsert: (physical column index, capture-projection index) per
  // SET column; projection layout is [0]=rowid, [1..n_cols]=old columns,
  // then one new value per SET column. INSERT projects the new rows
  // directly in table column order (no rowid — the commit guard is seq +
  // count). Upsert additionally carries the insert image (the padded
  // source row in table order) at slots [insert_slot_base ..
  // insert_slot_base+n_cols-1], with rowid NULL marking unmatched
  // (insert-part) rows; empty set_cols with Kind::Upsert = DO NOTHING.
  std::vector<std::pair<size_t, size_t>> set_cols;
  size_t insert_slot_base = 0;
};

inline bool source_node_capturable(const duckdb::QueryNode &node);
inline bool source_ref_capturable(const duckdb::TableRef &ref);

// Parameters have no value at parse time, DEFAULT needs default-
// expression resolution, and window output over tied sort keys depends
// on scan order: always uncapturable. Subqueries read OTHER tables —
// capturable only when the statement sees pure committed state
// (autocommit, or an explicit transaction with no prior writes) AND the
// subquery's own row source is repeatable.
inline bool parsed_expr_capturable_ex(const duckdb::ParsedExpression &expr,
                                      bool allow_subqueries) {
  const auto cls = expr.GetExpressionClass();
  if (cls == duckdb::ExpressionClass::PARAMETER ||
      cls == duckdb::ExpressionClass::DEFAULT ||
      cls == duckdb::ExpressionClass::WINDOW) {
    return false;
  }
  if (cls == duckdb::ExpressionClass::SUBQUERY) {
    if (!allow_subqueries) {
      return false;
    }
    auto &sub = expr.Cast<duckdb::SubqueryExpression>();
    if (!sub.subquery || !sub.subquery->node ||
        !source_node_capturable(*sub.subquery->node)) {
      return false;
    }
    // fall through: EnumerateChildren covers the IN/comparison child
  }
  bool ok = true;
  duckdb::ParsedExpressionIterator::EnumerateChildren(
      expr, [&](const duckdb::ParsedExpression &child) {
        if (ok && !parsed_expr_capturable_ex(child, allow_subqueries)) {
          ok = false;
        }
      });
  return ok;
}

inline bool parsed_expr_capturable(const duckdb::ParsedExpression &expr) {
  return parsed_expr_capturable_ex(expr, false);
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
  // VALUES lists live in LogicalExpressionGet::expressions, a
  // vector-of-rows member that SHADOWS the base-class expressions walked
  // above — without this a volatile function inside VALUES slips through
  if (op.type == duckdb::LogicalOperatorType::LOGICAL_EXPRESSION_GET) {
    auto &get = op.Cast<duckdb::LogicalExpressionGet>();
    for (auto &row : get.expressions) {
      for (auto &expr : row) {
        if (!bound_expr_consistent(*expr)) {
          return false;
        }
      }
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
// `allow_subqueries` = the statement sees pure committed state (autocommit
// or a write-free transaction so far): subqueries in WHERE/SET and DELETE
// ... USING (rewritten to a correlated EXISTS probe) become capturable.
inline std::unique_ptr<WriteCapturePlan>
plan_write_capture(duckdb::ClientContext &context, duckdb::SQLStatement &stmt,
                   duckdb::TableCatalogEntry &entry,
                   const std::string &table_key, bool allow_subqueries) {
  auto plan = std::make_unique<WriteCapturePlan>();
  const duckdb::ParsedExpression *condition = nullptr;
  const duckdb::TableRef *target = nullptr;
  duckdb::UpdateSetInfo *set_info = nullptr;
  const std::vector<duckdb::unique_ptr<duckdb::TableRef>> *using_refs =
      nullptr;

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
    if (!del.returning_list.empty() || !del.cte_map.map.empty()) {
      return nullptr;
    }
    if (!del.using_clauses.empty()) {
      // DELETE ... USING is a semi-join delete: rewritable as EXISTS,
      // but only with a committed-state view of the USING tables
      if (!allow_subqueries) {
        return nullptr;
      }
      for (auto &ref : del.using_clauses) {
        if (!ref || !source_ref_capturable(*ref)) {
          return nullptr;
        }
      }
      using_refs = &del.using_clauses;
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
  if (condition && !parsed_expr_capturable_ex(*condition, allow_subqueries)) {
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
      if (!parsed_expr_capturable_ex(*set_info->expressions[j],
                                     allow_subqueries)) {
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
  if (using_refs) {
    // DELETE t USING u WHERE cond  ==  delete every t row with at least
    // one matching u row: EXISTS(SELECT 1 FROM u WHERE cond), with the
    // full condition (it references both sides) inside the probe
    sql += " WHERE EXISTS (SELECT 1 FROM ";
    bool first = true;
    for (auto &ref : *using_refs) {
      sql += (first ? "" : ", ") + ref->ToString();
      first = false;
    }
    if (condition) {
      sql += " WHERE (" + condition->ToString() + ")";
    }
    sql += ")";
  } else if (condition) {
    sql += " WHERE (" + condition->ToString() + ")";
  }
  plan->capture_sql = std::move(sql);
  return plan;
}

// Deterministic-source vetting for INSERT ... SELECT capture: the source
// runs twice (capture SELECT, then the statement), so its ROW SET must be
// repeatable. LIMIT/SAMPLE pick rows by scan order (parallel scans make
// that non-repeatable), table functions carry no stability metadata, and
// CTE/pivot/show refs are out of whitelist scope.
inline bool source_node_capturable(const duckdb::QueryNode &node);

inline bool source_ref_capturable(const duckdb::TableRef &ref) {
  if (ref.sample) {
    return false;
  }
  switch (ref.type) {
  case duckdb::TableReferenceType::BASE_TABLE:
  case duckdb::TableReferenceType::EMPTY_FROM:
  case duckdb::TableReferenceType::EXPRESSION_LIST:
    return true;
  case duckdb::TableReferenceType::JOIN: {
    auto &join = ref.Cast<duckdb::JoinRef>();
    if (join.condition && !parsed_expr_capturable(*join.condition)) {
      return false;
    }
    return source_ref_capturable(*join.left) &&
           source_ref_capturable(*join.right);
  }
  case duckdb::TableReferenceType::SUBQUERY: {
    auto &sub = ref.Cast<duckdb::SubqueryRef>();
    return sub.subquery && sub.subquery->node &&
           source_node_capturable(*sub.subquery->node);
  }
  default:
    return false;
  }
}

inline bool source_node_capturable(const duckdb::QueryNode &node) {
  if (!node.cte_map.map.empty()) {
    return false;
  }
  for (auto &mod : node.modifiers) {
    if (mod->type == duckdb::ResultModifierType::LIMIT_MODIFIER ||
        mod->type == duckdb::ResultModifierType::LIMIT_PERCENT_MODIFIER) {
      return false; // which rows survive depends on scan order
    }
  }
  if (node.type == duckdb::QueryNodeType::SELECT_NODE) {
    auto &sel = node.Cast<duckdb::SelectNode>();
    if (sel.sample) {
      return false;
    }
    for (auto &expr : sel.select_list) {
      if (!parsed_expr_capturable(*expr)) {
        return false;
      }
    }
    if (sel.where_clause && !parsed_expr_capturable(*sel.where_clause)) {
      return false;
    }
    if (sel.having && !parsed_expr_capturable(*sel.having)) {
      return false;
    }
    if (sel.qualify && !parsed_expr_capturable(*sel.qualify)) {
      return false;
    }
    for (auto &expr : sel.groups.group_expressions) {
      if (!parsed_expr_capturable(*expr)) {
        return false;
      }
    }
    return !sel.from_table || source_ref_capturable(*sel.from_table);
  }
  if (node.type == duckdb::QueryNodeType::SET_OPERATION_NODE) {
    auto &setop = node.Cast<duckdb::SetOperationNode>();
    for (auto &child : setop.children) {
      if (!child || !source_node_capturable(*child)) {
        return false;
      }
    }
    return true;
  }
  return false; // recursive CTE nodes and friends
}

// The vetted row source of an INSERT/upsert: the source SQL (a VALUES
// list or deterministic SELECT, parenthesized), the statement's column
// names in supply order, and the lowercased set of provided columns.
struct InsertSource {
  std::string source_sql;
  std::vector<std::string> value_names;
  std::unordered_set<std::string> provided;
};

inline bool build_insert_source(duckdb::InsertStatement &ins,
                                duckdb::TableCatalogEntry &entry,
                                InsertSource &out) {
  if (!ins.returning_list.empty() || !ins.cte_map.map.empty() ||
      ins.default_values ||
      ins.column_order != duckdb::InsertColumnOrder::INSERT_BY_POSITION ||
      !ins.select_statement || !ins.select_statement->node) {
    return false;
  }
  out.value_names = ins.columns;
  if (out.value_names.empty()) {
    for (auto &col : entry.GetColumns().Physical()) {
      out.value_names.push_back(col.Name());
    }
  }
  for (const auto &name : out.value_names) {
    if (!entry.ColumnExists(name) || entry.GetColumn(name).Generated()) {
      return false;
    }
    if (!out.provided.insert(duckdb::StringUtil::Lower(name)).second) {
      return false; // duplicate column name
    }
  }

  auto values = ins.GetValuesList();
  if (values) {
    if (values->values.empty()) {
      return false;
    }
    std::string rows;
    for (const auto &row : values->values) {
      if (row.size() != out.value_names.size()) {
        return false;
      }
      rows += rows.empty() ? "(" : ", (";
      for (size_t i = 0; i < row.size(); i++) {
        if (!parsed_expr_capturable(*row[i])) {
          return false;
        }
        rows += (i ? ", " : "") + row[i]->ToString();
      }
      rows += ")";
    }
    out.source_sql = "(VALUES " + rows + ")";
    return true;
  }
  if (!source_node_capturable(*ins.select_statement->node)) {
    return false;
  }
  // A source-arity/column-list mismatch makes the capture SELECT (or
  // the statement itself) error out — declined at execution, no risk
  out.source_sql = "(" + ins.select_statement->node->ToString() + ")";
  return true;
}

// One projected value per table column, in table order: the provided
// source column (via `alias`.), the declared DEFAULT, or NULL — all cast
// to the column type. Empty string = an uncapturable default expression.
inline std::string insert_image_projection(duckdb::TableCatalogEntry &entry,
                                           const InsertSource &src,
                                           const std::string &alias) {
  std::string proj;
  for (auto &col : entry.GetColumns().Physical()) {
    proj += proj.empty() ? "" : ", ";
    if (src.provided.count(duckdb::StringUtil::Lower(col.Name()))) {
      proj += "CAST(" + alias + "." + quote_ident(col.Name()) + " AS " +
              col.Type().ToString() + ")";
    } else if (col.HasDefaultValue()) {
      if (!parsed_expr_capturable(col.DefaultValue())) {
        return {};
      }
      proj += "CAST((" + col.DefaultValue().ToString() + ") AS " +
              col.Type().ToString() + ")";
    } else {
      proj += "CAST(NULL AS " + col.Type().ToString() + ")";
    }
  }
  return proj;
}

// Autocommit INSERT capture: evaluate the statement's row source (a
// VALUES list or a whitelisted deterministic SELECT) via one internal
// SELECT, projected into table column order with the INSERT's own
// to-column-type casts. Columns missing from a partial column list take
// their declared DEFAULT (or NULL) — volatile defaults like nextval()
// fail the bound-plan stability check downstream.
//   INSERT INTO t (b, a) VALUES (e1, e2), ...
//     -> SELECT CAST(v."a" AS ...), CAST(v."b" AS ...), <default/NULL...>
//        FROM (VALUES (e1, e2), ...) v("b", "a")
// nullptr = not capturable. Explicit transactions never need this — the
// G2 LocalStorage scan is exact there.
inline std::unique_ptr<WriteCapturePlan>
plan_insert_capture(duckdb::SQLStatement &stmt,
                    duckdb::TableCatalogEntry &entry) {
  if (stmt.type != duckdb::StatementType::INSERT_STATEMENT) {
    return nullptr;
  }
  auto &ins = stmt.Cast<duckdb::InsertStatement>();
  if (ins.on_conflict_info) {
    return nullptr; // upserts have their own planner
  }
  InsertSource src;
  if (!build_insert_source(ins, entry, src)) {
    return nullptr;
  }
  const std::string proj = insert_image_projection(entry, src, "v");
  if (proj.empty()) {
    return nullptr;
  }

  auto plan = std::make_unique<WriteCapturePlan>();
  plan->kind = WriteCapturePlan::Kind::Insert;
  plan->n_cols = entry.GetColumns().PhysicalColumnCount();

  std::string alias_cols;
  for (const auto &name : src.value_names) {
    alias_cols += (alias_cols.empty() ? "" : ", ") + quote_ident(name);
  }
  plan->capture_sql =
      "SELECT " + proj + " FROM " + src.source_sql + " v(" + alias_cols + ")";
  return plan;
}

// Upsert (INSERT ... ON CONFLICT) capture: probe committed state with a
// LEFT JOIN from the row source (aliased `excluded`, so the statement's
// own excluded.* references bind unchanged) to the target on the conflict
// columns:
//   [0] target rowid (NULL = no conflict: insert-part row)
//   [1..n] target old image (matched rows)
//   [n+1..2n] insert image (padded source row in table order)
//   [2n+1..] DO UPDATE SET values, cast to column types
// Unqualified target-column references in SET are ambiguous in this
// SELECT (both sides expose them) and decline via query error; the
// common excluded.-qualified form captures. DO NOTHING carries no SET
// slots — matched rows contribute nothing. Duplicate conflict keys
// INSIDE the source: DO UPDATE errors in DuckDB (statement never
// commits), DO NOTHING inserts only one row while the capture predicts
// two — the signed COUNT(*) guard catches that and falls back.
inline std::unique_ptr<WriteCapturePlan>
plan_upsert_capture(duckdb::ClientContext &context, duckdb::SQLStatement &stmt,
                    duckdb::TableCatalogEntry &entry,
                    const std::string &table_key) {
  if (stmt.type != duckdb::StatementType::INSERT_STATEMENT) {
    return nullptr;
  }
  auto &ins = stmt.Cast<duckdb::InsertStatement>();
  if (!ins.on_conflict_info) {
    return nullptr;
  }
  auto &conflict = *ins.on_conflict_info;
  const bool do_update =
      conflict.action_type == duckdb::OnConflictAction::UPDATE;
  if (!do_update &&
      conflict.action_type != duckdb::OnConflictAction::NOTHING) {
    return nullptr; // OR REPLACE and friends
  }
  if (conflict.condition || conflict.indexed_columns.empty()) {
    return nullptr; // conditional DO ..., or no explicit conflict target
  }
  if (do_update && !conflict.set_info) {
    return nullptr;
  }
  for (auto &col : entry.GetColumns().Physical()) {
    if (duckdb::StringUtil::Lower(col.Name()) == "rowid") {
      return nullptr; // shadows the guard pseudo-column
    }
  }
  InsertSource src;
  if (!build_insert_source(ins, entry, src)) {
    return nullptr;
  }
  // conflict columns must be provided by the source (the join needs them)
  for (const auto &name : conflict.indexed_columns) {
    if (!src.provided.count(duckdb::StringUtil::Lower(name))) {
      return nullptr;
    }
  }

  auto plan = std::make_unique<WriteCapturePlan>();
  plan->kind = WriteCapturePlan::Kind::Upsert;
  plan->n_cols = entry.GetColumns().PhysicalColumnCount();
  plan->insert_slot_base = plan->n_cols + 1;

  std::string sql = "SELECT t.rowid";
  for (auto &col : entry.GetColumns().Physical()) {
    sql += ", t." + quote_ident(col.Name());
  }
  const std::string insert_proj =
      insert_image_projection(entry, src, "excluded");
  if (insert_proj.empty()) {
    return nullptr;
  }
  sql += ", " + insert_proj;
  if (do_update) {
    auto &set = *conflict.set_info;
    if (set.condition || set.columns.size() != set.expressions.size() ||
        set.columns.empty()) {
      return nullptr;
    }
    duckdb::TableStorageInfo storage_info = entry.GetStorageInfo(context);
    for (size_t j = 0; j < set.columns.size(); j++) {
      if (!parsed_expr_capturable(*set.expressions[j])) {
        return nullptr;
      }
      if (!entry.ColumnExists(set.columns[j])) {
        return nullptr;
      }
      auto &col = entry.GetColumn(set.columns[j]);
      if (col.Generated() || !col.Type().SupportsRegularUpdate()) {
        return nullptr;
      }
      // SET on an indexed column executes as delete+re-append: unstable
      // rowids, the guard cannot re-verify — same rule as plain UPDATE
      const auto physical = col.Physical().index;
      for (auto &index : storage_info.index_info) {
        if (index.column_set.find(physical) != index.column_set.end()) {
          return nullptr;
        }
      }
      sql += ", CAST((" + set.expressions[j]->ToString() + ") AS " +
             col.Type().ToString() + ")";
      plan->set_cols.emplace_back(physical, 2 * plan->n_cols + 1 + j);
    }
  }

  std::string alias_cols;
  for (const auto &name : src.value_names) {
    alias_cols += (alias_cols.empty() ? "" : ", ") + quote_ident(name);
  }
  sql += " FROM " + src.source_sql + " excluded(" + alias_cols +
         ") LEFT JOIN " + quote_table_key(table_key) + " t ON ";
  bool first = true;
  for (const auto &name : conflict.indexed_columns) {
    sql += first ? "" : " AND ";
    first = false;
    sql += "t." + quote_ident(name) + " = excluded." + quote_ident(name);
  }
  plan->capture_sql = std::move(sql);
  return plan;
}

} // namespace dbsp_native

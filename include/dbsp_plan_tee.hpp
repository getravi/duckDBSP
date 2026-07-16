// D2 plan tee (docs/DESIGN_WRITE_CAPTURE.md, "design 2"): an
// OptimizerExtension that widens a tracked-table DELETE's child plan to
// carry the full old row images and injects a pass-through extension
// operator that copies (rowid, old row) into the connection's
// DBSPContextState as the plan executes. Teed rows are EXACT — they are
// what the statement actually processed — so they capture the shapes the
// design-1 pre-image SELECT must decline: predicates with prepared
// parameters or volatile functions, subqueries after a same-transaction
// write, USING probes over transaction-local state, and repeated writes
// to one table in a transaction. Design 1 stays the first choice (no
// plan mutation); the tee only arms when it declined.
//
// Phase 1 covers LogicalDelete. UPDATE needs the same child widening
// plus the SET-value columns already present in its child projection —
// tracked in the design doc.

#pragma once

#include "dbsp_context_state.hpp"
#include "duckdb.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <memory>
#include <string>
#include <vector>

namespace dbsp_native {

inline duckdb::shared_ptr<DBSPContextState>
tee_context_state(duckdb::ClientContext &context) {
  return context.registered_state->Get<DBSPContextState>("dbsp_cdc_state");
}

// Pass-through operator: forwards every chunk unchanged while copying
// (rowid, old row) into the connection's tee buffer. Runs on execution
// threads — the buffer locks per chunk.
class PhysicalTee : public duckdb::PhysicalOperator {
public:
  PhysicalTee(duckdb::PhysicalPlan &physical_plan,
              duckdb::vector<duckdb::LogicalType> types,
              duckdb::idx_t estimated_cardinality, duckdb::idx_t rowid_pos,
              std::vector<duckdb::idx_t> col_pos)
      : duckdb::PhysicalOperator(physical_plan,
                                 duckdb::PhysicalOperatorType::EXTENSION,
                                 std::move(types), estimated_cardinality),
        rowid_pos(rowid_pos), col_pos(std::move(col_pos)) {}

  duckdb::idx_t rowid_pos;
  std::vector<duckdb::idx_t> col_pos; // old row image, table column order

  duckdb::unique_ptr<duckdb::OperatorState>
  GetOperatorState(duckdb::ExecutionContext &context) const override {
    return duckdb::make_uniq<duckdb::OperatorState>();
  }

  duckdb::OperatorResultType
  Execute(duckdb::ExecutionContext &context, duckdb::DataChunk &input,
          duckdb::DataChunk &chunk, duckdb::GlobalOperatorState &gstate,
          duckdb::OperatorState &state) const override {
    auto ctx_state = tee_context_state(context.client);
    if (ctx_state) {
      for (duckdb::idx_t i = 0; i < input.size(); i++) {
        DuckDBRow row;
        row.columns.reserve(col_pos.size());
        for (const auto pos : col_pos) {
          row.columns.push_back(input.GetValue(pos, i));
        }
        ctx_state->tee_add_delete(
            input.GetValue(rowid_pos, i).GetValue<int64_t>(), std::move(row));
      }
    }
    chunk.Reference(input); // pass through unchanged
    return duckdb::OperatorResultType::NEED_MORE_INPUT;
  }

  bool ParallelOperator() const override { return true; }

  duckdb::string GetName() const override { return "DBSP_TEE"; }
};

class LogicalTee : public duckdb::LogicalExtensionOperator {
public:
  LogicalTee(duckdb::unique_ptr<duckdb::LogicalOperator> child,
             duckdb::idx_t rowid_pos, std::vector<duckdb::idx_t> col_pos)
      : rowid_pos(rowid_pos), col_pos(std::move(col_pos)) {
    children.push_back(std::move(child));
  }

  duckdb::idx_t rowid_pos;
  std::vector<duckdb::idx_t> col_pos;

  duckdb::vector<duckdb::ColumnBinding> GetColumnBindings() override {
    return children[0]->GetColumnBindings();
  }

  duckdb::string GetExtensionName() const override { return "dbsp_tee"; }

  duckdb::PhysicalOperator &
  CreatePlan(duckdb::ClientContext &context,
             duckdb::PhysicalPlanGenerator &planner) override {
    auto &child = planner.CreatePlan(*children[0]);
    auto &op = planner.Make<PhysicalTee>(
        child.types, child.estimated_cardinality, rowid_pos, col_pos);
    op.children.push_back(child);
    return op;
  }

protected:
  void ResolveTypes() override { types = children[0]->types; }
};

// Widen the DELETE child chain so the full old row reaches the tee.
// Supported chain: {FILTER | PROJECTION | left side of a join}* over one
// LOGICAL_GET on the target table (subquery predicates plan as semi/mark
// joins with the target scan on the left, whose bindings pass through
// every join type). Appends every table column to the GET and re-exposes
// the appended columns through each intermediate operator; returns their
// positions in the child's final output. Any other plan shape declines
// (the scan fallback stays correct).
inline bool tee_join_descendable(duckdb::LogicalOperatorType type) {
  return type == duckdb::LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
         type == duckdb::LogicalOperatorType::LOGICAL_DELIM_JOIN ||
         type == duckdb::LogicalOperatorType::LOGICAL_ANY_JOIN ||
         type == duckdb::LogicalOperatorType::LOGICAL_CROSS_PRODUCT;
}

inline bool widen_delete_child(duckdb::LogicalOperator &child,
                               duckdb::TableCatalogEntry &table,
                               std::vector<duckdb::idx_t> &out_positions) {
  // locate the chain down to the GET
  std::vector<duckdb::LogicalOperator *> chain;
  duckdb::LogicalOperator *op = &child;
  while (true) {
    if (op->type == duckdb::LogicalOperatorType::LOGICAL_GET) {
      break;
    }
    if (tee_join_descendable(op->type)) {
      if (op->children.empty()) {
        return false;
      }
      chain.push_back(op); // joins pass left bindings through untouched
      op = op->children[0].get();
      continue;
    }
    if (op->type != duckdb::LogicalOperatorType::LOGICAL_FILTER &&
        op->type != duckdb::LogicalOperatorType::LOGICAL_PROJECTION) {
      return false;
    }
    if (op->children.size() != 1) {
      return false;
    }
    chain.push_back(op);
    op = op->children[0].get();
  }
  auto &get = op->Cast<duckdb::LogicalGet>();
  auto get_table = get.GetTable();
  if (!get_table || get_table.get() != &table) {
    return false;
  }
  // Generated columns make storage column ids diverge from the tracked
  // row layout — decline (rare; the scan fallback handles it)
  if (table.GetColumns().PhysicalColumnCount() !=
      table.GetColumns().LogicalColumnCount()) {
    return false;
  }

  // append every table column to the GET (duplicates with existing
  // entries are fine — simpler than dedup + remapping)
  const auto prev = get.GetColumnIds().size();
  const auto n_cols = table.GetColumns().PhysicalColumnCount();
  std::vector<duckdb::ColumnBinding> appended;
  for (duckdb::idx_t c = 0; c < n_cols; c++) {
    get.AddColumnId(c);
    appended.emplace_back(get.table_index, prev + c);
  }

  // re-expose through the chain, bottom-up. Joins pass left bindings
  // through untouched — nothing to extend there.
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    auto &node = **it;
    if (tee_join_descendable(node.type)) {
      continue;
    }
    if (node.type == duckdb::LogicalOperatorType::LOGICAL_FILTER) {
      auto &filter = node.Cast<duckdb::LogicalFilter>();
      if (!filter.projection_map.empty()) {
        // map entries index into the child's output: locate each
        // appended binding there (a join below may have put right-side
        // columns after them, so never assume they sit at the end)
        const auto child_bindings = node.children[0]->GetColumnBindings();
        for (const auto &target : appended) {
          bool found = false;
          for (duckdb::idx_t i = 0; i < child_bindings.size(); i++) {
            if (child_bindings[i] == target) {
              filter.projection_map.push_back(i);
              found = true;
              break;
            }
          }
          if (!found) {
            return false;
          }
        }
      }
      // bindings pass through: appended stay as-is
    } else { // LOGICAL_PROJECTION
      auto &proj = node.Cast<duckdb::LogicalProjection>();
      for (duckdb::idx_t c = 0; c < n_cols; c++) {
        proj.expressions.push_back(
            duckdb::make_uniq<duckdb::BoundColumnRefExpression>(
                table.GetColumns().GetColumnTypes()[c], appended[c]));
        appended[c] = duckdb::ColumnBinding(proj.table_index,
                                            proj.expressions.size() - 1);
      }
    }
  }

  // positions of the appended columns in the child's final output
  const auto bindings = child.GetColumnBindings();
  out_positions.clear();
  for (const auto &target : appended) {
    bool found = false;
    for (duckdb::idx_t i = 0; i < bindings.size(); i++) {
      if (bindings[i] == target) {
        out_positions.push_back(i);
        found = true;
        break;
      }
    }
    if (!found) {
      return false; // widened columns must all be visible
    }
  }
  return true;
}

inline void tee_walk(duckdb::ClientContext &context, CDCManager &manager,
                     DBSPContextState &state,
                     duckdb::unique_ptr<duckdb::LogicalOperator> &op) {
  if (op->type == duckdb::LogicalOperatorType::LOGICAL_DELETE) {
    auto &del = op->Cast<duckdb::LogicalDelete>();
    const std::string key = canonical_table_key(del.table);
    if (manager.is_table_tracked(key) && !del.children.empty() &&
        !del.expressions.empty() &&
        del.expressions[0]->GetExpressionClass() ==
            duckdb::ExpressionClass::BOUND_COLUMN_REF) {
      std::vector<duckdb::idx_t> col_pos;
      if (widen_delete_child(*del.children[0], del.table, col_pos)) {
        // rowid position AFTER widening (bindings may have grown)
        const auto rowid_binding =
            del.expressions[0]
                ->Cast<duckdb::BoundColumnRefExpression>()
                .binding;
        const auto bindings = del.children[0]->GetColumnBindings();
        for (duckdb::idx_t i = 0; i < bindings.size(); i++) {
          if (bindings[i] == rowid_binding) {
            del.children[0] = duckdb::make_uniq<LogicalTee>(
                std::move(del.children[0]), i, std::move(col_pos));
            del.children[0]->ResolveOperatorTypes();
            state.arm_tee(key);
            break;
          }
        }
      }
    }
  }
  for (auto &child : op->children) {
    tee_walk(context, manager, state, child);
  }
}

inline void tee_optimize(duckdb::OptimizerExtensionInput &input,
                         duckdb::unique_ptr<duckdb::LogicalOperator> &plan) {
  if (internal_query_depth > 0) {
    return; // never tee DBSP's own internal queries
  }
  auto state = tee_context_state(input.context);
  if (!state || state->current_stmt_captured()) {
    return; // design 1 already serves this statement
  }
  auto &manager = get_cdc_manager(input.context);
  if (!manager.is_auto_sync_enabled() || !manager.write_capture_enabled()) {
    return;
  }
  try {
    tee_walk(input.context, manager, *state, plan);
  } catch (...) {
    // arming is best-effort; an un-teed statement takes the scan path
  }
}

inline void register_plan_tee(duckdb::DBConfig &config) {
  duckdb::OptimizerExtension ext;
  ext.optimize_function = tee_optimize;
  duckdb::OptimizerExtension::Register(config, std::move(ext));
}

} // namespace dbsp_native

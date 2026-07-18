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
// DELETE tee: full pass-through, emits (old row, -1).
// UPDATE tee: the child projection's LAST column is the rowid BY
// CONVENTION (PhysicalUpdate reads chunk.ColumnCount()-1), so appended
// old-image columns must never reach the update operator — the tee
// projects them back out, emitting (old, -1)/(new, +1) where the new
// image overlays the SET values (already computed in the child
// projection) onto the old image. A repeated rowid (UPDATE ... FROM
// multi-match) is ambiguous and invalidates the tee for the statement.

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
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_update.hpp"

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
              std::vector<duckdb::idx_t> col_pos,
              std::vector<std::pair<duckdb::idx_t, duckdb::idx_t>> set_map,
              duckdb::idx_t n_output)
      : duckdb::PhysicalOperator(physical_plan,
                                 duckdb::PhysicalOperatorType::EXTENSION,
                                 std::move(types), estimated_cardinality),
        rowid_pos(rowid_pos), col_pos(std::move(col_pos)),
        set_map(std::move(set_map)), n_output(n_output) {}
  // set after Make<> for INSERT-mode tees


  duckdb::idx_t rowid_pos;
  std::vector<duckdb::idx_t> col_pos; // row image positions, table order
  // UPDATE only: (physical column index, child position of its SET value)
  std::vector<std::pair<duckdb::idx_t, duckdb::idx_t>> set_map;
  // 0 = full pass-through (DELETE/INSERT); otherwise emit only the first
  // n_output input columns so the update operator's rowid-is-last-column
  // convention survives the widening
  duckdb::idx_t n_output;
  // INSERT mode: no rowid exists yet — col_pos maps the child's columns
  // (already cast to table types by the binder) into table order
  bool is_insert = false;

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
      const bool is_update = !set_map.empty();
      for (duckdb::idx_t i = 0; i < input.size(); i++) {
        DuckDBRow old_row;
        old_row.columns.reserve(col_pos.size());
        for (const auto pos : col_pos) {
          old_row.columns.push_back(input.GetValue(pos, i));
        }
        if (is_insert) {
          // appended row: no rowid yet, duplicates are real inserts
          ctx_state->tee_add_insert(std::move(old_row));
          continue;
        }
        const auto rowid =
            input.GetValue(rowid_pos, i).GetValue<int64_t>();
        if (is_update) {
          DuckDBRow new_row = old_row;
          for (const auto &[col, pos] : set_map) {
            new_row.columns[col] = input.GetValue(pos, i);
          }
          ctx_state->tee_add_update(rowid, std::move(old_row),
                                    std::move(new_row));
        } else {
          ctx_state->tee_add_delete(rowid, std::move(old_row));
        }
      }
    }
    if (n_output == 0) {
      chunk.Reference(input); // DELETE: pass through unchanged
    } else {
      for (duckdb::idx_t c = 0; c < n_output; c++) {
        chunk.data[c].Reference(input.data[c]);
      }
      chunk.SetCardinality(input.size());
    }
    return duckdb::OperatorResultType::NEED_MORE_INPUT;
  }

  bool ParallelOperator() const override { return true; }

  duckdb::string GetName() const override { return "DBSP_TEE"; }
};

class LogicalTee : public duckdb::LogicalExtensionOperator {
public:
  LogicalTee(duckdb::unique_ptr<duckdb::LogicalOperator> child,
             duckdb::idx_t rowid_pos, std::vector<duckdb::idx_t> col_pos,
             std::vector<std::pair<duckdb::idx_t, duckdb::idx_t>> set_map,
             duckdb::idx_t n_output)
      : rowid_pos(rowid_pos), col_pos(std::move(col_pos)),
        set_map(std::move(set_map)), n_output(n_output) {
    children.push_back(std::move(child));
  }

  duckdb::idx_t rowid_pos;
  std::vector<duckdb::idx_t> col_pos;
  std::vector<std::pair<duckdb::idx_t, duckdb::idx_t>> set_map;
  duckdb::idx_t n_output;
  bool is_insert = false;

  duckdb::vector<duckdb::ColumnBinding> GetColumnBindings() override {
    auto bindings = children[0]->GetColumnBindings();
    if (n_output > 0 && bindings.size() > n_output) {
      bindings.resize(n_output); // hide the widened old-image columns
    }
    return bindings;
  }

  duckdb::string GetExtensionName() const override { return "dbsp_tee"; }

  duckdb::PhysicalOperator &
  CreatePlan(duckdb::ClientContext &context,
             duckdb::PhysicalPlanGenerator &planner) override {
    auto &child = planner.CreatePlan(*children[0]);
    auto out_types = child.types;
    if (n_output > 0 && out_types.size() > n_output) {
      out_types.resize(n_output);
    }
    auto &op = planner.Make<PhysicalTee>(std::move(out_types),
                                         child.estimated_cardinality,
                                         rowid_pos, col_pos, set_map, n_output);
    op.Cast<PhysicalTee>().is_insert = is_insert;
    op.children.push_back(child);
    return op;
  }

protected:
  void ResolveTypes() override {
    types = children[0]->types;
    if (n_output > 0 && types.size() > n_output) {
      types.resize(n_output);
    }
  }
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
    // with projection pushdown the GET exposes only projection_ids
    // entries (as (table_index, proj_id) bindings) — the appended
    // columns must join that list to be visible at all
    if (!get.projection_ids.empty()) {
      get.projection_ids.push_back(prev + c);
    }
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
                std::move(del.children[0]), i, std::move(col_pos),
                std::vector<std::pair<duckdb::idx_t, duckdb::idx_t>>(),
                /*n_output=*/0);
            del.children[0]->ResolveOperatorTypes();
            state.arm_tee(key);
            break;
          }
        }
      }
    }
  }
  if (op->type == duckdb::LogicalOperatorType::LOGICAL_UPDATE) {
    auto &upd = op->Cast<duckdb::LogicalUpdate>();
    const std::string key = canonical_table_key(upd.table);
    do {
      if (!manager.is_table_tracked(key) || upd.children.empty() ||
          upd.children[0]->type !=
              duckdb::LogicalOperatorType::LOGICAL_PROJECTION ||
          upd.columns.size() != upd.expressions.size()) {
        break;
      }
      auto &proj = upd.children[0]->Cast<duckdb::LogicalProjection>();
      // PhysicalUpdate takes the rowid from the LAST child column — the
      // tee will project the widened columns back out to preserve that
      const auto n_output = proj.GetColumnBindings().size();
      if (n_output == 0) {
        break;
      }
      const auto rowid_pos = n_output - 1;
      // every SET value must be a plain reference into the child
      // projection (DEFAULTs and friends live elsewhere — decline)
      std::vector<std::pair<duckdb::idx_t, duckdb::idx_t>> set_map;
      bool ok = true;
      for (duckdb::idx_t i = 0; i < upd.expressions.size(); i++) {
        if (upd.expressions[i]->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_COLUMN_REF) {
          ok = false;
          break;
        }
        const auto binding = upd.expressions[i]
                                 ->Cast<duckdb::BoundColumnRefExpression>()
                                 .binding;
        if (binding.table_index != proj.table_index ||
            binding.column_index >= n_output) {
          ok = false;
          break;
        }
        set_map.emplace_back(upd.columns[i].index, binding.column_index);
      }
      if (!ok) {
        break;
      }
      std::vector<duckdb::idx_t> col_pos;
      if (!widen_delete_child(proj, upd.table, col_pos)) {
        break;
      }
      upd.children[0] = duckdb::make_uniq<LogicalTee>(
          std::move(upd.children[0]), rowid_pos, std::move(col_pos),
          std::move(set_map), n_output);
      upd.children[0]->ResolveOperatorTypes();
      state.arm_tee(key);
    } while (false);
  }
  if (op->type == duckdb::LogicalOperatorType::LOGICAL_INSERT) {
    auto &ins = op->Cast<duckdb::LogicalInsert>();
    const std::string key = canonical_table_key(ins.table);
    do {
      // Explicit-transaction INSERTs are G2's job (LocalStorage scan is
      // exact there; teeing both would double-count). ON CONFLICT rows
      // are resolved inside the insert operator — child rows are not the
      // final effect. Defaults and partial column lists resolve in a
      // PHYSICAL projection injected ABOVE this tee at plan time, so
      // only identity or pure-permutation column maps are teeable
      // (replicating default evaluation would run sequences twice).
      if (!manager.is_table_tracked(key) ||
          !context.transaction.IsAutoCommit() || ins.children.empty() ||
          ins.on_conflict_info.action_type !=
              duckdb::OnConflictAction::THROW) {
        break;
      }
      auto &table_cols = ins.table.GetColumns();
      const auto n_cols = table_cols.PhysicalColumnCount();
      if (n_cols != table_cols.LogicalColumnCount()) {
        break; // generated columns: child order diverges from row layout
      }
      std::vector<duckdb::idx_t> col_pos;
      if (ins.column_index_map.empty()) {
        // no column list: the binder cast the source to table order
        if (ins.children[0]->GetColumnBindings().size() != n_cols) {
          break;
        }
        for (duckdb::idx_t c = 0; c < n_cols; c++) {
          col_pos.push_back(c);
        }
      } else {
        // full-cover (possibly permuted) column list: map back to table
        // order; any INVALID entry means a DEFAULT would fill it — the
        // physical defaults projection sits above us, decline
        bool ok = true;
        for (auto &col : table_cols.Physical()) {
          const auto mapped = ins.column_index_map[col.Physical()];
          if (mapped == duckdb::DConstants::INVALID_INDEX) {
            ok = false;
            break;
          }
          col_pos.push_back(mapped);
        }
        if (!ok || col_pos.size() != n_cols) {
          break;
        }
      }
      auto tee = duckdb::make_uniq<LogicalTee>(
          std::move(ins.children[0]), /*rowid_pos=*/0, std::move(col_pos),
          std::vector<std::pair<duckdb::idx_t, duckdb::idx_t>>(),
          /*n_output=*/0);
      tee->is_insert = true;
      ins.children[0] = std::move(tee);
      ins.children[0]->ResolveOperatorTypes();
      state.arm_tee(key);
    } while (false);
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

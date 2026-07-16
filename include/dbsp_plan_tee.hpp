// D2 spike (docs/DESIGN_WRITE_CAPTURE.md, "design 2"): OptimizerExtension
// tee on DML plans. Verifies on pinned DuckDB v1.5.4 that an extension
// can (a) rewrite a bound LogicalDelete's child, (b) inject a
// LogicalExtensionOperator whose physical operator streams the child's
// rows through unchanged while observing them at execution time.
//
// SPIKE SCOPE ONLY: gated behind the DBSP_TEE_SPIKE env var, observes
// DELETE child rowids into a process-global buffer, and is wired to no
// sync machinery. The real design would widen child projections to old
// row images and feed the per-transaction capture buffer.

#pragma once

#include "duckdb.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

#include <cstdlib>
#include <mutex>
#include <vector>

namespace dbsp_native {

// Process-global spike observation buffer (the real design would key a
// per-connection capture buffer through ClientContext)
struct TeeSpikeBuffer {
  std::mutex mutex;
  std::vector<int64_t> rowids;

  void clear() {
    std::lock_guard<std::mutex> guard(mutex);
    rowids.clear();
  }
  size_t count() {
    std::lock_guard<std::mutex> guard(mutex);
    return rowids.size();
  }
};

inline TeeSpikeBuffer &tee_spike_buffer() {
  static TeeSpikeBuffer buffer;
  return buffer;
}

class PhysicalTee : public duckdb::PhysicalOperator {
public:
  PhysicalTee(duckdb::PhysicalPlan &physical_plan,
              duckdb::vector<duckdb::LogicalType> types,
              duckdb::idx_t estimated_cardinality, duckdb::idx_t rowid_index)
      : duckdb::PhysicalOperator(physical_plan,
                                 duckdb::PhysicalOperatorType::EXTENSION,
                                 std::move(types), estimated_cardinality),
        rowid_index(rowid_index) {}

  duckdb::idx_t rowid_index;

  duckdb::unique_ptr<duckdb::OperatorState>
  GetOperatorState(duckdb::ExecutionContext &context) const override {
    return duckdb::make_uniq<duckdb::OperatorState>();
  }

  duckdb::OperatorResultType
  Execute(duckdb::ExecutionContext &context, duckdb::DataChunk &input,
          duckdb::DataChunk &chunk, duckdb::GlobalOperatorState &gstate,
          duckdb::OperatorState &state) const override {
    auto &buffer = tee_spike_buffer();
    {
      std::lock_guard<std::mutex> guard(buffer.mutex);
      for (duckdb::idx_t i = 0; i < input.size(); i++) {
        buffer.rowids.push_back(
            input.GetValue(rowid_index, i).GetValue<int64_t>());
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
             duckdb::idx_t rowid_index)
      : rowid_index(rowid_index) {
    children.push_back(std::move(child));
  }

  duckdb::idx_t rowid_index;

  duckdb::vector<duckdb::ColumnBinding> GetColumnBindings() override {
    return children[0]->GetColumnBindings();
  }

  duckdb::string GetExtensionName() const override { return "dbsp_tee"; }

  duckdb::PhysicalOperator &
  CreatePlan(duckdb::ClientContext &context,
             duckdb::PhysicalPlanGenerator &planner) override {
    auto &child = planner.CreatePlan(*children[0]);
    auto &op = planner.Make<PhysicalTee>(child.types, child.estimated_cardinality,
                                         rowid_index);
    op.children.push_back(child);
    return op;
  }

protected:
  void ResolveTypes() override { types = children[0]->types; }
};

inline void tee_walk(duckdb::unique_ptr<duckdb::LogicalOperator> &op) {
  if (op->type == duckdb::LogicalOperatorType::LOGICAL_DELETE) {
    auto &del = op->Cast<duckdb::LogicalDelete>();
    // At optimize time the rowid is a BOUND_COLUMN_REF (binding-based);
    // BOUND_REF indexes only exist after the ColumnBindingResolver runs.
    if (!del.children.empty() && !del.expressions.empty() &&
        del.expressions[0]->GetExpressionClass() ==
            duckdb::ExpressionClass::BOUND_COLUMN_REF) {
      const auto binding =
          del.expressions[0]->Cast<duckdb::BoundColumnRefExpression>().binding;
      const auto child_bindings = del.children[0]->GetColumnBindings();
      for (duckdb::idx_t i = 0; i < child_bindings.size(); i++) {
        if (child_bindings[i] == binding) {
          del.children[0] = duckdb::make_uniq<LogicalTee>(
              std::move(del.children[0]), i);
          break;
        }
      }
    }
  }
  for (auto &child : op->children) {
    tee_walk(child);
  }
}

inline void tee_optimize(duckdb::OptimizerExtensionInput &input,
                         duckdb::unique_ptr<duckdb::LogicalOperator> &plan) {
  if (!std::getenv("DBSP_TEE_SPIKE")) {
    return;
  }
  tee_walk(plan);
}

inline void register_tee_spike(duckdb::DBConfig &config) {
  duckdb::OptimizerExtension ext;
  ext.optimize_function = tee_optimize;
  duckdb::OptimizerExtension::Register(config, std::move(ext));
}

} // namespace dbsp_native

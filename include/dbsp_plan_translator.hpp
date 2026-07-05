// Planner frontend (Phase B): translate DuckDB logical plans into circuit
// views.
//
// Instead of the bespoke SQL parser, view SQL is parsed/bound/planned by
// DuckDB itself (Connection::ExtractPlan on an internal connection with the
// optimizer disabled, so plan shapes stay canonical: PROJECTION / FILTER /
// GET chains without filter pushdown or projection collapse). The bound
// LogicalOperator tree is walked and mapped onto circuit nodes; bound
// expressions are evaluated row-at-a-time through ExpressionExecutor.
//
// B1 scope: single-table LOGICAL_GET -> LOGICAL_FILTER -> LOGICAL_PROJECTION
// chains. Any other operator yields a DBSP-E110 error naming the operator;
// CDCManager::create_view falls back to the bespoke parser transparently.
//
// Caller contract: PlanTranslator::translate issues queries on an internal
// connection, so the caller must hold an InternalQueryGuard (dbsp_cdc.hpp)
// to keep transaction-commit hooks from recursing into CDCManager.

#pragma once

#include "dbsp_circuit_views.hpp"
#include "dbsp_errors.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <memory>
#include <string>
#include <vector>

namespace dbsp_native {

// Keeps the internal connection and the extracted plan alive for as long as
// any node lambda references bound expressions or the client context.
struct PlanKeepAlive {
  std::unique_ptr<duckdb::Connection> connection;
  std::unique_ptr<duckdb::LogicalOperator> plan;
};

// Row-at-a-time adapter over ExpressionExecutor: builds a 1-row DataChunk
// from a DuckDBRow, evaluates one bound expression, returns the result Value.
// Correct but slow; vectorized evaluation is a later milestone (B6).
class RowExprEval {
public:
  RowExprEval(std::shared_ptr<PlanKeepAlive> keep_alive,
              const duckdb::Expression &expr,
              duckdb::vector<duckdb::LogicalType> input_types)
      : keep_alive_(std::move(keep_alive)),
        context_(*keep_alive_->connection->context), expr_(expr),
        input_types_(std::move(input_types)), executor_(context_, expr) {
    if (!input_types_.empty()) {
      chunk_.Initialize(duckdb::Allocator::Get(context_), input_types_);
    }
  }

  duckdb::Value eval(const DuckDBRow &row) {
    chunk_.Reset();
    for (duckdb::idx_t i = 0; i < input_types_.size(); i++) {
      duckdb::Value v = i < row.columns.size()
                            ? row.columns[i]
                            : duckdb::Value(input_types_[i]);
      if (v.type() != input_types_[i]) {
        v = v.DefaultCastAs(input_types_[i]);
      }
      chunk_.SetValue(i, 0, v);
    }
    chunk_.SetCardinality(1);
    duckdb::Vector result(expr_.return_type);
    executor_.ExecuteExpression(chunk_, result);
    return result.GetValue(0);
  }

private:
  // Declared first so it outlives the executor during destruction
  std::shared_ptr<PlanKeepAlive> keep_alive_;
  duckdb::ClientContext &context_;
  const duckdb::Expression &expr_;
  duckdb::vector<duckdb::LogicalType> input_types_;
  duckdb::ExpressionExecutor executor_;
  duckdb::DataChunk chunk_;
};

// One translated plan operator, in source-to-root order.
struct PlanStep {
  enum class Kind {
    FILTER_EXPR, // FilterNode over one bound predicate expression
    MAP_EXPR,    // MapNode evaluating projection expressions
    MAP_COLS     // MapNode selecting columns by index (GET column_ids)
  };

  Kind kind;
  std::vector<const duckdb::Expression *> exprs;    // FILTER_EXPR / MAP_EXPR
  duckdb::vector<duckdb::LogicalType> input_types; // child operator's types
  std::vector<duckdb::idx_t> column_idxs;        // MAP_COLS
};

// Circuit view built from a translated plan: Source -> steps -> Sink.
class PlannedCircuitView : public SingleSourceCircuitView {
public:
  using RowFilter = dbsp::FilterNode<DuckDBRow, DuckDBRowHash>;
  using RowMap =
      dbsp::MapNode<DuckDBRow, DuckDBRow, DuckDBRowHash, DuckDBRowHash>;

  PlannedCircuitView(const std::string &name, const std::string &sql,
                     const std::string &source_table,
                     const TableSchema &result_schema,
                     std::shared_ptr<PlanKeepAlive> keep_alive,
                     const std::vector<PlanStep> &steps)
      : SingleSourceCircuitView(name, sql, source_table, result_schema) {
    OutputFn prev = [this]() -> const DuckDBZSet & { return source_output(); };
    for (const auto &step : steps) {
      switch (step.kind) {
      case PlanStep::Kind::FILTER_EXPR: {
        auto eval = std::make_shared<RowExprEval>(keep_alive, *step.exprs[0],
                                                  step.input_types);
        auto *node = circuit_.add_node(std::make_unique<RowFilter>(
            circuit_.next_node_id(), prev,
            [eval](const DuckDBRow &row) {
              duckdb::Value v = eval->eval(row);
              return !v.IsNull() && v.GetValue<bool>();
            },
            "plan_filter"));
        prev = [node]() -> const DuckDBZSet & { return node->output(); };
        break;
      }
      case PlanStep::Kind::MAP_EXPR: {
        auto evals =
            std::make_shared<std::vector<std::unique_ptr<RowExprEval>>>();
        for (const auto *expr : step.exprs) {
          evals->push_back(std::make_unique<RowExprEval>(keep_alive, *expr,
                                                         step.input_types));
        }
        auto *node = circuit_.add_node(std::make_unique<RowMap>(
            circuit_.next_node_id(), prev,
            [evals](const DuckDBRow &row) {
              DuckDBRow out;
              out.columns.reserve(evals->size());
              for (auto &eval : *evals) {
                out.columns.push_back(eval->eval(row));
              }
              return out;
            },
            "plan_project"));
        prev = [node]() -> const DuckDBZSet & { return node->output(); };
        break;
      }
      case PlanStep::Kind::MAP_COLS: {
        auto idxs = step.column_idxs;
        auto *node = circuit_.add_node(std::make_unique<RowMap>(
            circuit_.next_node_id(), prev,
            [idxs](const DuckDBRow &row) {
              DuckDBRow out;
              out.columns.reserve(idxs.size());
              for (auto idx : idxs) {
                out.columns.push_back(idx < row.columns.size()
                                          ? row.columns[idx]
                                          : duckdb::Value());
              }
              return out;
            },
            "plan_scan_cols"));
        prev = [node]() -> const DuckDBZSet & { return node->output(); };
        break;
      }
      }
    }
    finish(std::move(prev));
  }
};

class PlanTranslator {
public:
  struct Result {
    std::unique_ptr<NativeMaterializedView> view;
    std::string error; // set when view is null
  };

  // Translate view SQL into a circuit view via DuckDB's planner.
  // Caller must hold an InternalQueryGuard.
  static Result translate(duckdb::ClientContext &context,
                          const std::string &view_name,
                          const std::string &sql) {
    auto keep_alive = std::make_shared<PlanKeepAlive>();
    try {
      keep_alive->connection = std::make_unique<duckdb::Connection>(
          duckdb::DatabaseInstance::GetDatabase(context));
      // Canonical plan shapes: no filter pushdown into GET, no projection
      // collapse. ExtractPlan still runs ColumnBindingResolver and
      // ResolveOperatorTypes.
      keep_alive->connection->context->config.enable_optimizer = false;
      keep_alive->plan = keep_alive->connection->ExtractPlan(sql);
    } catch (const std::exception &e) {
      return {nullptr, std::string("planner frontend: ") + e.what()};
    }

    Walker walker;
    if (!walker.visit(*keep_alive->plan)) {
      return {nullptr, walker.error};
    }

    TableSchema schema;
    schema.table_name = view_name;
    schema.columns = walker.columns;

    auto view = std::make_unique<PlannedCircuitView>(
        view_name, sql, walker.source_table, schema, std::move(keep_alive),
        walker.steps);
    return {std::move(view), ""};
  }

private:
  struct Walker {
    std::vector<PlanStep> steps;
    std::string source_table;
    std::vector<ColumnInfo> columns; // schema of current operator's output
    std::string error;

    bool unsupported(const std::string &what) {
      error = format_error_code(ErrorCode::PLAN_OPERATOR_NOT_SUPPORTED) +
              ": unsupported in planner frontend: " + what;
      return false;
    }

    bool visit(duckdb::LogicalOperator &op) {
      switch (op.type) {
      case duckdb::LogicalOperatorType::LOGICAL_PROJECTION:
        return visit_projection(op.Cast<duckdb::LogicalProjection>());
      case duckdb::LogicalOperatorType::LOGICAL_FILTER:
        return visit_filter(op.Cast<duckdb::LogicalFilter>());
      case duckdb::LogicalOperatorType::LOGICAL_GET:
        return visit_get(op.Cast<duckdb::LogicalGet>());
      default:
        return unsupported("logical operator " + op.GetName());
      }
    }

    bool visit_projection(duckdb::LogicalProjection &op) {
      if (!visit(*op.children[0])) {
        return false;
      }
      PlanStep step;
      step.kind = PlanStep::Kind::MAP_EXPR;
      step.input_types = op.children[0]->types;
      for (const auto &expr : op.expressions) {
        step.exprs.push_back(expr.get());
      }
      steps.push_back(std::move(step));

      columns.clear();
      for (duckdb::idx_t i = 0; i < op.expressions.size(); i++) {
        columns.push_back({op.expressions[i]->GetName(), op.types[i]});
      }
      return true;
    }

    bool visit_filter(duckdb::LogicalFilter &op) {
      if (!op.projection_map.empty()) {
        return unsupported("FILTER with projection map");
      }
      if (!visit(*op.children[0])) {
        return false;
      }
      for (const auto &expr : op.expressions) {
        PlanStep step;
        step.kind = PlanStep::Kind::FILTER_EXPR;
        step.input_types = op.children[0]->types;
        step.exprs.push_back(expr.get());
        steps.push_back(std::move(step));
      }
      // Filter passes rows through unchanged; columns stay as-is
      return true;
    }

    bool visit_get(duckdb::LogicalGet &op) {
      auto table_entry = op.GetTable();
      if (!table_entry) {
        return unsupported("non-table scan (" + op.function.name + ")");
      }
      if (!op.table_filters.filters.empty()) {
        return unsupported("GET with pushed-down table filters");
      }
      if (!op.projection_ids.empty()) {
        return unsupported("GET with projection ids");
      }
      source_table = table_entry->name;

      // CDC rows carry ALL table columns in declared order; GET's output is
      // column_ids order. Emit an index-selection map unless it's identity.
      const auto &column_ids = op.GetColumnIds();
      std::vector<duckdb::idx_t> idxs;
      bool identity = column_ids.size() == op.returned_types.size();
      for (duckdb::idx_t i = 0; i < column_ids.size(); i++) {
        duckdb::idx_t col = column_ids[i].GetPrimaryIndex();
        if (col >= op.returned_types.size()) {
          return unsupported("virtual column scan (e.g. rowid)");
        }
        if (col != i) {
          identity = false;
        }
        idxs.push_back(col);
      }

      columns.clear();
      for (auto idx : idxs) {
        columns.push_back({op.names[idx], op.returned_types[idx]});
      }
      if (!identity) {
        PlanStep step;
        step.kind = PlanStep::Kind::MAP_COLS;
        step.column_idxs = std::move(idxs);
        steps.push_back(std::move(step));
      }
      return true;
    }
  };
};

} // namespace dbsp_native

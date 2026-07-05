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
// Current scope (B1+B2): single-table LOGICAL_GET -> LOGICAL_FILTER ->
// LOGICAL_PROJECTION chains plus LOGICAL_AGGREGATE_AND_GROUP_BY (multiple
// aggregates per GROUP BY, expression keys; HAVING arrives as a FILTER above
// the aggregate and needs no special handling). Any other operator yields a
// DBSP-E110 error naming the operator; CDCManager::create_view falls back to
// the bespoke parser transparently.
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
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <set>

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

// One aggregate within an AGGREGATE step (B2)
struct PlanAggSpec {
  enum class Fn { COUNT_STAR, COUNT, SUM, AVG, MIN, MAX };

  Fn fn;
  const duckdb::Expression *arg = nullptr; // null for COUNT_STAR
  bool integer_arg = false;                // SUM/AVG: int64 vs double sum
  duckdb::LogicalType return_type;
};

// One translated plan operator, in source-to-root order.
struct PlanStep {
  enum class Kind {
    FILTER_EXPR, // FilterNode over one bound predicate expression
    MAP_EXPR,    // MapNode evaluating projection expressions
    MAP_COLS,    // MapNode selecting columns by index (GET column_ids)
    AGGREGATE    // PlanAggregateNode (exprs = group keys)
  };

  Kind kind;
  std::vector<const duckdb::Expression *> exprs;    // FILTER_EXPR / MAP_EXPR
  duckdb::vector<duckdb::LogicalType> input_types; // child operator's types
  std::vector<duckdb::idx_t> column_idxs;        // MAP_COLS
  std::vector<PlanAggSpec> agg_specs;            // AGGREGATE
};

// Incremental GROUP BY aggregation over bound expressions: retracts the old
// group row, applies weighted deltas to per-group accumulators, emits the new
// group row; the downstream sink integrates. Output row layout matches
// LogicalAggregate: group values first, then aggregate values.
//
// Global aggregates (no GROUP BY) always have exactly one output row, even
// for an empty input (COUNT=0, SUM/AVG/MIN/MAX=NULL) — the row is emitted on
// the first circuit step and retracted/re-emitted on every change.
class PlanAggregateNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  struct AggInstance {
    PlanAggSpec::Fn fn;
    std::unique_ptr<RowExprEval> arg; // null for COUNT_STAR
    bool integer_arg;
    duckdb::LogicalType return_type;
  };

  PlanAggregateNode(dbsp::NodeId id, InputFn input_fn,
                    std::vector<std::unique_ptr<RowExprEval>> group_evals,
                    std::vector<AggInstance> aggs,
                    std::string name = "plan_aggregate")
      : dbsp::Node(id, std::move(name)), input_fn_(std::move(input_fn)),
        group_evals_(std::move(group_evals)), aggs_(std::move(aggs)) {}

  void step() override {
    output_.clear();
    has_output_ = false;
    const DuckDBZSet &changes = input_fn_();
    bool global = group_evals_.empty();

    if (changes.empty()) {
      if (global && !global_emitted_) {
        output_.insert(result_row(DuckDBRow{}, states_[DuckDBRow{}]), 1);
        global_emitted_ = true;
        has_output_ = true;
      }
      return;
    }

    // Bucket incoming deltas by group key
    std::unordered_map<DuckDBRow, std::vector<std::pair<DuckDBRow, int64_t>>,
                       DuckDBRowHash>
        buckets;
    for (const auto &[row, weight] : changes) {
      DuckDBRow key;
      key.columns.reserve(group_evals_.size());
      for (auto &g : group_evals_) {
        key.columns.push_back(g->eval(row));
      }
      buckets[key].emplace_back(row, weight);
    }

    for (auto &[key, rows] : buckets) {
      auto &state = states_[key];
      bool had_row = global ? global_emitted_ : state.row_weight > 0;
      if (had_row) {
        output_.insert(result_row(key, state), -1);
      }
      for (const auto &[row, weight] : rows) {
        apply(state, row, weight);
      }
      if (global || state.row_weight > 0) {
        output_.insert(result_row(key, state), 1);
      }
      if (!global && state.row_weight <= 0) {
        states_.erase(key);
      }
      if (global) {
        global_emitted_ = true;
      }
    }
    has_output_ = !output_.empty();
  }

  void reset() override {
    states_.clear();
    output_.clear();
    has_output_ = false;
    global_emitted_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

private:
  struct AggState {
    int64_t count = 0; // non-NULL argument count (rows for COUNT(*))
    int64_t isum = 0;
    double dsum = 0;
    std::multiset<duckdb::Value> values; // MIN/MAX
  };

  struct GroupState {
    int64_t row_weight = 0; // total weight of rows in the group
    std::vector<AggState> aggs;
  };

  void apply(GroupState &state, const DuckDBRow &row, int64_t weight) {
    state.row_weight += weight;
    state.aggs.resize(aggs_.size());
    for (size_t i = 0; i < aggs_.size(); i++) {
      auto &spec = aggs_[i];
      auto &s = state.aggs[i];
      if (spec.fn == PlanAggSpec::Fn::COUNT_STAR) {
        s.count += weight;
        continue;
      }
      duckdb::Value v = spec.arg->eval(row);
      if (v.IsNull()) {
        continue; // SQL: NULL arguments are ignored
      }
      s.count += weight;
      switch (spec.fn) {
      case PlanAggSpec::Fn::COUNT:
        break;
      case PlanAggSpec::Fn::SUM:
      case PlanAggSpec::Fn::AVG:
        if (spec.integer_arg) {
          s.isum += v.GetValue<int64_t>() * weight;
        } else {
          s.dsum += v.GetValue<double>() * weight;
        }
        break;
      case PlanAggSpec::Fn::MIN:
      case PlanAggSpec::Fn::MAX:
        if (weight > 0) {
          for (int64_t w = 0; w < weight; w++) {
            s.values.insert(v);
          }
        } else {
          for (int64_t w = 0; w < -weight; w++) {
            auto it = s.values.find(v);
            if (it != s.values.end()) {
              s.values.erase(it);
            }
          }
        }
        break;
      default:
        break;
      }
    }
  }

  duckdb::Value agg_value(const AggInstance &spec, const AggState &s) const {
    switch (spec.fn) {
    case PlanAggSpec::Fn::COUNT_STAR:
    case PlanAggSpec::Fn::COUNT:
      return duckdb::Value::BIGINT(s.count);
    case PlanAggSpec::Fn::SUM:
      if (s.count == 0) {
        return duckdb::Value(spec.return_type);
      }
      if (spec.integer_arg) {
        return duckdb::Value::Numeric(spec.return_type, s.isum);
      }
      return duckdb::Value(s.dsum).DefaultCastAs(spec.return_type);
    case PlanAggSpec::Fn::AVG: {
      if (s.count == 0) {
        return duckdb::Value(spec.return_type);
      }
      double sum = spec.integer_arg ? static_cast<double>(s.isum) : s.dsum;
      return duckdb::Value(sum / static_cast<double>(s.count))
          .DefaultCastAs(spec.return_type);
    }
    case PlanAggSpec::Fn::MIN:
      return s.values.empty() ? duckdb::Value(spec.return_type)
                              : *s.values.begin();
    case PlanAggSpec::Fn::MAX:
      return s.values.empty() ? duckdb::Value(spec.return_type)
                              : *s.values.rbegin();
    }
    return duckdb::Value(spec.return_type);
  }

  DuckDBRow result_row(const DuckDBRow &key, const GroupState &state) const {
    DuckDBRow result;
    result.columns = key.columns;
    result.columns.reserve(key.columns.size() + aggs_.size());
    for (size_t i = 0; i < aggs_.size(); i++) {
      static const AggState kEmpty;
      const AggState &s = i < state.aggs.size() ? state.aggs[i] : kEmpty;
      result.columns.push_back(agg_value(aggs_[i], s));
    }
    return result;
  }

  InputFn input_fn_;
  std::vector<std::unique_ptr<RowExprEval>> group_evals_;
  std::vector<AggInstance> aggs_;
  std::unordered_map<DuckDBRow, GroupState, DuckDBRowHash> states_;
  DuckDBZSet output_;
  bool has_output_ = false;
  bool global_emitted_ = false;
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
      case PlanStep::Kind::AGGREGATE: {
        std::vector<std::unique_ptr<RowExprEval>> group_evals;
        for (const auto *expr : step.exprs) {
          group_evals.push_back(std::make_unique<RowExprEval>(
              keep_alive, *expr, step.input_types));
        }
        std::vector<PlanAggregateNode::AggInstance> aggs;
        for (const auto &spec : step.agg_specs) {
          PlanAggregateNode::AggInstance inst;
          inst.fn = spec.fn;
          inst.integer_arg = spec.integer_arg;
          inst.return_type = spec.return_type;
          if (spec.arg) {
            inst.arg = std::make_unique<RowExprEval>(keep_alive, *spec.arg,
                                                     step.input_types);
          }
          aggs.push_back(std::move(inst));
        }
        auto *node = circuit_.add_node(std::make_unique<PlanAggregateNode>(
            circuit_.next_node_id(), prev, std::move(group_evals),
            std::move(aggs)));
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
      case duckdb::LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
        return visit_aggregate(op.Cast<duckdb::LogicalAggregate>());
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

    bool visit_aggregate(duckdb::LogicalAggregate &op) {
      if (!op.grouping_functions.empty()) {
        return unsupported("GROUPING function");
      }
      if (op.grouping_sets.size() > 1) {
        return unsupported("ROLLUP/CUBE/GROUPING SETS");
      }
      if (!visit(*op.children[0])) {
        return false;
      }

      PlanStep step;
      step.kind = PlanStep::Kind::AGGREGATE;
      step.input_types = op.children[0]->types;
      for (const auto &group : op.groups) {
        step.exprs.push_back(group.get());
      }
      for (const auto &expr : op.expressions) {
        if (expr->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_AGGREGATE) {
          return unsupported("non-aggregate expression in AGGREGATE");
        }
        auto &agg = expr->Cast<duckdb::BoundAggregateExpression>();
        if (agg.IsDistinct()) {
          return unsupported("DISTINCT aggregate");
        }
        if (agg.filter) {
          return unsupported("FILTER clause on aggregate");
        }
        if (agg.order_bys) {
          return unsupported("ORDER BY in aggregate");
        }

        PlanAggSpec spec;
        spec.return_type = agg.return_type;
        const std::string &fn = agg.function.name;
        if (fn == "count_star") {
          spec.fn = PlanAggSpec::Fn::COUNT_STAR;
        } else if (fn == "count") {
          spec.fn = PlanAggSpec::Fn::COUNT;
        } else if (fn == "sum" || fn == "sum_no_overflow") {
          spec.fn = PlanAggSpec::Fn::SUM;
        } else if (fn == "avg") {
          spec.fn = PlanAggSpec::Fn::AVG;
        } else if (fn == "min") {
          spec.fn = PlanAggSpec::Fn::MIN;
        } else if (fn == "max") {
          spec.fn = PlanAggSpec::Fn::MAX;
        } else {
          return unsupported("aggregate function " + fn);
        }

        if (spec.fn != PlanAggSpec::Fn::COUNT_STAR) {
          if (agg.children.size() != 1) {
            return unsupported("aggregate with " +
                               std::to_string(agg.children.size()) +
                               " arguments (" + fn + ")");
          }
          spec.arg = agg.children[0].get();
          if (spec.fn == PlanAggSpec::Fn::SUM ||
              spec.fn == PlanAggSpec::Fn::AVG) {
            switch (spec.arg->return_type.id()) {
            case duckdb::LogicalTypeId::TINYINT:
            case duckdb::LogicalTypeId::SMALLINT:
            case duckdb::LogicalTypeId::INTEGER:
            case duckdb::LogicalTypeId::BIGINT:
            case duckdb::LogicalTypeId::UTINYINT:
            case duckdb::LogicalTypeId::USMALLINT:
            case duckdb::LogicalTypeId::UINTEGER:
              spec.integer_arg = true;
              break;
            case duckdb::LogicalTypeId::FLOAT:
            case duckdb::LogicalTypeId::DOUBLE:
              spec.integer_arg = false;
              break;
            default:
              return unsupported(fn + " over " +
                                 spec.arg->return_type.ToString());
            }
          }
        }
        step.agg_specs.push_back(std::move(spec));
      }
      steps.push_back(std::move(step));

      // Output layout: group values first, then aggregate values
      columns.clear();
      for (duckdb::idx_t i = 0; i < op.groups.size(); i++) {
        columns.push_back({op.groups[i]->GetName(), op.types[i]});
      }
      for (duckdb::idx_t i = 0; i < op.expressions.size(); i++) {
        columns.push_back(
            {op.expressions[i]->GetName(), op.types[op.groups.size() + i]});
      }
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

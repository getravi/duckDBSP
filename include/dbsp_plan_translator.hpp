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
// Current scope (B1-B4):
//   B1: LOGICAL_GET -> LOGICAL_FILTER -> LOGICAL_PROJECTION chains
//   B2: LOGICAL_AGGREGATE_AND_GROUP_BY (multi-aggregate, expression keys;
//       HAVING arrives as a FILTER above the aggregate — no special code)
//   B3: LOGICAL_COMPARISON_JOIN (inner equi + residual comparisons),
//       LOGICAL_CROSS_PRODUCT, LOGICAL_DISTINCT, LOGICAL_UNION /
//       LOGICAL_INTERSECT / LOGICAL_EXCEPT (ALL and DISTINCT)
//   B4: LOGICAL_WINDOW (column-ref partitions/orders/args, mapped onto the
//       proven NativeWindowView via EmbeddedViewNode), LOGICAL_MATERIALIZED_CTE
//       + LOGICAL_CTE_REF (definition subtree built once, shared by refs).
//       Correlated subqueries (DELIM_JOIN) and recursive CTEs are rejected
//       with explicit messages.
//   C1: LOGICAL_ORDER_BY / LOGICAL_LIMIT (constant limit/offset only) fold —
//       together with a trailing pure-column-ref projection — into one
//       NativeSortView/NativeLimitView behind an EmbeddedViewNode. When the
//       sort/limit is the plan root, PlannedCircuitView::scan delegates to it
//       so dbsp_query returns rows in ORDER BY order.
// Any other operator yields a DBSP-E110 error naming the operator;
// CDCManager::create_view falls back to the bespoke parser transparently.
//
// Caller contract: PlanTranslator::translate issues queries on an internal
// connection, so the caller must hold an InternalQueryGuard (dbsp_cdc.hpp)
// to keep transaction-commit hooks from recursing into CDCManager.

#pragma once

#include "dbsp_circuit_views.hpp"
#include "dbsp_errors.hpp"
#include "dbsp_window_view.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

#include <memory>
#include <set>
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

// One aggregate within an AGGREGATE spec (B2)
struct PlanAggSpec {
  enum class Fn { COUNT_STAR, COUNT, SUM, AVG, MIN, MAX };

  Fn fn;
  const duckdb::Expression *arg = nullptr; // null for COUNT_STAR
  bool integer_arg = false;                // SUM/AVG: int64 vs double sum
  duckdb::LogicalType return_type;
};

// One translated plan operator. Forms a tree mirroring the logical plan;
// unary operators have one child, JOIN has two, SET_OP has two or more.
struct PlanOpSpec {
  enum class Kind {
    SOURCE,      // base table scan feeding a SourceNode
    MAP_COLS,    // MapNode selecting columns by index (GET column_ids)
    FILTER_EXPR, // FilterNode over bound predicate expressions (AND)
    MAP_EXPR,    // MapNode evaluating projection expressions
    AGGREGATE,   // PlanAggregateNode (exprs = group keys)
    JOIN,        // PlanJoinNode (inner equi + residual comparisons)
    DISTINCT,    // PlanDistinctNode
    SET_OP,      // PlanSetOpNode
    WINDOW,      // NativeWindowView wrapped in an EmbeddedViewNode
    CTE,         // materialized CTE: children = {definition, main query}
    CTE_REF,     // reads the shared output of a CTE definition
    SORT_LIMIT   // NativeSortView/NativeLimitView in an EmbeddedViewNode
  };

  // Comparison between a left-side and a right-side bound expression
  struct JoinCond {
    const duckdb::Expression *left;
    const duckdb::Expression *right;
    duckdb::ExpressionType cmp;
  };

  enum class SetOp { UNION_ALL, UNION, INTERSECT, INTERSECT_ALL, EXCEPT,
                     EXCEPT_ALL };

  Kind kind;
  std::vector<std::unique_ptr<PlanOpSpec>> children;

  std::string table;                             // SOURCE
  std::vector<duckdb::idx_t> column_idxs;        // MAP_COLS
  std::vector<const duckdb::Expression *> exprs; // FILTER/MAP/group keys
  duckdb::vector<duckdb::LogicalType> input_types; // unary: child's types
  std::vector<PlanAggSpec> agg_specs;            // AGGREGATE
  std::vector<JoinCond> equi_conds;              // JOIN: EQUAL conditions
  std::vector<JoinCond> residual_conds;          // JOIN: other comparisons
  duckdb::vector<duckdb::LogicalType> left_types, right_types; // JOIN
  SetOp set_op = SetOp::UNION_ALL;               // SET_OP
  std::vector<NativeWindowView::WindowDef> window_defs;   // WINDOW
  std::vector<ColumnInfo> window_source_cols;             // WINDOW
  std::vector<ColumnInfo> window_result_cols;             // WINDOW
  duckdb::idx_t cte_index = 0;                   // CTE / CTE_REF

  // SORT_LIMIT: ORDER BY / LIMIT / OFFSET folded into one embedded view.
  // project_idxs is a trailing pure-column-ref projection folded in so sort
  // keys dropped from the output still order it. presentation_root marks the
  // plan root: only then does the view's ordered scan drive dbsp_query.
  std::vector<NativeSortView::SortColumn> sort_columns;
  int64_t limit = -1; // -1 = no limit
  int64_t offset = 0;
  std::vector<duckdb::idx_t> project_idxs; // empty = identity
  bool presentation_root = false;
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

// Incremental inner join (B3). Bilinear delta rule per step:
//   Δout = Δl ⋈ R_old + L_old ⋈ Δr + Δl ⋈ Δr
// Each side is indexed by its equi-key values; rows with a NULL key are
// never indexed (SQL: NULL never matches). Residual (non-equality)
// conditions are checked per candidate pair via Value comparison. With no
// conditions at all this degenerates to a cross product (single bucket).
// Output row = left columns followed by right columns (LogicalJoin layout).
class PlanJoinNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  struct KeyPair {
    std::unique_ptr<RowExprEval> left, right;
  };
  struct Residual {
    std::unique_ptr<RowExprEval> left, right;
    duckdb::ExpressionType cmp;
  };

  PlanJoinNode(dbsp::NodeId id, InputFn left_fn, InputFn right_fn,
               std::vector<KeyPair> keys, std::vector<Residual> residuals,
               std::string name = "plan_join")
      : dbsp::Node(id, std::move(name)), left_fn_(std::move(left_fn)),
        right_fn_(std::move(right_fn)), keys_(std::move(keys)),
        residuals_(std::move(residuals)) {}

  void step() override {
    output_.clear();
    has_output_ = false;
    const DuckDBZSet &dl = left_fn_();
    const DuckDBZSet &dr = right_fn_();
    if (dl.empty() && dr.empty()) {
      return;
    }

    // Δl ⋈ R_old
    for (const auto &[lrow, lw] : dl) {
      DuckDBRow key;
      if (!eval_key(lrow, /*left=*/true, key)) {
        continue;
      }
      auto it = right_index_.find(key);
      if (it == right_index_.end()) {
        continue;
      }
      for (const auto &[rrow, rw] : it->second) {
        try_emit(lrow, rrow, lw * rw);
      }
    }

    // L_old ⋈ Δr
    for (const auto &[rrow, rw] : dr) {
      DuckDBRow key;
      if (!eval_key(rrow, /*left=*/false, key)) {
        continue;
      }
      auto it = left_index_.find(key);
      if (it == left_index_.end()) {
        continue;
      }
      for (const auto &[lrow, lw] : it->second) {
        try_emit(lrow, rrow, lw * rw);
      }
    }

    // Δl ⋈ Δr (both sides changed in the same step, e.g. self-joins)
    if (!dl.empty() && !dr.empty()) {
      Index dr_index;
      for (const auto &[rrow, rw] : dr) {
        DuckDBRow key;
        if (eval_key(rrow, /*left=*/false, key)) {
          dr_index[key][rrow] += rw;
        }
      }
      for (const auto &[lrow, lw] : dl) {
        DuckDBRow key;
        if (!eval_key(lrow, /*left=*/true, key)) {
          continue;
        }
        auto it = dr_index.find(key);
        if (it == dr_index.end()) {
          continue;
        }
        for (const auto &[rrow, rw] : it->second) {
          try_emit(lrow, rrow, lw * rw);
        }
      }
    }

    // Integrate deltas into the side indexes
    integrate(dl, /*left=*/true);
    integrate(dr, /*left=*/false);
    has_output_ = !output_.empty();
  }

  void reset() override {
    left_index_.clear();
    right_index_.clear();
    output_.clear();
    has_output_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

private:
  using RowWeights = std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash>;
  using Index = std::unordered_map<DuckDBRow, RowWeights, DuckDBRowHash>;

  // Returns false when any key value is NULL (row can never match)
  bool eval_key(const DuckDBRow &row, bool left, DuckDBRow &key) {
    key.columns.reserve(keys_.size());
    for (auto &k : keys_) {
      duckdb::Value v = left ? k.left->eval(row) : k.right->eval(row);
      if (v.IsNull()) {
        return false;
      }
      key.columns.push_back(v);
    }
    return true;
  }

  bool residuals_pass(const DuckDBRow &lrow, const DuckDBRow &rrow) {
    for (auto &res : residuals_) {
      duckdb::Value lv = res.left->eval(lrow);
      duckdb::Value rv = res.right->eval(rrow);
      if (lv.IsNull() || rv.IsNull()) {
        return false;
      }
      bool pass = false;
      switch (res.cmp) {
      case duckdb::ExpressionType::COMPARE_GREATERTHAN:
        pass = rv < lv;
        break;
      case duckdb::ExpressionType::COMPARE_LESSTHAN:
        pass = lv < rv;
        break;
      case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        pass = !(lv < rv);
        break;
      case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
        pass = !(rv < lv);
        break;
      case duckdb::ExpressionType::COMPARE_NOTEQUAL:
        pass = lv != rv;
        break;
      case duckdb::ExpressionType::COMPARE_EQUAL:
        pass = lv == rv;
        break;
      default:
        return false;
      }
      if (!pass) {
        return false;
      }
    }
    return true;
  }

  void try_emit(const DuckDBRow &lrow, const DuckDBRow &rrow,
                int64_t weight) {
    if (weight == 0 || !residuals_pass(lrow, rrow)) {
      return;
    }
    DuckDBRow combined;
    combined.columns.reserve(lrow.columns.size() + rrow.columns.size());
    combined.columns = lrow.columns;
    combined.columns.insert(combined.columns.end(), rrow.columns.begin(),
                            rrow.columns.end());
    output_.insert(combined, weight);
  }

  void integrate(const DuckDBZSet &delta, bool left) {
    Index &index = left ? left_index_ : right_index_;
    for (const auto &[row, w] : delta) {
      DuckDBRow key;
      if (!eval_key(row, left, key)) {
        continue;
      }
      auto &rows = index[key];
      int64_t &weight = rows[row];
      weight += w;
      if (weight == 0) {
        rows.erase(row);
        if (rows.empty()) {
          index.erase(key);
        }
      }
    }
  }

  InputFn left_fn_, right_fn_;
  std::vector<KeyPair> keys_;
  std::vector<Residual> residuals_;
  Index left_index_, right_index_;
  DuckDBZSet output_;
  bool has_output_ = false;
};

// Incremental DISTINCT (B3): tracks integrated multiplicity per row; emits
// +1 when a row's count crosses 0 -> positive, -1 when it drops to 0.
class PlanDistinctNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  PlanDistinctNode(dbsp::NodeId id, InputFn input_fn,
                   std::string name = "plan_distinct")
      : dbsp::Node(id, std::move(name)), input_fn_(std::move(input_fn)) {}

  void step() override {
    output_.clear();
    has_output_ = false;
    const DuckDBZSet &changes = input_fn_();
    for (const auto &[row, w] : changes) {
      int64_t &count = counts_[row];
      int64_t old_count = count;
      count += w;
      if (old_count <= 0 && count > 0) {
        output_.insert(row, 1);
      } else if (old_count > 0 && count <= 0) {
        output_.insert(row, -1);
      }
      if (count == 0) {
        counts_.erase(row);
      }
    }
    has_output_ = !output_.empty();
  }

  void reset() override {
    counts_.clear();
    output_.clear();
    has_output_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

private:
  InputFn input_fn_;
  std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> counts_;
  DuckDBZSet output_;
  bool has_output_ = false;
};

// Incremental set operation (B3). Tracks integrated per-input multiplicity
// for every row and emits the change in output multiplicity:
//   UNION ALL      Σ counts             (stateless in principle, but kept
//                                        uniform for simplicity)
//   UNION          Σ counts > 0 ? 1 : 0
//   INTERSECT ALL  min(counts)          (2 inputs)
//   INTERSECT      all counts > 0 ? 1:0 (2 inputs)
//   EXCEPT ALL     max(a - b, 0)        (2 inputs)
//   EXCEPT         a > 0 && b == 0 ?1:0 (2 inputs)
class PlanSetOpNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;
  using SetOp = PlanOpSpec::SetOp;

  PlanSetOpNode(dbsp::NodeId id, std::vector<InputFn> inputs, SetOp op,
                std::string name = "plan_setop")
      : dbsp::Node(id, std::move(name)), inputs_(std::move(inputs)), op_(op) {}

  void step() override {
    output_.clear();
    has_output_ = false;

    // Collect changed rows and apply deltas per input
    std::unordered_map<DuckDBRow, std::vector<int64_t>, DuckDBRowHash>
        old_counts;
    for (size_t i = 0; i < inputs_.size(); i++) {
      const DuckDBZSet &delta = inputs_[i]();
      for (const auto &[row, w] : delta) {
        auto &counts = counts_[row];
        counts.resize(inputs_.size(), 0);
        if (!old_counts.count(row)) {
          old_counts[row] = counts;
        }
        counts[i] += w;
      }
    }

    for (const auto &[row, old] : old_counts) {
      auto it = counts_.find(row);
      const std::vector<int64_t> &now = it->second;
      int64_t delta = multiplicity(now) - multiplicity(old);
      if (delta != 0) {
        output_.insert(row, delta);
      }
      bool all_zero = true;
      for (int64_t c : now) {
        if (c != 0) {
          all_zero = false;
          break;
        }
      }
      if (all_zero) {
        counts_.erase(it);
      }
    }
    has_output_ = !output_.empty();
  }

  void reset() override {
    counts_.clear();
    output_.clear();
    has_output_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

private:
  int64_t multiplicity(const std::vector<int64_t> &counts) const {
    auto at = [&](size_t i) {
      return i < counts.size() ? std::max<int64_t>(counts[i], 0) : 0;
    };
    switch (op_) {
    case SetOp::UNION_ALL: {
      int64_t total = 0;
      for (size_t i = 0; i < counts.size(); i++) {
        total += at(i);
      }
      return total;
    }
    case SetOp::UNION: {
      for (size_t i = 0; i < counts.size(); i++) {
        if (at(i) > 0) {
          return 1;
        }
      }
      return 0;
    }
    case SetOp::INTERSECT_ALL:
      return std::min(at(0), at(1));
    case SetOp::INTERSECT:
      return at(0) > 0 && at(1) > 0 ? 1 : 0;
    case SetOp::EXCEPT_ALL:
      return std::max<int64_t>(at(0) - at(1), 0);
    case SetOp::EXCEPT:
      return at(0) > 0 && at(1) == 0 ? 1 : 0;
    }
    return 0;
  }

  std::vector<InputFn> inputs_;
  SetOp op_;
  std::unordered_map<DuckDBRow, std::vector<int64_t>, DuckDBRowHash> counts_;
  DuckDBZSet output_;
  bool has_output_ = false;
};

// Runs an existing NativeMaterializedView as a circuit node fed from any
// upstream node (not just a base table). Used for view types with proven
// incremental logic that isn't decomposed into fine-grained nodes yet —
// currently NativeWindowView. The wrapped view is constructed with
// kInputName as its source table; every circuit step feeds it the upstream
// delta and exposes the view's own delta as this node's output.
class EmbeddedViewNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;
  static constexpr const char *kInputName = "__plan_embedded_input__";

  EmbeddedViewNode(dbsp::NodeId id, InputFn input_fn,
                   std::unique_ptr<NativeMaterializedView> view)
      : dbsp::Node(id, view->name() + "_embedded"),
        input_fn_(std::move(input_fn)), view_(std::move(view)) {}

  void step() override {
    // The wrapped view clears its delta on every apply_changes call, so an
    // empty upstream delta correctly yields an empty output
    view_->apply_changes(kInputName, input_fn_());
  }

  void reset() override { view_->reset(); }

  bool has_output() const override { return !view_->get_delta().empty(); }

  const DuckDBZSet &output() const { return view_->get_delta(); }

private:
  InputFn input_fn_;
  std::unique_ptr<NativeMaterializedView> view_;
};

// Circuit view built from a translated plan tree. One SourceNode per base
// table (shared across subtrees, e.g. self-joins); apply_changes pushes the
// delta into the matching source and steps the whole circuit once.
class PlannedCircuitView : public NativeMaterializedView {
public:
  using RowSource = dbsp::SourceNode<DuckDBRow, DuckDBRowHash>;
  using RowSink = dbsp::SinkNode<DuckDBRow, DuckDBRowHash>;
  using RowFilter = dbsp::FilterNode<DuckDBRow, DuckDBRowHash>;
  using RowMap =
      dbsp::MapNode<DuckDBRow, DuckDBRow, DuckDBRowHash, DuckDBRowHash>;
  using OutputFn = std::function<const DuckDBZSet &()>;

  PlannedCircuitView(const std::string &name, const std::string &sql,
                     const TableSchema &result_schema,
                     std::shared_ptr<PlanKeepAlive> keep_alive,
                     const PlanOpSpec &root)
      : NativeMaterializedView(name, sql), schema_(result_schema) {
    schema_.table_name = name;
    OutputFn out = build(root, keep_alive);
    sink_ = circuit_.add_node(std::make_unique<RowSink>(
        circuit_.next_node_id(), std::move(out), name_ + "_sink"));
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    auto it = sources_.find(table_name);
    if (it != sources_.end()) {
      it->second->push(changes);
    }
    circuit_.step();
    ++version_;
  }

  const DuckDBZSet &get_result() const override {
    return sink_->materialized();
  }

  void set_result(const DuckDBZSet &result) override {
    sink_->set_materialized(result);
    version_++;
  }

  const DuckDBZSet &get_delta() const override { return sink_->delta(); }

  const TableSchema &result_schema() const override { return schema_; }

  std::vector<std::string> source_tables() const override {
    return source_order_;
  }

  void reset() override {
    circuit_.reset();
    version_ = 0;
  }

  // Root ORDER BY/LIMIT: delegate to the embedded sort/limit view so
  // dbsp_query sees rows in ORDER BY order (content identical to the sink)
  void scan(const std::function<void(const DuckDBRow &, Weight)> &callback)
      const override {
    if (ordered_view_) {
      ordered_view_->scan(callback);
      return;
    }
    NativeMaterializedView::scan(callback);
  }

private:
  OutputFn build(const PlanOpSpec &spec,
                 const std::shared_ptr<PlanKeepAlive> &keep_alive) {
    switch (spec.kind) {
    case PlanOpSpec::Kind::SOURCE: {
      auto it = sources_.find(spec.table);
      RowSource *src;
      if (it != sources_.end()) {
        src = it->second;
      } else {
        src = circuit_.add_node(
            std::make_unique<RowSource>(circuit_.next_node_id(), spec.table));
        sources_[spec.table] = src;
        source_order_.push_back(spec.table);
      }
      return [src]() -> const DuckDBZSet & { return src->output(); };
    }
    case PlanOpSpec::Kind::MAP_COLS: {
      OutputFn child = build(*spec.children[0], keep_alive);
      auto idxs = spec.column_idxs;
      auto *node = circuit_.add_node(std::make_unique<RowMap>(
          circuit_.next_node_id(), std::move(child),
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
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::FILTER_EXPR: {
      OutputFn child = build(*spec.children[0], keep_alive);
      auto evals =
          std::make_shared<std::vector<std::unique_ptr<RowExprEval>>>();
      for (const auto *expr : spec.exprs) {
        evals->push_back(std::make_unique<RowExprEval>(keep_alive, *expr,
                                                       spec.input_types));
      }
      auto *node = circuit_.add_node(std::make_unique<RowFilter>(
          circuit_.next_node_id(), std::move(child),
          [evals](const DuckDBRow &row) {
            for (auto &eval : *evals) {
              duckdb::Value v = eval->eval(row);
              if (v.IsNull() || !v.GetValue<bool>()) {
                return false;
              }
            }
            return true;
          },
          "plan_filter"));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::MAP_EXPR: {
      OutputFn child = build(*spec.children[0], keep_alive);
      auto evals =
          std::make_shared<std::vector<std::unique_ptr<RowExprEval>>>();
      for (const auto *expr : spec.exprs) {
        evals->push_back(std::make_unique<RowExprEval>(keep_alive, *expr,
                                                       spec.input_types));
      }
      auto *node = circuit_.add_node(std::make_unique<RowMap>(
          circuit_.next_node_id(), std::move(child),
          [evals](const DuckDBRow &row) {
            DuckDBRow out;
            out.columns.reserve(evals->size());
            for (auto &eval : *evals) {
              out.columns.push_back(eval->eval(row));
            }
            return out;
          },
          "plan_project"));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::AGGREGATE: {
      OutputFn child = build(*spec.children[0], keep_alive);
      std::vector<std::unique_ptr<RowExprEval>> group_evals;
      for (const auto *expr : spec.exprs) {
        group_evals.push_back(std::make_unique<RowExprEval>(
            keep_alive, *expr, spec.input_types));
      }
      std::vector<PlanAggregateNode::AggInstance> aggs;
      for (const auto &agg_spec : spec.agg_specs) {
        PlanAggregateNode::AggInstance inst;
        inst.fn = agg_spec.fn;
        inst.integer_arg = agg_spec.integer_arg;
        inst.return_type = agg_spec.return_type;
        if (agg_spec.arg) {
          inst.arg = std::make_unique<RowExprEval>(keep_alive, *agg_spec.arg,
                                                   spec.input_types);
        }
        aggs.push_back(std::move(inst));
      }
      auto *node = circuit_.add_node(std::make_unique<PlanAggregateNode>(
          circuit_.next_node_id(), std::move(child), std::move(group_evals),
          std::move(aggs)));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::JOIN: {
      OutputFn left = build(*spec.children[0], keep_alive);
      OutputFn right = build(*spec.children[1], keep_alive);
      std::vector<PlanJoinNode::KeyPair> keys;
      for (const auto &cond : spec.equi_conds) {
        PlanJoinNode::KeyPair kp;
        kp.left = std::make_unique<RowExprEval>(keep_alive, *cond.left,
                                                spec.left_types);
        kp.right = std::make_unique<RowExprEval>(keep_alive, *cond.right,
                                                 spec.right_types);
        keys.push_back(std::move(kp));
      }
      std::vector<PlanJoinNode::Residual> residuals;
      for (const auto &cond : spec.residual_conds) {
        PlanJoinNode::Residual res;
        res.left = std::make_unique<RowExprEval>(keep_alive, *cond.left,
                                                 spec.left_types);
        res.right = std::make_unique<RowExprEval>(keep_alive, *cond.right,
                                                  spec.right_types);
        res.cmp = cond.cmp;
        residuals.push_back(std::move(res));
      }
      auto *node = circuit_.add_node(std::make_unique<PlanJoinNode>(
          circuit_.next_node_id(), std::move(left), std::move(right),
          std::move(keys), std::move(residuals)));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::DISTINCT: {
      OutputFn child = build(*spec.children[0], keep_alive);
      auto *node = circuit_.add_node(std::make_unique<PlanDistinctNode>(
          circuit_.next_node_id(), std::move(child)));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::SET_OP: {
      std::vector<PlanSetOpNode::InputFn> inputs;
      for (const auto &child : spec.children) {
        inputs.push_back(build(*child, keep_alive));
      }
      auto *node = circuit_.add_node(std::make_unique<PlanSetOpNode>(
          circuit_.next_node_id(), std::move(inputs), spec.set_op));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::WINDOW: {
      OutputFn child = build(*spec.children[0], keep_alive);
      TableSchema source_schema;
      source_schema.table_name = EmbeddedViewNode::kInputName;
      source_schema.columns = spec.window_source_cols;
      TableSchema window_schema;
      window_schema.table_name = name_ + "_window";
      window_schema.columns = spec.window_result_cols;
      auto view = std::make_unique<NativeWindowView>(
          name_ + "_window", "", EmbeddedViewNode::kInputName, window_schema,
          source_schema, spec.window_defs);
      auto *node = circuit_.add_node(std::make_unique<EmbeddedViewNode>(
          circuit_.next_node_id(), std::move(child), std::move(view)));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::CTE: {
      // Build the definition once; all CTE_REFs share its output
      cte_outputs_[spec.cte_index] = build(*spec.children[0], keep_alive);
      return build(*spec.children[1], keep_alive);
    }
    case PlanOpSpec::Kind::CTE_REF:
      return cte_outputs_.at(spec.cte_index);
    case PlanOpSpec::Kind::SORT_LIMIT: {
      OutputFn child = build(*spec.children[0], keep_alive);
      TableSchema vschema;
      vschema.table_name = name_ + "_sortlimit";
      NativeSortView::ProjectFn project = nullptr;
      if (!spec.project_idxs.empty()) {
        auto idxs = spec.project_idxs;
        project = [idxs](const DuckDBRow &row) {
          DuckDBRow out;
          out.columns.reserve(idxs.size());
          for (auto i : idxs) {
            out.columns.push_back(i < row.columns.size() ? row.columns[i]
                                                         : duckdb::Value());
          }
          return out;
        };
      }
      std::unique_ptr<NativeMaterializedView> view;
      if (spec.limit >= 0 || spec.offset > 0) {
        view = std::make_unique<NativeLimitView>(
            name_ + "_limit", "", EmbeddedViewNode::kInputName, vschema,
            spec.limit, spec.offset, spec.sort_columns, project);
      } else {
        view = std::make_unique<NativeSortView>(
            name_ + "_sort", "", EmbeddedViewNode::kInputName, vschema,
            spec.sort_columns, project);
      }
      if (spec.presentation_root) {
        ordered_view_ = view.get();
      }
      auto *node = circuit_.add_node(std::make_unique<EmbeddedViewNode>(
          circuit_.next_node_id(), std::move(child), std::move(view)));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    }
    // Unreachable; keeps compilers happy
    static DuckDBZSet empty;
    return []() -> const DuckDBZSet & { return empty; };
  }

  dbsp::Circuit circuit_;
  TableSchema schema_;
  std::unordered_map<std::string, RowSource *> sources_;
  std::vector<std::string> source_order_;
  std::unordered_map<duckdb::idx_t, OutputFn> cte_outputs_;
  RowSink *sink_ = nullptr;
  // Embedded sort/limit view at the plan root; owned by its EmbeddedViewNode
  NativeMaterializedView *ordered_view_ = nullptr;
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
    auto root = walker.visit(*keep_alive->plan);
    if (!root) {
      return {nullptr, walker.error};
    }
    if (root->kind == PlanOpSpec::Kind::SORT_LIMIT) {
      // Only a root sort/limit drives dbsp_query's scan order; nested ones
      // (subqueries) affect membership only
      root->presentation_root = true;
    }

    TableSchema schema;
    schema.table_name = view_name;
    schema.columns = walker.columns;
    // Deduplicate column names (e.g. t.val and u.val in a join): repeated
    // names would make the result unqueryable through dbsp_query
    std::unordered_map<std::string, int> seen;
    for (auto &col : schema.columns) {
      int &n = seen[col.name];
      if (n++ > 0) {
        col.name += "_" + std::to_string(n - 1);
      }
    }

    auto view = std::make_unique<PlannedCircuitView>(
        view_name, sql, schema, std::move(keep_alive), *root);
    return {std::move(view), ""};
  }

private:
  struct Walker {
    std::vector<ColumnInfo> columns; // schema of current operator's output
    std::string error;

    using SpecPtr = std::unique_ptr<PlanOpSpec>;

    SpecPtr unsupported(const std::string &what) {
      error = format_error_code(ErrorCode::PLAN_OPERATOR_NOT_SUPPORTED) +
              ": unsupported in planner frontend: " + what;
      return nullptr;
    }

    SpecPtr visit(duckdb::LogicalOperator &op) {
      switch (op.type) {
      case duckdb::LogicalOperatorType::LOGICAL_PROJECTION:
        return visit_projection(op.Cast<duckdb::LogicalProjection>());
      case duckdb::LogicalOperatorType::LOGICAL_FILTER:
        return visit_filter(op.Cast<duckdb::LogicalFilter>());
      case duckdb::LogicalOperatorType::LOGICAL_GET:
        return visit_get(op.Cast<duckdb::LogicalGet>());
      case duckdb::LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
        return visit_aggregate(op.Cast<duckdb::LogicalAggregate>());
      case duckdb::LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
        return visit_join(op.Cast<duckdb::LogicalComparisonJoin>());
      case duckdb::LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
        return visit_cross_product(op);
      case duckdb::LogicalOperatorType::LOGICAL_DISTINCT:
        return visit_distinct(op.Cast<duckdb::LogicalDistinct>());
      case duckdb::LogicalOperatorType::LOGICAL_UNION:
      case duckdb::LogicalOperatorType::LOGICAL_INTERSECT:
      case duckdb::LogicalOperatorType::LOGICAL_EXCEPT:
        return visit_set_operation(op.Cast<duckdb::LogicalSetOperation>());
      case duckdb::LogicalOperatorType::LOGICAL_WINDOW:
        return visit_window(op.Cast<duckdb::LogicalWindow>());
      case duckdb::LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
        return visit_cte(op.Cast<duckdb::LogicalMaterializedCTE>());
      case duckdb::LogicalOperatorType::LOGICAL_CTE_REF:
        return visit_cte_ref(op.Cast<duckdb::LogicalCTERef>());
      case duckdb::LogicalOperatorType::LOGICAL_ORDER_BY:
        return visit_order(op.Cast<duckdb::LogicalOrder>(), /*limit=*/-1,
                           /*offset=*/0);
      case duckdb::LogicalOperatorType::LOGICAL_LIMIT:
        return visit_limit(op.Cast<duckdb::LogicalLimit>());
      case duckdb::LogicalOperatorType::LOGICAL_DELIM_JOIN:
      case duckdb::LogicalOperatorType::LOGICAL_DELIM_GET:
        return unsupported(
            "correlated subquery (DELIM_JOIN) — rewrite as a JOIN "
            "or create an intermediate view");
      case duckdb::LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
        return unsupported(
            "recursive CTE — handled by the parser frontend");
      default:
        return unsupported("logical operator " + op.GetName());
      }
    }

    SpecPtr visit_projection(duckdb::LogicalProjection &op) {
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }
      // A pure column-ref projection directly above ORDER BY/LIMIT folds into
      // the sort/limit view as its ProjectFn: sort keys dropped from the
      // SELECT list still order the output (the view sorts full input rows)
      if (child->kind == PlanOpSpec::Kind::SORT_LIMIT &&
          child->project_idxs.empty()) {
        bool pure_refs = true;
        for (const auto &expr : op.expressions) {
          if (expr->GetExpressionClass() !=
              duckdb::ExpressionClass::BOUND_REF) {
            pure_refs = false;
            break;
          }
        }
        if (pure_refs) {
          for (const auto &expr : op.expressions) {
            child->project_idxs.push_back(
                expr->Cast<duckdb::BoundReferenceExpression>().index);
          }
          columns.clear();
          for (duckdb::idx_t i = 0; i < op.expressions.size(); i++) {
            columns.push_back({op.expressions[i]->GetName(), op.types[i]});
          }
          return child;
        }
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::MAP_EXPR;
      spec->input_types = op.children[0]->types;
      for (const auto &expr : op.expressions) {
        spec->exprs.push_back(expr.get());
      }
      spec->children.push_back(std::move(child));

      columns.clear();
      for (duckdb::idx_t i = 0; i < op.expressions.size(); i++) {
        columns.push_back({op.expressions[i]->GetName(), op.types[i]});
      }
      return spec;
    }

    SpecPtr visit_filter(duckdb::LogicalFilter &op) {
      if (!op.projection_map.empty()) {
        return unsupported("FILTER with projection map");
      }
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::FILTER_EXPR;
      spec->input_types = op.children[0]->types;
      for (const auto &expr : op.expressions) {
        spec->exprs.push_back(expr.get());
      }
      spec->children.push_back(std::move(child));
      // Filter passes rows through unchanged; columns stay as-is
      return spec;
    }

    SpecPtr visit_limit(duckdb::LogicalLimit &op) {
      int64_t limit = -1, offset = 0;
      using LT = duckdb::LimitNodeType;
      if (op.limit_val.Type() == LT::CONSTANT_VALUE) {
        limit = static_cast<int64_t>(op.limit_val.GetConstantValue());
      } else if (op.limit_val.Type() != LT::UNSET) {
        return unsupported("non-constant LIMIT");
      }
      if (op.offset_val.Type() == LT::CONSTANT_VALUE) {
        offset = static_cast<int64_t>(op.offset_val.GetConstantValue());
      } else if (op.offset_val.Type() != LT::UNSET) {
        return unsupported("non-constant OFFSET");
      }
      auto &child = *op.children[0];
      if (child.type == duckdb::LogicalOperatorType::LOGICAL_ORDER_BY) {
        return visit_order(child.Cast<duckdb::LogicalOrder>(), limit, offset);
      }
      auto child_spec = visit(child);
      if (!child_spec) {
        return nullptr;
      }
      return make_sort_limit(std::move(child_spec), {}, limit, offset);
    }

    SpecPtr visit_order(duckdb::LogicalOrder &op, int64_t limit,
                        int64_t offset) {
      if (!op.projection_map.empty()) {
        return unsupported("ORDER BY with projection map");
      }
      std::vector<NativeSortView::SortColumn> cols;
      for (auto &o : op.orders) {
        if (o.expression->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_REF) {
          return unsupported("ORDER BY expression (use a plain column)");
        }
        auto &ref = o.expression->Cast<duckdb::BoundReferenceExpression>();
        NativeSortView::SortColumn sc;
        sc.column_idx = static_cast<size_t>(ref.index);
        sc.ascending = o.type != duckdb::OrderType::DESCENDING;
        sc.nulls_first = o.null_order == duckdb::OrderByNullType::NULLS_FIRST;
        cols.push_back(sc);
      }
      auto child_spec = visit(*op.children[0]);
      if (!child_spec) {
        return nullptr;
      }
      return make_sort_limit(std::move(child_spec), std::move(cols), limit,
                             offset);
    }

    // ORDER BY/LIMIT pass rows through (LIMIT changes membership, not
    // layout): columns stay as the child left them
    SpecPtr make_sort_limit(SpecPtr child,
                            std::vector<NativeSortView::SortColumn> cols,
                            int64_t limit, int64_t offset) {
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::SORT_LIMIT;
      spec->sort_columns = std::move(cols);
      spec->limit = limit;
      spec->offset = offset;
      spec->children.push_back(std::move(child));
      return spec;
    }

    SpecPtr visit_aggregate(duckdb::LogicalAggregate &op) {
      if (!op.grouping_functions.empty()) {
        return unsupported("GROUPING function");
      }
      if (op.grouping_sets.size() > 1) {
        return unsupported("ROLLUP/CUBE/GROUPING SETS");
      }
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }

      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::AGGREGATE;
      spec->input_types = op.children[0]->types;
      for (const auto &group : op.groups) {
        spec->exprs.push_back(group.get());
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

        PlanAggSpec agg_spec;
        agg_spec.return_type = agg.return_type;
        const std::string &fn = agg.function.name;
        if (fn == "count_star") {
          agg_spec.fn = PlanAggSpec::Fn::COUNT_STAR;
        } else if (fn == "count") {
          agg_spec.fn = PlanAggSpec::Fn::COUNT;
        } else if (fn == "sum" || fn == "sum_no_overflow") {
          agg_spec.fn = PlanAggSpec::Fn::SUM;
        } else if (fn == "avg") {
          agg_spec.fn = PlanAggSpec::Fn::AVG;
        } else if (fn == "min") {
          agg_spec.fn = PlanAggSpec::Fn::MIN;
        } else if (fn == "max") {
          agg_spec.fn = PlanAggSpec::Fn::MAX;
        } else {
          return unsupported("aggregate function " + fn);
        }

        if (agg_spec.fn != PlanAggSpec::Fn::COUNT_STAR) {
          if (agg.children.size() != 1) {
            return unsupported("aggregate with " +
                               std::to_string(agg.children.size()) +
                               " arguments (" + fn + ")");
          }
          agg_spec.arg = agg.children[0].get();
          if (agg_spec.fn == PlanAggSpec::Fn::SUM ||
              agg_spec.fn == PlanAggSpec::Fn::AVG) {
            switch (agg_spec.arg->return_type.id()) {
            case duckdb::LogicalTypeId::TINYINT:
            case duckdb::LogicalTypeId::SMALLINT:
            case duckdb::LogicalTypeId::INTEGER:
            case duckdb::LogicalTypeId::BIGINT:
            case duckdb::LogicalTypeId::UTINYINT:
            case duckdb::LogicalTypeId::USMALLINT:
            case duckdb::LogicalTypeId::UINTEGER:
              agg_spec.integer_arg = true;
              break;
            case duckdb::LogicalTypeId::FLOAT:
            case duckdb::LogicalTypeId::DOUBLE:
              agg_spec.integer_arg = false;
              break;
            default:
              return unsupported(fn + " over " +
                                 agg_spec.arg->return_type.ToString());
            }
          }
        }
        spec->agg_specs.push_back(std::move(agg_spec));
      }
      spec->children.push_back(std::move(child));

      // Output layout: group values first, then aggregate values
      columns.clear();
      for (duckdb::idx_t i = 0; i < op.groups.size(); i++) {
        columns.push_back({op.groups[i]->GetName(), op.types[i]});
      }
      for (duckdb::idx_t i = 0; i < op.expressions.size(); i++) {
        columns.push_back(
            {op.expressions[i]->GetName(), op.types[op.groups.size() + i]});
      }
      return spec;
    }

    SpecPtr visit_join(duckdb::LogicalComparisonJoin &op) {
      if (op.join_type != duckdb::JoinType::INNER) {
        return unsupported("join type " +
                           duckdb::EnumUtil::ToString(op.join_type));
      }
      if (!op.duplicate_eliminated_columns.empty()) {
        return unsupported("duplicate-eliminated (DELIM) join");
      }
      if (op.predicate) {
        return unsupported("join with single-sided ON predicate");
      }
      if (!op.left_projection_map.empty() ||
          !op.right_projection_map.empty()) {
        return unsupported("join with projection maps");
      }

      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::JOIN;
      for (const auto &cond : op.conditions) {
        PlanOpSpec::JoinCond jc{cond.left.get(), cond.right.get(),
                                cond.comparison};
        switch (cond.comparison) {
        case duckdb::ExpressionType::COMPARE_EQUAL:
          spec->equi_conds.push_back(jc);
          break;
        case duckdb::ExpressionType::COMPARE_GREATERTHAN:
        case duckdb::ExpressionType::COMPARE_LESSTHAN:
        case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
        case duckdb::ExpressionType::COMPARE_NOTEQUAL:
          spec->residual_conds.push_back(jc);
          break;
        default:
          return unsupported(
              "join comparison " +
              duckdb::EnumUtil::ToString(cond.comparison));
        }
      }

      auto left = visit(*op.children[0]);
      if (!left) {
        return nullptr;
      }
      std::vector<ColumnInfo> left_columns = columns;
      auto right = visit(*op.children[1]);
      if (!right) {
        return nullptr;
      }

      spec->left_types = op.children[0]->types;
      spec->right_types = op.children[1]->types;
      spec->children.push_back(std::move(left));
      spec->children.push_back(std::move(right));

      // Output: left columns then right columns
      std::vector<ColumnInfo> combined = std::move(left_columns);
      combined.insert(combined.end(), columns.begin(), columns.end());
      columns = std::move(combined);
      return spec;
    }

    SpecPtr visit_cross_product(duckdb::LogicalOperator &op) {
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::JOIN; // no conditions = cross product

      auto left = visit(*op.children[0]);
      if (!left) {
        return nullptr;
      }
      std::vector<ColumnInfo> left_columns = columns;
      auto right = visit(*op.children[1]);
      if (!right) {
        return nullptr;
      }

      spec->left_types = op.children[0]->types;
      spec->right_types = op.children[1]->types;
      spec->children.push_back(std::move(left));
      spec->children.push_back(std::move(right));

      std::vector<ColumnInfo> combined = std::move(left_columns);
      combined.insert(combined.end(), columns.begin(), columns.end());
      columns = std::move(combined);
      return spec;
    }

    SpecPtr visit_distinct(duckdb::LogicalDistinct &op) {
      if (op.distinct_type != duckdb::DistinctType::DISTINCT) {
        return unsupported("DISTINCT ON");
      }
      // Plain DISTINCT deduplicates whole rows; targets must be the full
      // identity column list, otherwise semantics differ
      const auto &child_types = op.children[0]->types;
      if (op.distinct_targets.size() != child_types.size()) {
        return unsupported("DISTINCT over a subset of columns");
      }
      for (duckdb::idx_t i = 0; i < op.distinct_targets.size(); i++) {
        auto &target = op.distinct_targets[i];
        if (target->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_REF) {
          return unsupported("DISTINCT over computed targets");
        }
        if (target->Cast<duckdb::BoundReferenceExpression>().index != i) {
          return unsupported("DISTINCT with reordered targets");
        }
      }
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::DISTINCT;
      spec->children.push_back(std::move(child));
      // Row shape unchanged; columns stay as-is
      return spec;
    }

    SpecPtr visit_set_operation(duckdb::LogicalSetOperation &op) {
      bool is_union =
          op.type == duckdb::LogicalOperatorType::LOGICAL_UNION;
      if (!is_union && op.children.size() != 2) {
        return unsupported("n-ary INTERSECT/EXCEPT");
      }

      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::SET_OP;
      if (is_union) {
        spec->set_op = op.setop_all ? PlanOpSpec::SetOp::UNION_ALL
                                    : PlanOpSpec::SetOp::UNION;
      } else if (op.type ==
                 duckdb::LogicalOperatorType::LOGICAL_INTERSECT) {
        spec->set_op = op.setop_all ? PlanOpSpec::SetOp::INTERSECT_ALL
                                    : PlanOpSpec::SetOp::INTERSECT;
      } else {
        spec->set_op = op.setop_all ? PlanOpSpec::SetOp::EXCEPT_ALL
                                    : PlanOpSpec::SetOp::EXCEPT;
      }

      std::vector<ColumnInfo> first_columns;
      for (size_t i = 0; i < op.children.size(); i++) {
        auto child = visit(*op.children[i]);
        if (!child) {
          return nullptr;
        }
        if (i == 0) {
          first_columns = columns;
        }
        spec->children.push_back(std::move(child));
      }
      // Set operation output takes the first child's column names
      columns = std::move(first_columns);
      return spec;
    }

    // Extract a column index from a bound expression; -1 if not a plain ref
    static int column_ref(const duckdb::Expression &expr) {
      if (expr.GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) {
        return -1;
      }
      return static_cast<int>(
          expr.Cast<duckdb::BoundReferenceExpression>().index);
    }

    // Extract a constant int64; false if not a non-NULL constant
    static bool constant_int(const duckdb::Expression &expr, int64_t &out) {
      if (expr.GetExpressionClass() !=
          duckdb::ExpressionClass::BOUND_CONSTANT) {
        return false;
      }
      const auto &val = expr.Cast<duckdb::BoundConstantExpression>().value;
      if (val.IsNull() || !val.type().IsNumeric()) {
        return false;
      }
      out = val.GetValue<int64_t>();
      return true;
    }

    SpecPtr visit_window(duckdb::LogicalWindow &op) {
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::WINDOW;
      spec->window_source_cols = columns;

      for (const auto &expr : op.expressions) {
        if (expr->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_WINDOW) {
          return unsupported("non-window expression in WINDOW");
        }
        auto &w = expr->Cast<duckdb::BoundWindowExpression>();
        if (w.filter_expr || w.distinct || w.ignore_nulls ||
            !w.arg_orders.empty() ||
            w.exclude_clause != duckdb::WindowExcludeMode::NO_OTHER) {
          return unsupported("window FILTER/DISTINCT/IGNORE NULLS/EXCLUDE");
        }

        NativeWindowView::WindowDef def;
        def.alias = expr->GetName();
        def.start = w.start;
        def.end = w.end;

        for (const auto &p : w.partitions) {
          int idx = column_ref(*p);
          if (idx < 0) {
            return unsupported("window PARTITION BY expression (use a "
                               "plain column)");
          }
          def.partition_indices.push_back(static_cast<size_t>(idx));
        }
        for (const auto &o : w.orders) {
          int idx = column_ref(*o.expression);
          if (idx < 0) {
            return unsupported("window ORDER BY expression (use a plain "
                               "column)");
          }
          NativeSortView::SortColumn sc;
          sc.column_idx = static_cast<size_t>(idx);
          sc.ascending = o.type != duckdb::OrderType::DESCENDING;
          sc.nulls_first = o.null_order == duckdb::OrderByNullType::NULLS_FIRST;
          def.sort_columns.push_back(sc);
        }

        int64_t n = 0;
        switch (w.GetExpressionType()) {
        case duckdb::ExpressionType::WINDOW_ROW_NUMBER:
          def.function = "ROW_NUMBER";
          break;
        case duckdb::ExpressionType::WINDOW_RANK:
          def.function = "RANK";
          break;
        case duckdb::ExpressionType::WINDOW_RANK_DENSE:
          def.function = "DENSE_RANK";
          break;
        case duckdb::ExpressionType::WINDOW_NTILE:
          def.function = "NTILE";
          if (w.children.empty() || !constant_int(*w.children[0], n)) {
            return unsupported("NTILE with non-constant bucket count");
          }
          def.offset = static_cast<int>(n);
          break;
        case duckdb::ExpressionType::WINDOW_LAG:
        case duckdb::ExpressionType::WINDOW_LEAD: {
          def.function = w.GetExpressionType() ==
                                 duckdb::ExpressionType::WINDOW_LAG
                             ? "LAG"
                             : "LEAD";
          if (w.children.empty()) {
            return unsupported(def.function + " without argument");
          }
          def.arg_column_idx = column_ref(*w.children[0]);
          if (def.arg_column_idx < 0) {
            return unsupported(def.function +
                               " over an expression (use a plain column)");
          }
          def.offset = 1;
          if (w.offset_expr) {
            if (!constant_int(*w.offset_expr, n)) {
              return unsupported(def.function + " with non-constant offset");
            }
            def.offset = static_cast<int>(n);
          }
          if (w.default_expr) {
            return unsupported(def.function + " with a default value");
          }
          break;
        }
        case duckdb::ExpressionType::WINDOW_FIRST_VALUE:
        case duckdb::ExpressionType::WINDOW_LAST_VALUE:
        case duckdb::ExpressionType::WINDOW_NTH_VALUE: {
          auto t = w.GetExpressionType();
          def.function =
              t == duckdb::ExpressionType::WINDOW_FIRST_VALUE
                  ? "FIRST_VALUE"
                  : t == duckdb::ExpressionType::WINDOW_LAST_VALUE
                        ? "LAST_VALUE"
                        : "NTH_VALUE";
          if (w.children.empty()) {
            return unsupported(def.function + " without argument");
          }
          def.arg_column_idx = column_ref(*w.children[0]);
          if (def.arg_column_idx < 0) {
            return unsupported(def.function +
                               " over an expression (use a plain column)");
          }
          if (t == duckdb::ExpressionType::WINDOW_NTH_VALUE) {
            if (w.children.size() < 2 || !constant_int(*w.children[1], n)) {
              return unsupported("NTH_VALUE with non-constant N");
            }
            def.offset = static_cast<int>(n);
          }
          break;
        }
        case duckdb::ExpressionType::WINDOW_AGGREGATE: {
          std::string fn =
              duckdb::StringUtil::Upper(w.aggregate ? w.aggregate->name : "");
          if (fn == "COUNT_STAR") {
            fn = "COUNT";
          }
          if (fn != "SUM" && fn != "COUNT" && fn != "AVG" && fn != "MIN" &&
              fn != "MAX") {
            return unsupported("window aggregate " + fn);
          }
          def.function = fn;
          if (!w.children.empty()) {
            def.arg_column_idx = column_ref(*w.children[0]);
            if (def.arg_column_idx < 0) {
              return unsupported(fn + " OVER an expression (use a plain "
                                      "column)");
            }
          }
          break;
        }
        default:
          return unsupported(
              "window function " +
              duckdb::EnumUtil::ToString(w.GetExpressionType()));
        }

        // Frame offsets must be constants when boundaries use expressions
        if (w.start == duckdb::WindowBoundary::EXPR_PRECEDING_ROWS) {
          if (!w.start_expr || !constant_int(*w.start_expr, n)) {
            return unsupported("non-constant frame start");
          }
          def.start_offset = static_cast<int>(n);
        }
        if (w.end == duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS) {
          if (!w.end_expr || !constant_int(*w.end_expr, n)) {
            return unsupported("non-constant frame end");
          }
          def.end_offset = static_cast<int>(n);
        }

        // NativeWindowView partitions all windows by the FIRST window's
        // partition/order; additional windows must match it
        if (!spec->window_defs.empty()) {
          const auto &first = spec->window_defs[0];
          bool same = first.partition_indices == def.partition_indices &&
                      first.sort_columns.size() == def.sort_columns.size();
          for (size_t i = 0; same && i < first.sort_columns.size(); i++) {
            same = first.sort_columns[i].column_idx ==
                       def.sort_columns[i].column_idx &&
                   first.sort_columns[i].ascending ==
                       def.sort_columns[i].ascending &&
                   first.sort_columns[i].nulls_first ==
                       def.sort_columns[i].nulls_first;
          }
          if (!same) {
            return unsupported("multiple windows with different "
                               "PARTITION BY / ORDER BY clauses");
          }
        }
        spec->window_defs.push_back(std::move(def));
      }

      // Output: child columns then one column per window expression
      duckdb::idx_t base = op.children[0]->types.size();
      for (duckdb::idx_t i = 0; i < op.expressions.size(); i++) {
        columns.push_back({op.expressions[i]->GetName(), op.types[base + i]});
      }
      spec->window_result_cols = columns;
      spec->children.push_back(std::move(child));
      return spec;
    }

    SpecPtr visit_cte(duckdb::LogicalMaterializedCTE &op) {
      auto def = visit(*op.children[0]);
      if (!def) {
        return nullptr;
      }
      cte_columns[op.table_index] = columns;
      auto main = visit(*op.children[1]);
      if (!main) {
        return nullptr;
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::CTE;
      spec->cte_index = op.table_index;
      spec->children.push_back(std::move(def));
      spec->children.push_back(std::move(main));
      // columns already reflect the main query's output
      return spec;
    }

    SpecPtr visit_cte_ref(duckdb::LogicalCTERef &op) {
      auto it = cte_columns.find(op.cte_index);
      if (it == cte_columns.end()) {
        return unsupported("reference to an untranslated CTE");
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::CTE_REF;
      spec->cte_index = op.cte_index;
      columns.clear();
      for (duckdb::idx_t i = 0; i < op.chunk_types.size(); i++) {
        std::string name = i < op.bound_columns.size()
                               ? op.bound_columns[i]
                               : it->second[i].name;
        columns.push_back({name, op.chunk_types[i]});
      }
      return spec;
    }

    std::unordered_map<duckdb::idx_t, std::vector<ColumnInfo>> cte_columns;

    SpecPtr visit_get(duckdb::LogicalGet &op) {
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

      auto source = std::make_unique<PlanOpSpec>();
      source->kind = PlanOpSpec::Kind::SOURCE;
      source->table = table_entry->name;

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
      if (identity) {
        return source;
      }
      auto select = std::make_unique<PlanOpSpec>();
      select->kind = PlanOpSpec::Kind::MAP_COLS;
      select->column_idxs = std::move(idxs);
      select->children.push_back(std::move(source));
      return select;
    }
  };
};

} // namespace dbsp_native

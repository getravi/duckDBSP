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
//   C2: LOGICAL_RECURSIVE_CTE via PlanRecursiveNode: anchor inline, the
//       recursive step as a nested PlannedCircuitView driven to a fixed
//       point (self-reference = sentinel source). Multi-table recursive
//       steps work; USING KEY is rejected. Insert-only deltas are
//       incremental; deltas containing deletions trigger a full fixed-point
//       recompute from integrated inputs (correct, non-incremental).
//   C3: DISTINCT ON via NativeDistinctOnView in an EmbeddedViewNode
//       (column-ref targets; winner-pick order from the DISTINCT node's own
//       order_by modifier — the ORDER_BY above is presentation, see C1).
//   C4: circuit-IR optimizer (plan_ir::optimize, g_plan_ir_optimize flag):
//       combine adjacent filters, push single-side filters below joins,
//       fuse MAP(FILTER(x)) into one batched node. Successor of the
//       ParsedViewDef-based DBSPOptimizer.
//   D1: vectorized evaluation — filter/map/fused nodes run expressions
//       over shared DataChunk batches (BatchEvaluator); sources/sinks
//       borrow deltas instead of copying.
//   D2: LEFT/RIGHT/FULL outer joins (incrementally reconciled NULL pads).
//   D3: MARK joins (IN/NOT IN, three-valued null-aware marks) and the
//       first() aggregate (uncorrelated scalar subquery comparisons).
// Any other operator yields a DBSP-E110 error naming the operator;
// CDCManager::create_view falls back to the bespoke parser transparently.
//
// Caller contract: PlanTranslator::translate issues queries on an internal
// connection, so the caller must hold an InternalQueryGuard (dbsp_cdc.hpp)
// to keep transaction-commit hooks from recursing into CDCManager.

#pragma once

#include "dbsp_circuit_views.hpp"
#include "dbsp_distinct_on.hpp"
#include "dbsp_errors.hpp"
#include "dbsp_window_view.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "core_functions/aggregate/quantile_helpers.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_delim_get.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_recursive_cte.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

#include <atomic>
#include <memory>
#include <set>
#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace dbsp_native {

// Keeps the internal connection and the extracted plan alive for as long as
// any node lambda references bound expressions or the client context.
struct PlanKeepAlive {
  std::unique_ptr<duckdb::Connection> connection;
  std::unique_ptr<duckdb::LogicalOperator> plan;
  // Expressions the IR optimizer rewrote (e.g. right-side join pushdown
  // shifts column indices): node lambdas reference them, so they must live
  // exactly as long as the plan itself
  std::vector<std::unique_ptr<duckdb::Expression>> rewritten_exprs;
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

// Batched adapter over ExpressionExecutor: fills a DataChunk with up to
// STANDARD_VECTOR_SIZE rows and evaluates one bound expression over the
// whole chunk. Amortizes the executor overhead RowExprEval pays per row
// (~4.6x on the filter path, see bench_planner_eval).
class BatchEvaluator {
public:
  BatchEvaluator(std::shared_ptr<PlanKeepAlive> keep_alive,
                 std::vector<const duckdb::Expression *> exprs,
                 duckdb::vector<duckdb::LogicalType> input_types)
      : keep_alive_(std::move(keep_alive)),
        context_(*keep_alive_->connection->context), exprs_(std::move(exprs)),
        input_types_(std::move(input_types)) {
    if (!input_types_.empty()) {
      chunk_.Initialize(duckdb::Allocator::Get(context_), input_types_);
    }
    for (const auto *expr : exprs_) {
      executors_.push_back(
          std::make_unique<duckdb::ExpressionExecutor>(context_, *expr));
      results_.emplace_back(expr->return_type);
    }
  }

  static constexpr duckdb::idx_t kBatch = STANDARD_VECTOR_SIZE;

  size_t expr_count() const { return exprs_.size(); }

  const duckdb::LogicalType &return_type(size_t e) const {
    return exprs_[e]->return_type;
  }

  // Fill the shared input chunk once per batch (count <= kBatch)
  void fill(const DuckDBRow *const *rows, duckdb::idx_t count) {
    chunk_.Reset();
    for (duckdb::idx_t c = 0; c < input_types_.size(); c++) {
      fill_column(chunk_.data[c], input_types_[c], rows, count, c);
    }
    chunk_.SetCardinality(count);
    count_ = count;
  }

  // Restrict the filled chunk to selected rows without refilling
  void slice(duckdb::SelectionVector &sel, duckdb::idx_t count) {
    chunk_.Slice(sel, count);
    count_ = count;
  }

  // Evaluate expression e over the current chunk; result is flattened and
  // valid until the next execute(e) call
  duckdb::Vector &execute(size_t e) {
    executors_[e]->ExecuteExpression(chunk_, results_[e]);
    results_[e].Flatten(count_);
    return results_[e];
  }

  // Read one entry of a flattened evaluation result as a Value, with typed
  // fast paths for common types (Vector::GetValue dispatches per call)
  static duckdb::Value read_result(duckdb::Vector &vec,
                                   const duckdb::LogicalType &type,
                                   duckdb::idx_t i) {
    if (!duckdb::FlatVector::Validity(vec).RowIsValid(i)) {
      return duckdb::Value(type);
    }
    switch (type.id()) {
    case duckdb::LogicalTypeId::BOOLEAN:
      return duckdb::Value::BOOLEAN(
          duckdb::FlatVector::GetData<bool>(vec)[i]);
    case duckdb::LogicalTypeId::INTEGER:
      return duckdb::Value::INTEGER(
          duckdb::FlatVector::GetData<int32_t>(vec)[i]);
    case duckdb::LogicalTypeId::BIGINT:
      return duckdb::Value::BIGINT(
          duckdb::FlatVector::GetData<int64_t>(vec)[i]);
    case duckdb::LogicalTypeId::FLOAT:
      return duckdb::Value::FLOAT(duckdb::FlatVector::GetData<float>(vec)[i]);
    case duckdb::LogicalTypeId::DOUBLE:
      return duckdb::Value::DOUBLE(
          duckdb::FlatVector::GetData<double>(vec)[i]);
    case duckdb::LogicalTypeId::VARCHAR:
      return duckdb::Value(
          duckdb::FlatVector::GetData<duckdb::string_t>(vec)[i].GetString());
    default:
      return vec.GetValue(i);
    }
  }

private:
  // Fill one chunk column from row values. Typed fast paths write vector
  // data directly; anything else falls back to per-value SetValue (which
  // handles casts). Per-cell Value boxing is what made the naive chunk fill
  // barely faster than the row-at-a-time path.
  static void fill_column(duckdb::Vector &vec, const duckdb::LogicalType &type,
                          const DuckDBRow *const *rows, duckdb::idx_t count,
                          duckdb::idx_t c) {
    auto &validity = duckdb::FlatVector::Validity(vec);
    validity.SetAllValid(count);

    auto value_at = [&](duckdb::idx_t i) -> const duckdb::Value * {
      const auto &cols = rows[i]->columns;
      return c < cols.size() ? &cols[c] : nullptr;
    };
    auto slow_cell = [&](duckdb::idx_t i, const duckdb::Value *v) {
      duckdb::Value cast = v ? *v : duckdb::Value(type);
      if (cast.type() != type) {
        cast = cast.DefaultCastAs(type);
      }
      vec.SetValue(i, cast);
    };

    switch (type.id()) {
    case duckdb::LogicalTypeId::BOOLEAN:
      fill_typed<bool>(vec, validity, type, count, value_at, slow_cell);
      break;
    case duckdb::LogicalTypeId::INTEGER:
      fill_typed<int32_t>(vec, validity, type, count, value_at, slow_cell);
      break;
    case duckdb::LogicalTypeId::BIGINT:
      fill_typed<int64_t>(vec, validity, type, count, value_at, slow_cell);
      break;
    case duckdb::LogicalTypeId::FLOAT:
      fill_typed<float>(vec, validity, type, count, value_at, slow_cell);
      break;
    case duckdb::LogicalTypeId::DOUBLE:
      fill_typed<double>(vec, validity, type, count, value_at, slow_cell);
      break;
    case duckdb::LogicalTypeId::VARCHAR: {
      auto data = duckdb::FlatVector::GetData<duckdb::string_t>(vec);
      for (duckdb::idx_t i = 0; i < count; i++) {
        const duckdb::Value *v = value_at(i);
        if (!v || v->IsNull()) {
          validity.SetInvalid(i);
        } else if (v->type().id() == duckdb::LogicalTypeId::VARCHAR) {
          data[i] = duckdb::StringVector::AddStringOrBlob(
              vec, duckdb::StringValue::Get(*v));
        } else {
          slow_cell(i, v);
        }
      }
      break;
    }
    default:
      for (duckdb::idx_t i = 0; i < count; i++) {
        slow_cell(i, value_at(i));
      }
      break;
    }
  }

  template <typename T, typename ValueAt, typename SlowCell>
  static void fill_typed(duckdb::Vector &vec, duckdb::ValidityMask &validity,
                         const duckdb::LogicalType &type, duckdb::idx_t count,
                         ValueAt &&value_at, SlowCell &&slow_cell) {
    auto data = duckdb::FlatVector::GetData<T>(vec);
    for (duckdb::idx_t i = 0; i < count; i++) {
      const duckdb::Value *v = value_at(i);
      if (!v || v->IsNull()) {
        validity.SetInvalid(i);
      } else if (v->type() == type) {
        data[i] = v->GetValueUnsafe<T>();
      } else {
        slow_cell(i, v);
      }
    }
  }

  // Declared first so it outlives the executors during destruction
  std::shared_ptr<PlanKeepAlive> keep_alive_;
  duckdb::ClientContext &context_;
  std::vector<const duckdb::Expression *> exprs_;
  duckdb::vector<duckdb::LogicalType> input_types_;
  std::vector<std::unique_ptr<duckdb::ExpressionExecutor>> executors_;
  std::vector<duckdb::Vector> results_;
  duckdb::DataChunk chunk_;
  duckdb::idx_t count_ = 0;
};

// One aggregate within an AGGREGATE spec (B2)
struct PlanAggSpec {
  enum class Fn {
    COUNT_STAR,
    COUNT,
    SUM,
    AVG,
    MIN,
    MAX,
    FIRST,
    STRING_AGG, // order-sensitive: requires ORDER BY inside the aggregate
    ARRAY_AGG,
    MEDIAN,        // quantile_cont 0.5
    QUANTILE_CONT, // interpolated
    QUANTILE_DISC, // lower-bound element
    MODE,          // most frequent (ties break by smallest value)
    MAD            // median absolute deviation (numeric args)
  };

  struct OrderKey {
    const duckdb::Expression *expr = nullptr;
    bool ascending = true;
    bool nulls_first = false;
  };

  Fn fn;
  const duckdb::Expression *arg = nullptr; // null for COUNT_STAR
  const duckdb::Expression *filter = nullptr; // FILTER (WHERE ...) clause
  std::vector<OrderKey> order_keys; // STRING_AGG/ARRAY_AGG
  std::string separator = ",";      // STRING_AGG
  double quantile = 0.5;            // QUANTILE_CONT/DISC
  bool distinct = false;                   // COUNT/SUM/AVG(DISTINCT x)
  bool integer_arg = false;                // SUM/AVG: int64 vs double sum
  bool decimal_arg = false; // SUM over DECIMAL: exact unscaled hugeint sum
  uint8_t decimal_scale = 0;
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
    SORT_LIMIT,  // NativeSortView/NativeLimitView in an EmbeddedViewNode
    REC_CTE,     // WITH RECURSIVE: children = {anchor, recursive step}
    DISTINCT_ON, // NativeDistinctOnView in an EmbeddedViewNode
    FILTER_MAP,  // fused filter+project (IR optimizer, exprs = projection)
    DELIM_JOIN,  // correlated subquery: children = {outer, subplan};
                 // exprs = duplicate-eliminated (correlated) columns
    DELIM_REF    // DELIM_GET: reads the shared distinct-correlated-keys
                 // output of the enclosing DELIM_JOIN
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
  duckdb::JoinType join_type = duckdb::JoinType::INNER; // JOIN
  bool null_safe_keys = false; // JOIN/DELIM: IS NOT DISTINCT FROM keys
  std::vector<NativeWindowView::WindowDef> window_defs;   // WINDOW
  std::vector<ColumnInfo> window_source_cols;             // WINDOW
  std::vector<ColumnInfo> window_result_cols;             // WINDOW
  duckdb::idx_t cte_index = 0;                   // CTE / CTE_REF

  // DISTINCT_ON: column_idxs = partition keys; sort_columns = winner-pick
  // order from the DISTINCT node's own order_by modifier (not the
  // presentation ORDER_BY above it). Output keeps the full child row layout.
  //
  // SORT_LIMIT: ORDER BY / LIMIT / OFFSET folded into one embedded view.
  // project_idxs is a trailing pure-column-ref projection folded in so sort
  // keys dropped from the output still order it. presentation_root marks the
  // plan root: only then does the view's ordered scan drive dbsp_query.
  std::vector<NativeSortView::SortColumn> sort_columns;
  int64_t limit = -1;        // -1 = no limit
  double limit_percent = -1; // LIMIT p PERCENT (>= 0 wins over limit)
  int64_t offset = 0;
  std::vector<duckdb::idx_t> project_idxs; // empty = identity
  bool presentation_root = false;

  // FILTER_MAP: predicates evaluated before the projection in `exprs`.
  // Pointers reference either the bound plan or PlanKeepAlive::rewritten_exprs
  std::vector<const duckdb::Expression *> filter_exprs;
};

// ===== Circuit-IR optimizer (Phase C4) =====
//
// Rewrites the translated PlanOpSpec tree before circuit construction —
// the successor of the retired ParsedViewDef-based DBSPOptimizer. Passes:
//   1. combine_filters:  FILTER(FILTER(x))      -> one FILTER (AND list)
//   2. pushdown_filters: FILTER above JOIN      -> per-side FILTER below it
//                        (shrinks join index state)
//   3. fuse_filter_map:  MAP(FILTER(x))         -> one FILTER_MAP node
// Projection pruning is deliberately NOT ported: DuckDB's binder already
// prunes via GET column_ids, so canonical plans have nothing left to prune.
inline std::atomic<bool> g_plan_ir_optimize{true};

// L2: intra-operator sharding. When > 1, residual-free inner equi-join
// probe passes over large deltas split across this many threads (probes
// are read-only; each shard emits into its own Z-set, merged after).
// Set by CDCManager::set_parallel_sync — one knob with view-level
// parallelism.
inline std::atomic<int> g_intraop_shards{0};

namespace plan_ir {

inline void collect_bound_refs(const duckdb::Expression &expr,
                               std::vector<duckdb::idx_t> &out) {
  if (expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_REF) {
    out.push_back(expr.Cast<duckdb::BoundReferenceExpression>().index);
  }
  duckdb::ExpressionIterator::EnumerateChildren(
      expr, [&](const duckdb::Expression &child) {
        collect_bound_refs(child, out);
      });
}

inline void shift_bound_refs(duckdb::Expression &expr, duckdb::idx_t delta) {
  if (expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_REF) {
    expr.Cast<duckdb::BoundReferenceExpression>().index -= delta;
  }
  duckdb::ExpressionIterator::EnumerateChildren(
      expr,
      [&](duckdb::Expression &child) { shift_bound_refs(child, delta); });
}

// FILTER(FILTER(x)) -> FILTER(x) with concatenated AND lists
inline void combine_filters(std::unique_ptr<PlanOpSpec> &spec) {
  while (spec->kind == PlanOpSpec::Kind::FILTER_EXPR &&
         spec->children[0]->kind == PlanOpSpec::Kind::FILTER_EXPR) {
    auto &child = spec->children[0];
    spec->exprs.insert(spec->exprs.end(), child->exprs.begin(),
                       child->exprs.end());
    // Filters preserve schema: the grandchild's output types are the same
    spec->input_types = child->input_types;
    auto grandchild = std::move(child->children[0]);
    spec->children[0] = std::move(grandchild);
  }
}

// FILTER above JOIN: move single-side predicates below the join, shrinking
// the join's per-side index state. Right-side predicates need their column
// indices shifted; the copies live in keep_alive->rewritten_exprs.
inline void pushdown_filters(std::unique_ptr<PlanOpSpec> &spec,
                             PlanKeepAlive &keep_alive) {
  if (spec->kind != PlanOpSpec::Kind::FILTER_EXPR ||
      spec->children[0]->kind != PlanOpSpec::Kind::JOIN) {
    return;
  }
  auto &join = spec->children[0];
  const duckdb::idx_t left_width = join->left_types.size();

  std::vector<const duckdb::Expression *> keep;
  std::vector<const duckdb::Expression *> left_push;
  std::vector<const duckdb::Expression *> right_push;
  for (const auto *expr : spec->exprs) {
    std::vector<duckdb::idx_t> refs;
    collect_bound_refs(*expr, refs);
    bool all_left = true, all_right = true;
    for (auto idx : refs) {
      (idx < left_width ? all_right : all_left) = false;
    }
    if (!refs.empty() && all_left) {
      left_push.push_back(expr); // left indices are already 0-based
    } else if (!refs.empty() && all_right) {
      auto copy = expr->Copy();
      shift_bound_refs(*copy, left_width);
      right_push.push_back(copy.get());
      keep_alive.rewritten_exprs.push_back(std::move(copy));
    } else {
      keep.push_back(expr);
    }
  }
  if (left_push.empty() && right_push.empty()) {
    return;
  }

  auto make_side_filter = [](std::unique_ptr<PlanOpSpec> child,
                             duckdb::vector<duckdb::LogicalType> types,
                             std::vector<const duckdb::Expression *> exprs) {
    auto f = std::make_unique<PlanOpSpec>();
    f->kind = PlanOpSpec::Kind::FILTER_EXPR;
    f->input_types = std::move(types);
    f->exprs = std::move(exprs);
    f->children.push_back(std::move(child));
    return f;
  };
  if (!left_push.empty()) {
    join->children[0] = make_side_filter(
        std::move(join->children[0]), join->left_types, std::move(left_push));
  }
  if (!right_push.empty()) {
    join->children[1] =
        make_side_filter(std::move(join->children[1]), join->right_types,
                         std::move(right_push));
  }

  if (keep.empty()) {
    spec = std::move(spec->children[0]); // filter fully absorbed
  } else {
    spec->exprs = std::move(keep);
  }
}

// MAP(FILTER(x)) -> FILTER_MAP(x): one node, no intermediate Z-set
inline void fuse_filter_map(std::unique_ptr<PlanOpSpec> &spec) {
  if (spec->kind != PlanOpSpec::Kind::MAP_EXPR ||
      spec->children[0]->kind != PlanOpSpec::Kind::FILTER_EXPR) {
    return;
  }
  auto &filter = spec->children[0];
  spec->kind = PlanOpSpec::Kind::FILTER_MAP;
  spec->filter_exprs = std::move(filter->exprs);
  // MAP's input == FILTER's output == FILTER's input (schema-preserving)
  if (!filter->input_types.empty()) {
    spec->input_types = filter->input_types;
  }
  auto grandchild = std::move(filter->children[0]);
  spec->children[0] = std::move(grandchild);
}

inline void optimize(std::unique_ptr<PlanOpSpec> &spec,
                     PlanKeepAlive &keep_alive) {
  combine_filters(spec);
  pushdown_filters(spec, keep_alive);
  fuse_filter_map(spec);
  for (auto &child : spec->children) {
    optimize(child, keep_alive);
  }
}

} // namespace plan_ir

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
    int arg_idx = -1;    // index into the batch evaluator; -1 for COUNT_STAR
    int filter_idx = -1; // FILTER predicate slot in the batch evaluator
    bool distinct = false;
    bool integer_arg;
    bool decimal_arg = false;
    uint8_t decimal_scale = 0;
    duckdb::LogicalType return_type;
    // Order-sensitive aggregates: batch slots of the ORDER BY keys +
    // their direction/null placement; STRING_AGG separator
    std::vector<int> order_idxs;
    std::vector<std::pair<bool, bool>> order_dirs; // (ascending, nulls_first)
    std::string separator;
    double quantile = 0.5;
  };

  // exprs layout in the shared batch evaluator: group keys first, then
  // aggregate arguments (H4: keys/args evaluate per 2048-row chunk instead
  // of per row through a 1-row executor)
  PlanAggregateNode(dbsp::NodeId id, InputFn input_fn,
                    std::shared_ptr<PlanKeepAlive> keep_alive,
                    std::vector<const duckdb::Expression *> group_exprs,
                    std::vector<const duckdb::Expression *> arg_exprs,
                    std::vector<AggInstance> aggs,
                    duckdb::vector<duckdb::LogicalType> input_types,
                    std::string name = "plan_aggregate")
      : dbsp::Node(id, std::move(name)), input_fn_(std::move(input_fn)),
        num_groups_(group_exprs.size()), aggs_(std::move(aggs)) {
    for (const auto *e : arg_exprs) {
      group_exprs.push_back(e);
    }
    if (!group_exprs.empty()) {
      eval_ = std::make_unique<BatchEvaluator>(
          std::move(keep_alive), std::move(group_exprs),
          std::move(input_types));
    }
  }

  void step() override {
    output_.clear();
    has_output_ = false;
    const DuckDBZSet &changes = input_fn_();
    bool global = num_groups_ == 0;

    if (changes.empty()) {
      if (global && !global_emitted_) {
        output_.insert(result_row(DuckDBRow{}, states_[DuckDBRow{}]), 1);
        global_emitted_ = true;
        has_output_ = true;
      }
      return;
    }

    // Batch-evaluate group keys and aggregate args, then bucket the
    // pre-evaluated (args, weight) pairs by key
    using Contribution = std::pair<std::vector<duckdb::Value>, int64_t>;
    std::unordered_map<DuckDBRow, std::vector<Contribution>, DuckDBRowHash>
        buckets;

    std::vector<const DuckDBRow *> rows;
    std::vector<int64_t> weights;
    rows.reserve(BatchEvaluator::kBatch);
    weights.reserve(BatchEvaluator::kBatch);
    const size_t num_args = eval_ ? eval_->expr_count() - num_groups_ : 0;

    auto flush = [&]() {
      if (rows.empty()) {
        return;
      }
      const duckdb::idx_t n = rows.size();
      std::vector<DuckDBRow> keys(n);
      if (eval_) {
        eval_->fill(rows.data(), n);
        std::vector<std::vector<duckdb::Value>> key_vals(n);
        for (auto &kv : key_vals) {
          kv.reserve(num_groups_);
        }
        for (size_t g = 0; g < num_groups_; g++) {
          duckdb::Vector &v = eval_->execute(g);
          const auto &type = eval_->return_type(g);
          for (duckdb::idx_t i = 0; i < n; i++) {
            key_vals[i].push_back(BatchEvaluator::read_result(v, type, i));
          }
        }
        for (duckdb::idx_t i = 0; i < n; i++) {
          keys[i].columns.assign(std::move(key_vals[i]));
        }
        std::vector<std::vector<duckdb::Value>> args(n);
        for (size_t a = 0; a < num_args; a++) {
          const size_t e = num_groups_ + a;
          duckdb::Vector &v = eval_->execute(e);
          const auto &type = eval_->return_type(e);
          for (duckdb::idx_t i = 0; i < n; i++) {
            args[i].push_back(BatchEvaluator::read_result(v, type, i));
          }
        }
        for (duckdb::idx_t i = 0; i < n; i++) {
          buckets[std::move(keys[i])].emplace_back(std::move(args[i]),
                                                   weights[i]);
        }
      } else {
        for (duckdb::idx_t i = 0; i < n; i++) {
          buckets[DuckDBRow{}].emplace_back(std::vector<duckdb::Value>{},
                                            weights[i]);
        }
      }
      rows.clear();
      weights.clear();
    };
    for (const auto &[row, weight] : changes) {
      rows.push_back(&row);
      weights.push_back(weight);
      if (rows.size() == BatchEvaluator::kBatch) {
        flush();
      }
    }
    flush();

    for (auto &[key, contributions] : buckets) {
      auto &state = states_[key];
      bool had_row = global ? global_emitted_ : state.row_weight > 0;
      if (had_row) {
        output_.insert(result_row(key, state), -1);
      }
      for (const auto &[args, weight] : contributions) {
        apply(state, args, weight);
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
    duckdb::hugeint_t hsum = 0; // DECIMAL SUM, unscaled
    std::multiset<duckdb::Value> values; // MIN/MAX
    // DISTINCT: per-value weights; contributions fire on presence
    // transitions (0→>0 adds the value once, >0→0 retracts it)
    std::map<duckdb::Value, int64_t> dvals;
    // Order-sensitive aggregates: (order keys, value) kept sorted; the
    // whole aggregate re-renders from this on every group change
    std::vector<std::pair<std::vector<duckdb::Value>, duckdb::Value>>
        ordered;
    // MODE: per-value multiplicities (values multiset serves
    // MEDIAN/QUANTILE the same way it serves MIN/MAX)
    std::map<duckdb::Value, int64_t> mode_counts;
  };

  struct GroupState {
    int64_t row_weight = 0; // total weight of rows in the group
    std::vector<AggState> aggs;
  };

  void apply(GroupState &state, const std::vector<duckdb::Value> &args,
             int64_t weight) {
    state.row_weight += weight;
    state.aggs.resize(aggs_.size());
    for (size_t i = 0; i < aggs_.size(); i++) {
      auto &spec = aggs_[i];
      auto &s = state.aggs[i];
      if (spec.filter_idx >= 0) {
        // FILTER (WHERE p): rows failing p contribute nothing to THIS
        // aggregate. Applies symmetrically to inserts and deletes, so
        // incremental maintenance is unchanged.
        const duckdb::Value &f = args[static_cast<size_t>(spec.filter_idx)];
        if (f.IsNull() || !f.GetValue<bool>()) {
          continue;
        }
      }
      if (spec.fn == PlanAggSpec::Fn::COUNT_STAR) {
        s.count += weight;
        continue;
      }
      const duckdb::Value &v = args[static_cast<size_t>(spec.arg_idx)];
      if (spec.fn == PlanAggSpec::Fn::STRING_AGG ||
          spec.fn == PlanAggSpec::Fn::ARRAY_AGG) {
        // string_agg skips NULLs; array_agg keeps them (DuckDB semantics)
        if (v.IsNull() && spec.fn == PlanAggSpec::Fn::STRING_AGG) {
          continue;
        }
        std::vector<duckdb::Value> okeys;
        okeys.reserve(spec.order_idxs.size());
        for (int oi : spec.order_idxs) {
          okeys.push_back(args[static_cast<size_t>(oi)]);
        }
        std::pair<std::vector<duckdb::Value>, duckdb::Value> entry{
            std::move(okeys), v};
        auto cmp = [&spec, this](const decltype(entry) &a,
                                 const decltype(entry) &b) {
          return ordered_less(spec, a, b);
        };
        if (weight > 0) {
          for (int64_t w = 0; w < weight; w++) {
            auto it = std::upper_bound(s.ordered.begin(), s.ordered.end(),
                                       entry, cmp);
            s.ordered.insert(it, entry);
          }
        } else {
          for (int64_t w = 0; w < -weight; w++) {
            auto range = std::equal_range(s.ordered.begin(),
                                          s.ordered.end(), entry, cmp);
            // Erase one exact match (order keys AND value equal)
            for (auto it = range.first; it != range.second; ++it) {
              if (it->second.IsNull() == entry.second.IsNull() &&
                  (it->second.IsNull() || !(it->second < entry.second) &&
                                              !(entry.second < it->second))) {
                s.ordered.erase(it);
                break;
              }
            }
          }
        }
        continue;
      }
      if (v.IsNull()) {
        continue; // SQL: NULL arguments are ignored
      }
      if (spec.distinct) {
        // Presence transition drives COUNT/SUM/AVG; MIN/MAX fall through
        // (duplicates never change an extreme)
        int64_t &dw = s.dvals[v];
        const bool was = dw > 0;
        dw += weight;
        const bool is = dw > 0;
        if (dw == 0) {
          s.dvals.erase(v);
        }
        if (spec.fn != PlanAggSpec::Fn::MIN &&
            spec.fn != PlanAggSpec::Fn::MAX) {
          const int64_t d = (is ? 1 : 0) - (was ? 1 : 0);
          if (d == 0) {
            continue;
          }
          s.count += d;
          if (spec.fn == PlanAggSpec::Fn::SUM ||
              spec.fn == PlanAggSpec::Fn::AVG) {
            if (spec.decimal_arg) {
              duckdb::Value wide = v.DefaultCastAs(
                  duckdb::LogicalType::DECIMAL(38, spec.decimal_scale));
              s.hsum += wide.GetValueUnsafe<duckdb::hugeint_t>() *
                        duckdb::hugeint_t(d);
            } else if (spec.integer_arg) {
              s.isum += v.GetValue<int64_t>() * d;
            } else {
              s.dsum += v.GetValue<double>() * d;
            }
          }
          continue;
        }
      }
      s.count += weight;
      switch (spec.fn) {
      case PlanAggSpec::Fn::COUNT:
        break;
      case PlanAggSpec::Fn::SUM:
      case PlanAggSpec::Fn::AVG:
        if (spec.decimal_arg) {
          // Exact: sum the unscaled decimal representation in 128 bits
          duckdb::Value wide = v.DefaultCastAs(
              duckdb::LogicalType::DECIMAL(38, spec.decimal_scale));
          s.hsum += wide.GetValueUnsafe<duckdb::hugeint_t>() *
                    duckdb::hugeint_t(weight);
        } else if (spec.integer_arg) {
          s.isum += v.GetValue<int64_t>() * weight;
        } else {
          s.dsum += v.GetValue<double>() * weight;
        }
        break;
      case PlanAggSpec::Fn::MIN:
      case PlanAggSpec::Fn::MAX:
      case PlanAggSpec::Fn::FIRST:
      case PlanAggSpec::Fn::MEDIAN:
      case PlanAggSpec::Fn::QUANTILE_CONT:
      case PlanAggSpec::Fn::QUANTILE_DISC:
      case PlanAggSpec::Fn::MAD:
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
      case PlanAggSpec::Fn::MODE: {
        int64_t &c = s.mode_counts[v];
        c += weight;
        if (c <= 0) {
          s.mode_counts.erase(v);
        }
        break;
      }
      default:
        break;
      }
    }
  }

  // Sort order for order-sensitive aggregate entries: the declared ORDER
  // BY keys (direction + null placement per key), then the value itself
  // as a deterministic tiebreak. duckdb::Value::operator< cannot compare
  // NULLs, so NULL handling comes first at every step.
  static bool value_less(const duckdb::Value &a, const duckdb::Value &b,
                         bool ascending, bool nulls_first) {
    const bool an = a.IsNull(), bn = b.IsNull();
    if (an || bn) {
      if (an && bn) {
        return false;
      }
      return an ? nulls_first : !nulls_first;
    }
    return ascending ? a < b : b < a;
  }

  bool ordered_less(
      const AggInstance &spec,
      const std::pair<std::vector<duckdb::Value>, duckdb::Value> &a,
      const std::pair<std::vector<duckdb::Value>, duckdb::Value> &b) const {
    for (size_t k = 0; k < spec.order_dirs.size(); k++) {
      const auto &[asc, nf] = spec.order_dirs[k];
      if (value_less(a.first[k], b.first[k], asc, nf)) {
        return true;
      }
      if (value_less(b.first[k], a.first[k], asc, nf)) {
        return false;
      }
    }
    // Tiebreak on the value (ascending, NULLs last) for determinism
    return value_less(a.second, b.second, true, false);
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
      if (spec.decimal_arg) {
        return duckdb::Value::DECIMAL(s.hsum, 38, spec.decimal_scale)
            .DefaultCastAs(spec.return_type);
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
    case PlanAggSpec::Fn::FIRST:
      return s.values.empty() ? duckdb::Value(spec.return_type)
                              : *s.values.begin();
    case PlanAggSpec::Fn::MAX:
      return s.values.empty() ? duckdb::Value(spec.return_type)
                              : *s.values.rbegin();
    case PlanAggSpec::Fn::STRING_AGG: {
      if (s.ordered.empty()) {
        return duckdb::Value(spec.return_type);
      }
      std::string out;
      bool first = true;
      for (const auto &[keys, v] : s.ordered) {
        if (!first) {
          out += spec.separator;
        }
        out += v.DefaultCastAs(duckdb::LogicalType::VARCHAR)
                   .GetValue<std::string>();
        first = false;
      }
      return duckdb::Value(out);
    }
    case PlanAggSpec::Fn::MEDIAN:
    case PlanAggSpec::Fn::QUANTILE_CONT: {
      if (s.values.empty()) {
        return duckdb::Value(spec.return_type);
      }
      // Interpolated quantile over the sorted multiset: position
      // q*(n-1) between neighbors lo and hi
      const double q =
          spec.fn == PlanAggSpec::Fn::MEDIAN ? 0.5 : spec.quantile;
      const size_t n = s.values.size();
      const double pos = q * static_cast<double>(n - 1);
      const size_t lo_idx = static_cast<size_t>(pos);
      const double frac = pos - static_cast<double>(lo_idx);
      auto it = s.values.begin();
      std::advance(it, lo_idx);
      const duckdb::Value lo = *it;
      if (frac == 0.0 || lo_idx + 1 >= n) {
        return lo.DefaultCastAs(spec.return_type);
      }
      const duckdb::Value hi = *std::next(it);
      const double lod = lo.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                             .GetValue<double>();
      const double hid = hi.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                             .GetValue<double>();
      return duckdb::Value(lod + frac * (hid - lod))
          .DefaultCastAs(spec.return_type);
    }
    case PlanAggSpec::Fn::QUANTILE_DISC: {
      if (s.values.empty()) {
        return duckdb::Value(spec.return_type);
      }
      // Discrete quantile: element at ceil(q*n)-1 (DuckDB semantics)
      const size_t n = s.values.size();
      size_t idx = static_cast<size_t>(
          std::ceil(spec.quantile * static_cast<double>(n)));
      idx = idx > 0 ? idx - 1 : 0;
      if (idx >= n) {
        idx = n - 1;
      }
      auto it = s.values.begin();
      std::advance(it, idx);
      return it->DefaultCastAs(spec.return_type);
    }
    case PlanAggSpec::Fn::MAD: {
      if (s.values.empty()) {
        return duckdb::Value(spec.return_type);
      }
      // Median absolute deviation: median(|x - median(x)|), both medians
      // interpolated (DuckDB semantics). Deviations come out sorted by
      // merging the two halves around the median.
      std::vector<double> vals;
      vals.reserve(s.values.size());
      for (const auto &v : s.values) {
        vals.push_back(v.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                           .GetValue<double>());
      }
      auto interp = [](const std::vector<double> &sorted) {
        const double pos = 0.5 * static_cast<double>(sorted.size() - 1);
        const size_t lo = static_cast<size_t>(pos);
        const double frac = pos - static_cast<double>(lo);
        if (frac == 0.0 || lo + 1 >= sorted.size()) {
          return sorted[lo];
        }
        return sorted[lo] + frac * (sorted[lo + 1] - sorted[lo]);
      };
      const double med = interp(vals);
      std::vector<double> devs;
      devs.reserve(vals.size());
      size_t below = 0;
      while (below < vals.size() && vals[below] <= med) {
        below++;
      }
      size_t l = below, r = below; // l walks down, r walks up
      while (devs.size() < vals.size()) {
        const double dl =
            l > 0 ? med - vals[l - 1] : std::numeric_limits<double>::max();
        const double dr = r < vals.size()
                              ? vals[r] - med
                              : std::numeric_limits<double>::max();
        if (dl <= dr) {
          devs.push_back(dl);
          l--;
        } else {
          devs.push_back(dr);
          r++;
        }
      }
      return duckdb::Value(interp(devs)).DefaultCastAs(spec.return_type);
    }
    case PlanAggSpec::Fn::MODE: {
      if (s.mode_counts.empty()) {
        return duckdb::Value(spec.return_type);
      }
      // Highest multiplicity; ties break by smallest value (std::map is
      // value-ordered, first hit wins) — DuckDB's tie choice is
      // scan-order-dependent and unreproducible incrementally
      const duckdb::Value *best = nullptr;
      int64_t best_count = 0;
      for (const auto &[v, c] : s.mode_counts) {
        if (c > best_count) {
          best = &v;
          best_count = c;
        }
      }
      return best->DefaultCastAs(spec.return_type);
    }
    case PlanAggSpec::Fn::ARRAY_AGG: {
      if (s.ordered.empty()) {
        return duckdb::Value(spec.return_type);
      }
      duckdb::vector<duckdb::Value> vals;
      vals.reserve(s.ordered.size());
      for (const auto &[keys, v] : s.ordered) {
        vals.push_back(v);
      }
      return duckdb::Value::LIST(
          duckdb::ListType::GetChildType(spec.return_type),
          std::move(vals));
    }
    }
    return duckdb::Value(spec.return_type);
  }

  DuckDBRow result_row(const DuckDBRow &key, const GroupState &state) const {
    std::vector<duckdb::Value> vals;
    vals.reserve(key.columns.size() + aggs_.size());
    vals.insert(vals.end(), key.columns.begin(), key.columns.end());
    for (size_t i = 0; i < aggs_.size(); i++) {
      static const AggState kEmpty;
      const AggState &s = i < state.aggs.size() ? state.aggs[i] : kEmpty;
      vals.push_back(agg_value(aggs_[i], s));
    }
    DuckDBRow result;
    result.columns.assign(std::move(vals));
    return result;
  }

  InputFn input_fn_;
  size_t num_groups_;
  std::unique_ptr<BatchEvaluator> eval_;
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
// Shared join arrangement (I1): one table-side equi-key index maintained
// ONCE per table delta by the CDC layer and consumed read-only by every
// join that registered for the same (table, key expressions) fingerprint.
// Updated at the START of propagation, so consuming joins see the NEW
// state for this side; their bilinear formula drops the Δl⋈Δr term
// accordingly (v1 shares at most one side per join, which keeps view
// initialization = plain replay of the local side).
struct SharedArrangement {
  using RowWeights = std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash>;
  using Index = std::unordered_map<DuckDBRow, RowWeights, DuckDBRowHash>;

  std::string table;
  bool null_safe = false;
  bool track_weights = false; // per-row totals (outer pads / marks)
  bool track_counters = false; // total + null-key weights (marks)
  // Column projection applied to raw table rows before indexing — join
  // sides are usually MAP_COLS(SOURCE), so the arrangement stores rows in
  // the shape the join consumes
  bool project = false;
  std::vector<duckdb::idx_t> column_idxs;

  Index index;
  RowWeights weights;
  int64_t total = 0;
  int64_t nulls = 0;

  // K2: index spilled to a disk bucket log (dbsp_spill). Probes go
  // through probe_spilled() with an internal mutex — concurrent
  // same-level views (I2) may probe simultaneously. Weights/counters
  // stay in RAM (shared sides never self-pad, so they are empty/tiny).
  std::unique_ptr<SpilledBucketIndex> spilled;
  mutable std::mutex spill_probe_mutex;

  bool is_spilled() const { return spilled != nullptr; }

  static RowDigest digest_of_row(const DuckDBRow &row) {
    std::vector<duckdb::Value> vals;
    vals.reserve(row.columns.size());
    for (size_t i = 0; i < row.columns.size(); i++) {
      vals.push_back(row.columns[i]);
    }
    std::vector<uint8_t> bytes;
    serialize_row(vals, bytes);
    return digest_bytes(bytes.data(), bytes.size());
  }

  // Fill `out` with the bucket for `key`; false when absent
  bool probe_spilled(const DuckDBRow &key, RowWeights &out) const {
    out.clear();
    std::lock_guard<std::mutex> guard(spill_probe_mutex);
    const auto *bucket = spilled->probe(digest_of_row(key));
    if (!bucket) {
      return false;
    }
    for (const auto &[vals, w] : *bucket) {
      DuckDBRow row;
      std::vector<duckdb::Value> copy = vals;
      row.columns.assign(std::move(copy));
      out.emplace(std::move(row), w);
    }
    return true;
  }

  void enable_spill(const std::string &path) {
    if (spilled) {
      return;
    }
    spilled = std::make_unique<SpilledBucketIndex>(path);
    for (const auto &[key, bucket] : index) {
      SpilledBucketIndex::Bucket delta;
      delta.reserve(bucket.size());
      for (const auto &[row, w] : bucket) {
        std::vector<duckdb::Value> vals;
        vals.reserve(row.columns.size());
        for (size_t i = 0; i < row.columns.size(); i++) {
          vals.push_back(row.columns[i]);
        }
        delta.emplace_back(std::move(vals), w);
      }
      spilled->update(digest_of_row(key), delta);
    }
    index.clear();
  }

  void disable_spill() {
    if (!spilled) {
      return;
    }
    spilled->scan([&](const SpilledBucketIndex::Bucket &bucket) {
      for (const auto &[vals, w] : bucket) {
        DuckDBRow row;
        std::vector<duckdb::Value> copy = vals;
        row.columns.assign(std::move(copy));
        DuckDBRow key;
        if (eval_key(row, key)) {
          index[key][row] += w;
        }
      }
    });
    spilled.reset();
  }

  // Key evaluation owned by the arrangement (first registrant's bound
  // expressions; keep_alive pins that plan for the arrangement's lifetime)
  std::shared_ptr<PlanKeepAlive> keep_alive;
  std::unique_ptr<BatchEvaluator> key_eval;
  std::vector<std::unique_ptr<RowExprEval>> row_key_evals; // single-row

  bool eval_key(const DuckDBRow &row, DuckDBRow &key) const {
    std::vector<duckdb::Value> vals;
    vals.reserve(row_key_evals.size());
    for (const auto &k : row_key_evals) {
      duckdb::Value v = k->eval(row);
      if (v.IsNull() && !null_safe) {
        return false;
      }
      vals.push_back(std::move(v));
    }
    key.columns.assign(std::move(vals));
    return true;
  }

  void apply(const DuckDBZSet &delta) {
    // Project raw table rows into the join side's shape first (mirrors
    // the view's own MAP_COLS node), then batched key extraction and the
    // same integration the join used
    std::vector<DuckDBRow> projected;
    std::vector<const DuckDBRow *> rows;
    std::vector<int64_t> ws;
    rows.reserve(delta.size());
    ws.reserve(delta.size());
    if (project) {
      projected.reserve(delta.size());
      for (const auto &[row, w] : delta) {
        DuckDBRow out;
        std::vector<duckdb::Value> vals;
        vals.reserve(column_idxs.size());
        for (auto idx : column_idxs) {
          vals.push_back(idx < row.columns.size() ? row.columns[idx]
                                                  : duckdb::Value());
        }
        out.columns.assign(std::move(vals));
        projected.push_back(std::move(out));
        ws.push_back(w);
      }
      for (const auto &r : projected) {
        rows.push_back(&r);
      }
    } else {
      for (const auto &[row, w] : delta) {
        rows.push_back(&row);
        ws.push_back(w);
      }
    }
    std::vector<DuckDBRow> keys(rows.size());
    std::vector<char> valid(rows.size(), 1);
    if (key_eval && key_eval->expr_count() > 0) {
      size_t base = 0;
      while (base < rows.size()) {
        const duckdb::idx_t chunk = static_cast<duckdb::idx_t>(
            std::min<size_t>(BatchEvaluator::kBatch, rows.size() - base));
        key_eval->fill(rows.data() + base, chunk);
        std::vector<std::vector<duckdb::Value>> kv(chunk);
        for (size_t k = 0; k < key_eval->expr_count(); k++) {
          duckdb::Vector &v = key_eval->execute(k);
          const auto &type = key_eval->return_type(k);
          for (duckdb::idx_t i = 0; i < chunk; i++) {
            duckdb::Value val = BatchEvaluator::read_result(v, type, i);
            if (val.IsNull() && !null_safe) {
              valid[base + i] = 0;
            }
            kv[i].push_back(std::move(val));
          }
        }
        for (duckdb::idx_t i = 0; i < chunk; i++) {
          keys[base + i].columns.assign(std::move(kv[i]));
        }
        base += chunk;
      }
    }
    // Spilled mode: group contributions per key first, one disk-bucket
    // merge per touched key (updates run pre-views, single-threaded)
    std::unordered_map<RowDigest, SpilledBucketIndex::Bucket, RowDigestHash>
        spill_batch;
    for (size_t i = 0; i < rows.size(); i++) {
      const DuckDBRow &row = *rows[i];
      const int64_t w = ws[i];
      if (track_weights) {
        int64_t &t = weights[row];
        t += w;
        if (t == 0) {
          weights.erase(row);
        }
      }
      if (track_counters) {
        total += w;
        if (!valid[i]) {
          nulls += w;
        }
      }
      if (!valid[i]) {
        continue;
      }
      if (spilled) {
        std::vector<duckdb::Value> vals;
        vals.reserve(row.columns.size());
        for (size_t c = 0; c < row.columns.size(); c++) {
          vals.push_back(row.columns[c]);
        }
        spill_batch[digest_of_row(keys[i])].emplace_back(std::move(vals), w);
        continue;
      }
      auto &bucket = index[keys[i]];
      int64_t &weight = bucket[row];
      weight += w;
      if (weight == 0) {
        bucket.erase(row);
        if (bucket.empty()) {
          index.erase(keys[i]);
        }
      }
    }
    for (auto &[digest, delta_bucket] : spill_batch) {
      spilled->update(digest, delta_bucket);
    }
  }
};

// A join side's request to consume a shared arrangement; collected during
// circuit construction, resolved by CDCManager after sources are tracked
struct ArrangementRequest {
  std::string fingerprint;
  std::string table;
  bool left_side = false;
  bool null_safe = false;
  bool track_weights = false;
  bool track_counters = false;
  std::vector<const duckdb::Expression *> key_exprs;
  duckdb::vector<duckdb::LogicalType> side_types;
  bool project = false; // side is MAP_COLS(SOURCE): project before indexing
  std::vector<duckdb::idx_t> column_idxs;
  // Skip this table's init replay (the arrangement already holds full
  // state). For a both-sides-shared join only ONE side is skipped: the
  // other side's replay ⋈ this arrangement bootstraps the full join.
  bool init_skip = true;
  std::shared_ptr<PlanKeepAlive> keep_alive; // pins exprs for the arrangement
  class PlanJoinNode *node = nullptr;
};

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
               duckdb::JoinType join_type = duckdb::JoinType::INNER,
               duckdb::vector<duckdb::LogicalType> left_types = {},
               duckdb::vector<duckdb::LogicalType> right_types = {},
               bool null_safe_keys = false,
               std::shared_ptr<PlanKeepAlive> keep_alive = nullptr,
               std::vector<const duckdb::Expression *> left_key_exprs = {},
               std::vector<const duckdb::Expression *> right_key_exprs = {},
               std::string name = "plan_join")
      : dbsp::Node(id, std::move(name)), left_fn_(std::move(left_fn)),
        right_fn_(std::move(right_fn)), keys_(std::move(keys)),
        residuals_(std::move(residuals)), join_type_(join_type),
        left_types_(std::move(left_types)),
        right_types_(std::move(right_types)),
        null_safe_keys_(null_safe_keys) {
    // H4: whole-delta key extraction runs batched; the per-row KeyPair
    // evals stay for point lookups (match counts, pads, marks)
    if (keep_alive && !left_key_exprs.empty()) {
      batch_left_keys_ = std::make_unique<BatchEvaluator>(
          keep_alive, std::move(left_key_exprs), left_types_);
      batch_right_keys_ = std::make_unique<BatchEvaluator>(
          keep_alive, std::move(right_key_exprs), right_types_);
    }
    pads_left_ = join_type_ == duckdb::JoinType::LEFT ||
                 join_type_ == duckdb::JoinType::OUTER;
    pads_right_ = join_type_ == duckdb::JoinType::RIGHT ||
                  join_type_ == duckdb::JoinType::OUTER;
    marks_ = join_type_ == duckdb::JoinType::MARK;
  }

  void step() override {
    output_.clear();
    has_output_ = false;
    const DuckDBZSet &dl = left_fn_();
    const DuckDBZSet &dr = right_fn_();
    if (dl.empty() && dr.empty()) {
      return;
    }

    if (marks_) {
      // MARK join is left-preserving: every left row appears exactly once
      // with a three-valued match mark — no bilinear emission at all
      // Category detection needs the BEFORE counters. A shared right side
      // is already post-delta (updated before views step): derive before =
      // after − this step's contribution. A local right side is pre-delta:
      // read, then integrate.
      bool was_nonempty, had_nulls;
      if (shared_right_) {
        DeltaKeys kr_probe = materialize_keys(dr, /*left=*/false);
        int64_t d_total = 0, d_nulls = 0;
        for (size_t i = 0; i < kr_probe.rows.size(); i++) {
          d_total += kr_probe.weights[i];
          if (!kr_probe.valid[i]) {
            d_nulls += kr_probe.weights[i];
          }
        }
        was_nonempty = (shared_right_->total - d_total) > 0;
        had_nulls = (shared_right_->nulls - d_nulls) > 0;
      } else {
        was_nonempty = right_total_ > 0;
        had_nulls = right_null_ > 0;
        integrate(materialize_keys(dr, /*left=*/false), /*left=*/false);
      }
      if (!shared_left_) {
        integrate(materialize_keys(dl, /*left=*/true), /*left=*/true);
      }
      const bool category_changed =
          was_nonempty != (mark_right_total() > 0) ||
          had_nulls != (mark_right_null() > 0);
      reconcile_marks(dl, dr, category_changed);
      has_output_ = !output_.empty();
      return;
    }

    // Materialize both deltas once with batch-evaluated keys; every pass
    // below (probe passes AND integration) reuses them
    DeltaKeys kl = materialize_keys(dl, /*left=*/true);
    DeltaKeys kr = materialize_keys(dr, /*left=*/false);

    // Δl ⋈ R (local sides: OLD state — integration runs after the
    // passes; a shared side: NEW state — the arrangement was updated
    // before views stepped, and the Δl⋈Δr term is dropped to compensate:
    // Δl⋈R_new + L_old⋈Δr == Δl⋈R_old + L_old⋈Δr + Δl⋈Δr)
    // Intra-operator sharding (L2): pure inner equi-joins (no residuals,
    // no pads, no marks) may split large probe passes across threads —
    // probes are read-only, each shard emits into its own Z-set
    const int shards_cfg = g_intraop_shards.load(std::memory_order_relaxed);
    const bool shardable = shards_cfg > 1 && residuals_.empty() &&
                           !marks_ && !pads_left_ && !pads_right_ &&
                           kl.rows.size() + kr.rows.size() >= 4096;

    if (shardable) {
      run_sharded_probe(kl, /*probe_left_side=*/false, shards_cfg);
    } else if (shared_right_ && shared_right_->is_spilled()) {
      for (size_t i = 0; i < kl.rows.size(); i++) {
        if (!kl.valid[i]) {
          continue;
        }
        const RowWeights *bucket = probe_side(/*left=*/false, kl.keys[i]);
        if (!bucket) {
          continue;
        }
        for (const auto &[rrow, rw] : *bucket) {
          try_emit(*kl.rows[i], rrow, kl.weights[i] * rw);
        }
      }
    } else {
      // Hot path: hoist the index ref out of the loop (per-row branch
      // resolution cost ~20% of join throughput)
      const Index &right_probe = side_index(/*left=*/false);
      for (size_t i = 0; i < kl.rows.size(); i++) {
        if (!kl.valid[i]) {
          continue;
        }
        auto it = right_probe.find(kl.keys[i]);
        if (it == right_probe.end()) {
          continue;
        }
        for (const auto &[rrow, rw] : it->second) {
          try_emit(*kl.rows[i], rrow, kl.weights[i] * rw);
        }
      }
    }

    // L ⋈ Δr
    if (shardable) {
      run_sharded_probe(kr, /*probe_left_side=*/true, shards_cfg);
    } else if (shared_left_ && shared_left_->is_spilled()) {
      for (size_t i = 0; i < kr.rows.size(); i++) {
        if (!kr.valid[i]) {
          continue;
        }
        const RowWeights *bucket = probe_side(/*left=*/true, kr.keys[i]);
        if (!bucket) {
          continue;
        }
        for (const auto &[lrow, lw] : *bucket) {
          try_emit(lrow, *kr.rows[i], lw * kr.weights[i]);
        }
      }
    } else {
      const Index &left_probe = side_index(/*left=*/true);
      for (size_t i = 0; i < kr.rows.size(); i++) {
        if (!kr.valid[i]) {
          continue;
        }
        auto it = left_probe.find(kr.keys[i]);
        if (it == left_probe.end()) {
          continue;
        }
        for (const auto &[lrow, lw] : it->second) {
          try_emit(lrow, *kr.rows[i], lw * kr.weights[i]);
        }
      }
    }

    // Δl ⋈ Δr (both sides changed in the same step, e.g. self-joins).
    // Shared sides expose POST-delta state, which shifts this term:
    //   no side shared:   Δl⋈R_old + L_old⋈Δr + Δl⋈Δr  → emit it (+)
    //   one side shared:  Δl⋈R_new + L_old⋈Δr           → drop it
    //   both shared:      Δl⋈R_new + L_new⋈Δr − Δl⋈Δr  → emit it (−)
    const bool both_shared = shared_left_ && shared_right_;
    const bool any_shared = shared_left_ || shared_right_;
    if ((!any_shared || both_shared) && !kl.rows.empty() &&
        !kr.rows.empty()) {
      const int64_t sign = both_shared ? -1 : 1;
      Index dr_index;
      for (size_t i = 0; i < kr.rows.size(); i++) {
        if (kr.valid[i]) {
          dr_index[kr.keys[i]][*kr.rows[i]] += kr.weights[i];
        }
      }
      for (size_t i = 0; i < kl.rows.size(); i++) {
        if (!kl.valid[i]) {
          continue;
        }
        auto it = dr_index.find(kl.keys[i]);
        if (it == dr_index.end()) {
          continue;
        }
        for (const auto &[rrow, rw] : it->second) {
          try_emit(*kl.rows[i], rrow, sign * kl.weights[i] * rw);
        }
      }
    }

    // Integrate deltas into the LOCAL side indexes (shared sides are
    // maintained by the CDC layer)
    if (!shared_left_) {
      integrate(kl, /*left=*/true);
    }
    if (!shared_right_) {
      integrate(kr, /*left=*/false);
    }

    // Outer-join NULL padding: reconcile the pad of every row whose match
    // count may have changed. Desired pad weight = the row's total weight
    // when it has no (residual-passing) matches, else 0; emit the diff.
    if (pads_left_) {
      reconcile_pads(dl, dr, /*left=*/true);
    }
    if (pads_right_) {
      reconcile_pads(dr, dl, /*left=*/false);
    }
    has_output_ = !output_.empty();
  }

  void reset() override {
    left_index_.clear();
    right_index_.clear();
    left_weights_.clear();
    right_weights_.clear();
    left_pad_.clear();
    right_pad_.clear();
    mark_state_.clear();
    right_total_ = 0;
    right_null_ = 0;
    output_.clear();
    has_output_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

  void set_shared_arrangement(bool left,
                              std::shared_ptr<const SharedArrangement> arr) {
    (left ? shared_left_ : shared_right_) = std::move(arr);
  }

private:
  using RowWeights = std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash>;
  using Index = std::unordered_map<DuckDBRow, RowWeights, DuckDBRowHash>;

  const Index &side_index(bool left) const {
    if (left && shared_left_) {
      return shared_left_->index;
    }
    if (!left && shared_right_) {
      return shared_right_->index;
    }
    return left ? left_index_ : right_index_;
  }

  // Probe one side for `key`. Returns nullptr on no match. For a spilled
  // shared side the bucket materializes into this node's scratch (nodes
  // are per-view, so scratches are thread-private under I2 parallel
  // propagation; the arrangement serializes its own cache internally).
  // The pointer is valid until the next probe_side call on that side.
  const RowWeights *probe_side(bool left, const DuckDBRow &key) {
    const auto &sh = left ? shared_left_ : shared_right_;
    if (sh && sh->is_spilled()) {
      RowWeights &scratch = left ? probe_scratch_left_ : probe_scratch_right_;
      return sh->probe_spilled(key, scratch) ? &scratch : nullptr;
    }
    const Index &idx = side_index(left);
    auto it = idx.find(key);
    return it == idx.end() ? nullptr : &it->second;
  }

  // Returns false when any key value is NULL (row can never match) —
  // unless keys are null-safe (IS NOT DISTINCT FROM, DELIM joins), where
  // NULL is an ordinary key value (DuckDBRow equality treats NULL == NULL)
  bool eval_key(const DuckDBRow &row, bool left, DuckDBRow &key) {
    key.columns.reserve(keys_.size());
    for (auto &k : keys_) {
      duckdb::Value v = left ? k.left->eval(row) : k.right->eval(row);
      if (v.IsNull() && !null_safe_keys_) {
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
    std::vector<duckdb::Value> vals;
    vals.reserve(lrow.columns.size() + rrow.columns.size());
    vals.insert(vals.end(), lrow.columns.begin(), lrow.columns.end());
    vals.insert(vals.end(), rrow.columns.begin(), rrow.columns.end());
    DuckDBRow combined;
    combined.columns.assign(std::move(vals));
    output_.insert(std::move(combined), weight);
  }

  // One side's delta with batch-evaluated keys. valid[i] == false means a
  // NULL key column under non-null-safe semantics (row can never match);
  // with null-safe keys every row is valid and NULLs are key values.
  struct DeltaKeys {
    std::vector<const DuckDBRow *> rows;
    std::vector<int64_t> weights;
    std::vector<DuckDBRow> keys;
    std::vector<char> valid;
  };

  // Range-split one probe pass across shards. `dk` is the delta being
  // probed; probe_left_side is the side whose index gets probed (the
  // OPPOSITE of dk's side). Only called for residual-free inner joins:
  // the emit is a pure concat, and shard-local scratches make spilled
  // probes thread-safe (the arrangement's cache has its own mutex).
  void run_sharded_probe(const DeltaKeys &dk, bool probe_left_side,
                         int shards_cfg) {
    const size_t n = dk.rows.size();
    if (n == 0) {
      return;
    }
    const size_t shards =
        std::min<size_t>(static_cast<size_t>(shards_cfg), (n + 511) / 512);
    if (shards <= 1) {
      for (size_t i = 0; i < n; i++) {
        probe_one(dk, i, probe_left_side, output_, nullptr);
      }
      return;
    }
    std::vector<DuckDBZSet> outs(shards);
    std::vector<std::thread> threads;
    threads.reserve(shards);
    const size_t chunk = (n + shards - 1) / shards;
    for (size_t t = 0; t < shards; t++) {
      threads.emplace_back([&, t]() {
        RowWeights scratch;
        const size_t lo = t * chunk;
        const size_t hi = std::min(n, lo + chunk);
        for (size_t i = lo; i < hi; i++) {
          probe_one(dk, i, probe_left_side, outs[t], &scratch);
        }
      });
    }
    for (auto &th : threads) {
      th.join();
    }
    for (auto &out : outs) {
      for (const auto &[row, w] : out) {
        output_.insert(row, w);
      }
    }
  }

  void probe_one(const DeltaKeys &dk, size_t i, bool probe_left_side,
                 DuckDBZSet &out, RowWeights *scratch) {
    if (!dk.valid[i]) {
      return;
    }
    const RowWeights *bucket;
    const auto &sh = probe_left_side ? shared_left_ : shared_right_;
    if (sh && sh->is_spilled() && scratch) {
      bucket = sh->probe_spilled(dk.keys[i], *scratch) ? scratch : nullptr;
    } else {
      bucket = probe_side(probe_left_side, dk.keys[i]);
    }
    if (!bucket) {
      return;
    }
    for (const auto &[orow, ow] : *bucket) {
      const int64_t w = dk.weights[i] * ow;
      if (w == 0) {
        continue;
      }
      std::vector<duckdb::Value> vals;
      const DuckDBRow &lrow = probe_left_side ? orow : *dk.rows[i];
      const DuckDBRow &rrow = probe_left_side ? *dk.rows[i] : orow;
      vals.reserve(lrow.columns.size() + rrow.columns.size());
      vals.insert(vals.end(), lrow.columns.begin(), lrow.columns.end());
      vals.insert(vals.end(), rrow.columns.begin(), rrow.columns.end());
      DuckDBRow combined;
      combined.columns.assign(std::move(vals));
      out.insert(std::move(combined), w);
    }
  }

  DeltaKeys materialize_keys(const DuckDBZSet &delta, bool left) {
    DeltaKeys out;
    const size_t n = delta.size();
    out.rows.reserve(n);
    out.weights.reserve(n);
    out.keys.resize(n);
    out.valid.assign(n, 1);
    for (const auto &[row, w] : delta) {
      out.rows.push_back(&row);
      out.weights.push_back(w);
    }
    if (keys_.empty()) {
      return out; // keyless join: single empty key, all valid
    }
    BatchEvaluator *be =
        left ? batch_left_keys_.get() : batch_right_keys_.get();
    if (!be) {
      // No batch evaluators wired (unit-constructed node): per-row path
      for (size_t i = 0; i < out.rows.size(); i++) {
        DuckDBRow key;
        if (eval_key(*out.rows[i], left, key)) {
          out.keys[i] = std::move(key);
        } else {
          out.valid[i] = 0;
        }
      }
      return out;
    }
    size_t base = 0;
    while (base < out.rows.size()) {
      const duckdb::idx_t chunk = static_cast<duckdb::idx_t>(
          std::min<size_t>(BatchEvaluator::kBatch, out.rows.size() - base));
      be->fill(out.rows.data() + base, chunk);
      std::vector<std::vector<duckdb::Value>> key_vals(chunk);
      for (auto &kv : key_vals) {
        kv.reserve(be->expr_count());
      }
      for (size_t k = 0; k < be->expr_count(); k++) {
        duckdb::Vector &v = be->execute(k);
        const auto &type = be->return_type(k);
        for (duckdb::idx_t i = 0; i < chunk; i++) {
          duckdb::Value val = BatchEvaluator::read_result(v, type, i);
          if (val.IsNull() && !null_safe_keys_) {
            out.valid[base + i] = 0;
          }
          key_vals[i].push_back(std::move(val));
        }
      }
      for (duckdb::idx_t i = 0; i < chunk; i++) {
        out.keys[base + i].columns.assign(std::move(key_vals[i]));
      }
      base += chunk;
    }
    return out;
  }

  void integrate(const DeltaKeys &dk, bool left) {
    Index &index = left ? left_index_ : right_index_;
    const bool track_weights = left ? (pads_left_ || marks_) : pads_right_;
    RowWeights &weights = left ? left_weights_ : right_weights_;
    for (size_t i = 0; i < dk.rows.size(); i++) {
      const DuckDBRow &row = *dk.rows[i];
      const int64_t w = dk.weights[i];
      if (track_weights) {
        int64_t &total = weights[row];
        total += w;
        if (total == 0) {
          weights.erase(row);
        }
      }
      if (!dk.valid[i]) {
        if (!left && marks_) {
          right_total_ += w;
          right_null_ += w; // NULL key on the subquery side
        }
        continue;
      }
      if (!left && marks_) {
        right_total_ += w;
      }
      auto &rows = index[dk.keys[i]];
      int64_t &weight = rows[row];
      weight += w;
      if (weight == 0) {
        rows.erase(row);
        if (rows.empty()) {
          index.erase(dk.keys[i]);
        }
      }
    }
  }

  // Weighted count of residual-passing matches for one row of the padded
  // side against the other side's integrated index. NULL keys never match.
  int64_t match_count(const DuckDBRow &row, bool left) {
    DuckDBRow key;
    if (!eval_key(row, left, key)) {
      return 0;
    }
    const RowWeights *bucket = probe_side(!left, key);
    if (!bucket) {
      return 0;
    }
    int64_t count = 0;
    for (const auto &[orow, ow] : *bucket) {
      const bool pass = left ? residuals_pass(row, orow)
                             : residuals_pass(orow, row);
      if (pass) {
        count += ow;
      }
    }
    return count;
  }

  DuckDBRow pad_row(const DuckDBRow &row, bool left) const {
    DuckDBRow out;
    if (left) {
      out.columns = row.columns;
      for (const auto &t : right_types_) {
        out.columns.push_back(duckdb::Value(t));
      }
    } else {
      out.columns.reserve(left_types_.size() + row.columns.size());
      for (const auto &t : left_types_) {
        out.columns.push_back(duckdb::Value(t));
      }
      out.columns.insert(out.columns.end(), row.columns.begin(),
                         row.columns.end());
    }
    return out;
  }

  // Reconcile pads for every row whose match count may have changed:
  // rows in this side's delta, plus integrated rows sharing an equi key
  // with the other side's delta (keyless joins share one empty key, so a
  // non-empty other-side delta touches every row — correct, if not cheap).
  void reconcile_pads(const DuckDBZSet &own_delta,
                      const DuckDBZSet &other_delta, bool left) {
    std::unordered_map<DuckDBRow, char, DuckDBRowHash> affected;
    for (const auto &[row, w] : own_delta) {
      affected.emplace(row, 0);
    }
    if (!other_delta.empty()) {
      const Index &own_index = side_index(left);
      std::unordered_map<DuckDBRow, char, DuckDBRowHash> seen_keys;
      for (const auto &[orow, ow] : other_delta) {
        DuckDBRow key;
        if (!eval_key(orow, !left, key)) {
          continue;
        }
        if (!seen_keys.emplace(key, 0).second) {
          continue;
        }
        auto it = own_index.find(key);
        if (it == own_index.end()) {
          continue;
        }
        for (const auto &[row, w] : it->second) {
          affected.emplace(row, 0);
        }
      }
    }

    const RowWeights &weights =
        (left && shared_left_)    ? shared_left_->weights
        : (!left && shared_right_) ? shared_right_->weights
        : (left ? left_weights_ : right_weights_);
    RowWeights &pads = left ? left_pad_ : right_pad_;
    for (const auto &[row, unused] : affected) {
      auto wit = weights.find(row);
      const int64_t total = wit == weights.end() ? 0 : wit->second;
      const int64_t desired =
          (total > 0 && match_count(row, left) == 0) ? total : 0;
      auto pit = pads.find(row);
      const int64_t current = pit == pads.end() ? 0 : pit->second;
      if (desired == current) {
        continue;
      }
      output_.insert(pad_row(row, left), desired - current);
      if (desired == 0) {
        pads.erase(row);
      } else {
        pads[row] = desired;
      }
    }
  }

  // Three-valued mark per SQL IN semantics: TRUE when a residual-passing
  // match exists; FALSE when the subquery side is empty (even for NULL
  // probes); otherwise NULL when the probe key is NULL or the subquery
  // side contains a NULL key; else FALSE.
  int64_t mark_right_total() const {
    return shared_right_ ? shared_right_->total : right_total_;
  }
  int64_t mark_right_null() const {
    return shared_right_ ? shared_right_->nulls : right_null_;
  }

  duckdb::Value mark_value(const DuckDBRow &row) {
    if (match_count(row, /*left=*/true) > 0) {
      return duckdb::Value::BOOLEAN(true);
    }
    if (mark_right_total() <= 0) {
      return duckdb::Value::BOOLEAN(false);
    }
    DuckDBRow key;
    if (!eval_key(row, /*left=*/true, key) || mark_right_null() > 0) {
      return duckdb::Value(duckdb::LogicalType::BOOLEAN);
    }
    return duckdb::Value::BOOLEAN(false);
  }

  void reconcile_marks(const DuckDBZSet &dl, const DuckDBZSet &dr,
                       bool category_changed) {
    std::unordered_map<DuckDBRow, char, DuckDBRowHash> affected;
    if (category_changed) {
      // Emptiness or NULL-presence flipped: every unmatched mark changes
      for (const auto &[row, w] :
           (shared_left_ ? shared_left_->weights : left_weights_)) {
        affected.emplace(row, 0);
      }
      for (const auto &[row, entry] : mark_state_) {
        affected.emplace(row, 0); // rows whose weight just went to 0
      }
    } else {
      for (const auto &[row, w] : dl) {
        affected.emplace(row, 0);
      }
      if (!dr.empty()) {
        std::unordered_map<DuckDBRow, char, DuckDBRowHash> seen_keys;
        for (const auto &[orow, ow] : dr) {
          DuckDBRow key;
          if (!eval_key(orow, /*left=*/false, key)) {
            continue;
          }
          if (!seen_keys.emplace(key, 0).second) {
            continue;
          }
          const Index &lidx = side_index(/*left=*/true);
          auto it = lidx.find(key);
          if (it == lidx.end()) {
            continue;
          }
          for (const auto &[row, w] : it->second) {
            affected.emplace(row, 0);
          }
        }
      }
    }

    const RowWeights &lw =
        shared_left_ ? shared_left_->weights : left_weights_;
    for (const auto &[row, unused] : affected) {
      auto wit = lw.find(row);
      const int64_t weight = wit == lw.end() ? 0 : wit->second;
      duckdb::Value mark =
          weight > 0 ? mark_value(row) : duckdb::Value::BOOLEAN(false);

      auto sit = mark_state_.find(row);
      if (sit != mark_state_.end()) {
        const auto &[old_mark, old_w] = sit->second;
        if (weight > 0 && old_w == weight &&
            old_mark.IsNull() == mark.IsNull() &&
            (old_mark.IsNull() ||
             old_mark.GetValue<bool>() == mark.GetValue<bool>())) {
          continue; // unchanged
        }
        DuckDBRow old_row = row;
        old_row.columns.push_back(old_mark);
        output_.insert(old_row, -old_w);
        mark_state_.erase(sit);
      }
      if (weight > 0) {
        DuckDBRow new_row = row;
        new_row.columns.push_back(mark);
        output_.insert(new_row, weight);
        mark_state_.emplace(row, std::make_pair(mark, weight));
      }
    }
  }

  InputFn left_fn_, right_fn_;
  std::vector<KeyPair> keys_;
  std::vector<Residual> residuals_;
  duckdb::JoinType join_type_;
  duckdb::vector<duckdb::LogicalType> left_types_, right_types_;
  bool pads_left_ = false, pads_right_ = false;
  bool marks_ = false;
  bool null_safe_keys_ = false;
  std::unique_ptr<BatchEvaluator> batch_left_keys_, batch_right_keys_;
  // I1: at most one side reads a shared, CDC-maintained arrangement
  // (updated BEFORE views step, so it is post-delta for the current step)
  std::shared_ptr<const SharedArrangement> shared_left_, shared_right_;
  RowWeights probe_scratch_left_, probe_scratch_right_;
  Index left_index_, right_index_;
  RowWeights left_weights_, right_weights_;   // incl. NULL-key rows
  RowWeights left_pad_, right_pad_;           // currently emitted pad weight
  int64_t right_total_ = 0, right_null_ = 0;  // MARK: subquery-side stats
  std::unordered_map<DuckDBRow, std::pair<duckdb::Value, int64_t>,
                     DuckDBRowHash>
      mark_state_; // MARK: emitted (mark, weight) per left row
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

// Batched filter/project node (D1): covers FILTER_EXPR (filters only,
// identity output), MAP_EXPR (projections only), and the IR optimizer's
// fused FILTER_MAP (both). Delta rows are evaluated in DataChunk batches
// through ChunkedEval; projections run over filter survivors only, matching
// the old per-row semantics. Weight-preserving.
class PlanBatchNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  // exprs = filters (num_filters of them) followed by projections; all
  // evaluate against ONE shared input chunk filled once per batch, with
  // survivors selected via DataChunk::Slice (no refill)
  PlanBatchNode(dbsp::NodeId id, InputFn input_fn,
                std::shared_ptr<PlanKeepAlive> keep_alive,
                std::vector<const duckdb::Expression *> filter_exprs,
                std::vector<const duckdb::Expression *> proj_exprs,
                duckdb::vector<duckdb::LogicalType> input_types)
      : dbsp::Node(id, "plan_batch"), input_fn_(std::move(input_fn)),
        num_filters_(filter_exprs.size()) {
    for (const auto *e : proj_exprs) {
      filter_exprs.push_back(e);
    }
    eval_ = std::make_unique<BatchEvaluator>(
        std::move(keep_alive), std::move(filter_exprs),
        std::move(input_types));
  }

  void step() override {
    output_.clear();
    const DuckDBZSet &input = input_fn_();

    std::vector<const DuckDBRow *> rows;
    std::vector<int64_t> weights;
    rows.reserve(BatchEvaluator::kBatch);
    weights.reserve(BatchEvaluator::kBatch);

    const size_t num_projs = eval_->expr_count() - num_filters_;

    auto flush = [&]() {
      if (rows.empty()) {
        return;
      }
      const duckdb::idx_t n = rows.size();
      eval_->fill(rows.data(), n);

      // Filters: conjunction over the batch (NULL = fail)
      duckdb::SelectionVector sel(n);
      duckdb::idx_t m = n;
      if (num_filters_ > 0) {
        std::vector<bool> keep(n, true);
        for (size_t f = 0; f < num_filters_; f++) {
          duckdb::Vector &v = eval_->execute(f); // flattened BOOLEAN
          auto &validity = duckdb::FlatVector::Validity(v);
          auto data = duckdb::FlatVector::GetData<bool>(v);
          for (duckdb::idx_t i = 0; i < n; i++) {
            if (keep[i] && (!validity.RowIsValid(i) || !data[i])) {
              keep[i] = false;
            }
          }
        }
        m = 0;
        for (duckdb::idx_t i = 0; i < n; i++) {
          if (keep[i]) {
            sel.set_index(m++, i);
          }
        }
      } else {
        for (duckdb::idx_t i = 0; i < n; i++) {
          sel.set_index(i, i);
        }
      }

      if (num_projs == 0) {
        for (duckdb::idx_t i = 0; i < m; i++) {
          const duckdb::idx_t src = sel.get_index(i);
          output_.insert(*rows[src], weights[src]);
        }
      } else if (m > 0) {
        // Projections over survivors only: slice the already-filled chunk
        if (m < n) {
          eval_->slice(sel, m);
        }
        std::vector<std::vector<duckdb::Value>> out(m);
        for (auto &vals : out) {
          vals.reserve(num_projs);
        }
        for (size_t p = 0; p < num_projs; p++) {
          const size_t e = num_filters_ + p;
          duckdb::Vector &v = eval_->execute(e);
          const auto &type = eval_->return_type(e);
          for (duckdb::idx_t i = 0; i < m; i++) {
            out[i].push_back(BatchEvaluator::read_result(v, type, i));
          }
        }
        for (duckdb::idx_t i = 0; i < m; i++) {
          DuckDBRow row;
          row.columns.assign(std::move(out[i]));
          output_.insert(std::move(row), weights[sel.get_index(i)]);
        }
      }

      rows.clear();
      weights.clear();
    };

    for (const auto &[row, w] : input) {
      rows.push_back(&row);
      weights.push_back(w);
      if (rows.size() == BatchEvaluator::kBatch) {
        flush();
      }
    }
    flush();
    has_output_ = !output_.empty();
  }

  void reset() override {
    output_.clear();
    has_output_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

private:
  InputFn input_fn_;
  size_t num_filters_;
  std::unique_ptr<BatchEvaluator> eval_;
  DuckDBZSet output_;
  bool has_output_ = false;
};

// Fixed-point driver for WITH RECURSIVE. The anchor is computed inline in
// the outer circuit; the recursive step runs as an inner view (a nested
// PlannedCircuitView) whose self-reference is a source named `sentinel`.
//
// Insert-only deltas are handled incrementally: seed the frontier with the
// anchor delta plus the step's reaction to base-table deltas, then iterate
// the step on the frontier until it stops producing new rows (UNION dedup
// state persists across calls, so later deltas cannot double-count).
//
// Deltas containing a deletion fall back to a full fixed-point recompute
// from integrated anchor/base state: a derived row may be supported by any
// number of recursion paths, so retraction cannot be decided locally. The
// node integrates its inputs (anchor_total_, base_totals_) for exactly this
// purpose and emits the diff against the previous accumulated state.
// Correct always; O(fixed point) instead of O(delta) when deletions occur.
class PlanRecursiveNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  PlanRecursiveNode(dbsp::NodeId id, InputFn anchor,
                    std::unique_ptr<NativeMaterializedView> step_view,
                    std::string sentinel, bool union_all,
                    std::vector<std::pair<std::string, InputFn>> base_inputs,
                    size_t max_iterations = 1000)
      : dbsp::Node(id, "plan_recursive"), anchor_(std::move(anchor)),
        step_view_(std::move(step_view)), sentinel_(std::move(sentinel)),
        union_all_(union_all), base_inputs_(std::move(base_inputs)),
        max_iterations_(max_iterations) {}

  void step() override {
    output_.clear();

    // Integrate inputs; detect deletions
    const DuckDBZSet &anchor_delta = anchor_();
    bool has_deletion = false;
    for (const auto &[row, w] : anchor_delta) {
      anchor_total_.insert(row, w);
      has_deletion |= w < 0;
    }
    std::vector<std::pair<std::string, const DuckDBZSet *>> base_deltas;
    for (auto &[table, fn] : base_inputs_) {
      const DuckDBZSet &d = fn();
      if (d.empty()) {
        continue;
      }
      auto &total = base_totals_[table];
      for (const auto &[row, w] : d) {
        total.insert(row, w);
        has_deletion |= w < 0;
      }
      base_deltas.emplace_back(table, &d);
    }

    if (has_deletion) {
      recompute();
      has_output_ = !output_.empty();
      return;
    }

    // Incremental insert-only path
    DuckDBZSet seed = anchor_delta;
    for (const auto &[table, d] : base_deltas) {
      step_view_->apply_changes(table, *d);
      for (const auto &[row, w] : step_view_->get_delta()) {
        seed.insert(row, w);
      }
    }
    DuckDBZSet frontier;
    for (const auto &[row, w] : seed) {
      if (w > 0) {
        admit(row, w, frontier, output_);
      }
    }
    iterate(frontier, output_);
    has_output_ = !output_.empty();
  }

  void reset() override {
    accumulated_.clear();
    anchor_total_.clear();
    base_totals_.clear();
    output_.clear();
    has_output_ = false;
    step_view_->reset();
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

private:
  // Re-run the whole fixed point from integrated inputs; output_ becomes
  // the diff against the previous accumulated state
  void recompute() {
    DuckDBZSet old_accumulated = std::move(accumulated_);
    accumulated_ = DuckDBZSet();
    step_view_->reset();

    DuckDBZSet seed = anchor_total_;
    for (const auto &[table, total] : base_totals_) {
      if (total.empty()) {
        continue;
      }
      step_view_->apply_changes(table, total);
      for (const auto &[row, w] : step_view_->get_delta()) {
        seed.insert(row, w);
      }
    }
    DuckDBZSet scratch; // recompute emits via diff, not via admit
    DuckDBZSet frontier;
    for (const auto &[row, w] : seed) {
      if (w > 0) {
        admit(row, w, frontier, scratch);
      }
    }
    iterate(frontier, scratch);

    for (const auto &[row, w] : accumulated_) {
      output_.insert(row, w);
    }
    for (const auto &[row, w] : old_accumulated) {
      output_.insert(row, -w);
    }
  }

  void iterate(DuckDBZSet &frontier, DuckDBZSet &out) {
    size_t iter = 0;
    while (!frontier.empty() && iter++ < max_iterations_) {
      step_view_->apply_changes(sentinel_, frontier);
      DuckDBZSet next;
      for (const auto &[row, w] : step_view_->get_delta()) {
        if (w > 0) {
          admit(row, w, next, out);
        }
      }
      frontier = std::move(next);
    }
  }

  void admit(const DuckDBRow &row, int64_t w, DuckDBZSet &frontier,
             DuckDBZSet &out) {
    if (union_all_) {
      accumulated_.insert(row, w);
      out.insert(row, w);
      frontier.insert(row, w);
    } else if (accumulated_.get(row) == 0) {
      accumulated_.insert(row, 1);
      out.insert(row, 1);
      frontier.insert(row, 1);
    }
  }

  InputFn anchor_;
  std::unique_ptr<NativeMaterializedView> step_view_;
  std::string sentinel_;
  bool union_all_;
  std::vector<std::pair<std::string, InputFn>> base_inputs_;
  size_t max_iterations_;
  DuckDBZSet accumulated_;
  DuckDBZSet anchor_total_;                            // ∫ anchor deltas
  std::unordered_map<std::string, DuckDBZSet> base_totals_; // ∫ per table
  DuckDBZSet output_;
  bool has_output_ = false;
};

// Circuit view built from a translated plan tree. One SourceNode per base
// table (shared across subtrees, e.g. self-joins); apply_changes pushes the
// delta into the matching source and steps the whole circuit once.
class PlannedCircuitView : public NativeMaterializedView {
public:
  using RowSource = dbsp::SourceNode<DuckDBRow, DuckDBRowHash>;
  using RowSink = dbsp::SinkNode<DuckDBRow, DuckDBRowHash>;
  using RowMap =
      dbsp::MapNode<DuckDBRow, DuckDBRow, DuckDBRowHash, DuckDBRowHash>;
  using OutputFn = std::function<const DuckDBZSet &()>;

  PlannedCircuitView(const std::string &name, const std::string &sql,
                     const TableSchema &result_schema,
                     std::shared_ptr<PlanKeepAlive> keep_alive,
                     const PlanOpSpec &root)
      : NativeMaterializedView(name, sql), schema_(result_schema),
        keep_alive_(keep_alive) {
    schema_.table_name = name;
    count_sources(root);
    OutputFn out = build(root, keep_alive);
    sink_ = circuit_.add_node(std::make_unique<RowSink>(
        circuit_.next_node_id(), std::move(out), name_ + "_sink"));
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    auto it = sources_.find(table_name);
    if (it != sources_.end()) {
      it->second->push_borrowed(changes);
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

  // Circuit size; used by IR-optimizer tests to prove rewrites fired
  size_t node_count() const { return circuit_.node_count(); }

  // I1 shared arrangements: join sides eligible for a shared, CDC-owned
  // arrangement (resolved by CDCManager after sources are tracked)
  const std::vector<ArrangementRequest> &arrangement_requests() const {
    return arrangement_requests_;
  }
  // Tables whose state must NOT be replayed at view initialization: their
  // only consumer is a shared join side whose arrangement is already
  // populated (replaying would double-count through Δl⋈R_arr)
  const std::unordered_set<std::string> &shared_init_skip() const {
    return shared_init_skip_;
  }
  void mark_shared_init_skip(const std::string &table) {
    shared_init_skip_.insert(table);
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
  void count_sources(const PlanOpSpec &spec) {
    if (spec.kind == PlanOpSpec::Kind::SOURCE) {
      source_refs_[spec.table]++;
    }
    for (const auto &child : spec.children) {
      count_sources(*child);
    }
  }

  // v1 eligibility: side is a bare SOURCE referenced exactly once in the
  // whole plan (so init replay can skip that table wholesale) and no more
  // than one side per join shares (keeps init = plain local-side replay).
  // Prefer the right side (conventionally the bigger build side).
  void record_arrangement_request(
      const PlanOpSpec &spec, PlanJoinNode *node,
      const std::vector<const duckdb::Expression *> &lkeys,
      const std::vector<const duckdb::Expression *> &rkeys) {
    // A shared side must be a pure probe target. A side that pads or
    // marks ITSELF (right of RIGHT/FULL, left of LEFT/FULL/MARK) cannot
    // share: init replay skips its table, so its unmatched rows would
    // never be visited and their NULL pads / marks never emitted.
    auto self_padding = [&](bool left) {
      if (left) {
        return spec.join_type == duckdb::JoinType::LEFT ||
               spec.join_type == duckdb::JoinType::OUTER ||
               spec.join_type == duckdb::JoinType::MARK;
      }
      return spec.join_type == duckdb::JoinType::RIGHT ||
             spec.join_type == duckdb::JoinType::OUTER;
    };
    // A side qualifies as a bare scan when it is SOURCE or
    // MAP_COLS(SOURCE) — the projection is folded into the arrangement
    auto scan_of = [](const PlanOpSpec &c) -> const PlanOpSpec * {
      if (c.kind == PlanOpSpec::Kind::SOURCE) {
        return &c;
      }
      if (c.kind == PlanOpSpec::Kind::MAP_COLS &&
          c.children[0]->kind == PlanOpSpec::Kind::SOURCE) {
        return c.children[0].get();
      }
      return nullptr;
    };
    auto eligible = [&](size_t child) {
      const PlanOpSpec *src = scan_of(*spec.children[child]);
      return src && source_refs_[src->table] == 1 &&
             !self_padding(child == 0);
    };
    const bool right_ok = eligible(1);
    const bool left_ok = eligible(0);
    if (!right_ok && !left_ok) {
      return;
    }
    // Both sides shareable: skip only the RIGHT side's init replay — the
    // left side's full replay ⋈ right arrangement bootstraps the join
    // (skipping both would leave the view empty at init)
    for (const bool left_side : {false, true}) {
      if (left_side ? !left_ok : !right_ok) {
        continue;
      }
      const PlanOpSpec &side = *spec.children[left_side ? 0 : 1];
      const auto &key_exprs = left_side ? lkeys : rkeys;

      ArrangementRequest req;
      req.table = scan_of(side)->table;
      if (side.kind == PlanOpSpec::Kind::MAP_COLS) {
        req.project = true;
        req.column_idxs = side.column_idxs;
      }
      req.left_side = left_side;
      req.init_skip = !(left_side && right_ok);
      req.null_safe = spec.null_safe_keys;
      // Probe-target sides never need per-row weights (those serve
      // self-pads/marks, excluded above); marks on a shared RIGHT side
      // still need the total/null-key counters
      req.track_weights = false;
      req.track_counters =
          !left_side && spec.join_type == duckdb::JoinType::MARK;
      req.key_exprs = key_exprs;
      req.side_types = left_side ? spec.left_types : spec.right_types;
      req.keep_alive = keep_alive_;
      req.node = node;
      finish_request(std::move(req));
    }
  }

  void finish_request(ArrangementRequest req) {
    const auto &key_exprs = req.key_exprs;
    std::string fp = req.table;
    fp += req.null_safe ? "|ns1" : "|ns0";
    fp += req.track_weights ? "|w1" : "|w0";
    fp += req.track_counters ? "|c1" : "|c0";
    fp += "|p";
    for (auto idx : req.column_idxs) {
      fp += std::to_string(idx);
      fp += ",";
    }
    for (const auto *e : key_exprs) {
      fp += "|";
      fp += e->ToString();
    }
    req.fingerprint = std::move(fp);
    arrangement_requests_.push_back(std::move(req));
  }


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
      auto *node = circuit_.add_node(std::make_unique<PlanBatchNode>(
          circuit_.next_node_id(), std::move(child), keep_alive, spec.exprs,
          std::vector<const duckdb::Expression *>{}, spec.input_types));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::MAP_EXPR: {
      OutputFn child = build(*spec.children[0], keep_alive);
      auto *node = circuit_.add_node(std::make_unique<PlanBatchNode>(
          circuit_.next_node_id(), std::move(child), keep_alive,
          std::vector<const duckdb::Expression *>{}, spec.exprs,
          spec.input_types));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::AGGREGATE: {
      OutputFn child = build(*spec.children[0], keep_alive);
      std::vector<const duckdb::Expression *> arg_exprs;
      std::vector<PlanAggregateNode::AggInstance> aggs;
      for (const auto &agg_spec : spec.agg_specs) {
        PlanAggregateNode::AggInstance inst;
        inst.fn = agg_spec.fn;
        inst.integer_arg = agg_spec.integer_arg;
        inst.decimal_arg = agg_spec.decimal_arg;
        inst.decimal_scale = agg_spec.decimal_scale;
        inst.return_type = agg_spec.return_type;
        inst.distinct = agg_spec.distinct;
        inst.separator = agg_spec.separator;
        inst.quantile = agg_spec.quantile;
        if (agg_spec.arg) {
          inst.arg_idx = static_cast<int>(arg_exprs.size());
          arg_exprs.push_back(agg_spec.arg);
        }
        if (agg_spec.filter) {
          inst.filter_idx = static_cast<int>(arg_exprs.size());
          arg_exprs.push_back(agg_spec.filter);
        }
        for (const auto &key : agg_spec.order_keys) {
          inst.order_idxs.push_back(static_cast<int>(arg_exprs.size()));
          inst.order_dirs.emplace_back(key.ascending, key.nulls_first);
          arg_exprs.push_back(key.expr);
        }
        aggs.push_back(std::move(inst));
      }
      auto *node = circuit_.add_node(std::make_unique<PlanAggregateNode>(
          circuit_.next_node_id(), std::move(child), keep_alive, spec.exprs,
          std::move(arg_exprs), std::move(aggs), spec.input_types));
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
      std::vector<const duckdb::Expression *> lkeys, rkeys;
      for (const auto &cond : spec.equi_conds) {
        lkeys.push_back(cond.left);
        rkeys.push_back(cond.right);
      }
      auto lkeys_copy = lkeys;
      auto rkeys_copy = rkeys;
      auto *node = circuit_.add_node(std::make_unique<PlanJoinNode>(
          circuit_.next_node_id(), std::move(left), std::move(right),
          std::move(keys), std::move(residuals), spec.join_type,
          spec.left_types, spec.right_types, spec.null_safe_keys,
          keep_alive, std::move(lkeys), std::move(rkeys)));
      record_arrangement_request(spec, node, lkeys_copy, rkeys_copy);
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
    case PlanOpSpec::Kind::FILTER_MAP: {
      OutputFn child = build(*spec.children[0], keep_alive);
      auto *node = circuit_.add_node(std::make_unique<PlanBatchNode>(
          circuit_.next_node_id(), std::move(child), keep_alive,
          spec.filter_exprs, spec.exprs, spec.input_types));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::DELIM_JOIN: {
      OutputFn left = build(*spec.children[0], keep_alive);
      // DISTINCT of the correlated key columns, shared by every DELIM_GET
      // in the right subplan (incremental: emits key deltas as the set of
      // distinct outer keys changes)
      auto *keys_node = circuit_.add_node(std::make_unique<PlanBatchNode>(
          circuit_.next_node_id(), left, keep_alive,
          std::vector<const duckdb::Expression *>{}, spec.exprs,
          spec.left_types));
      auto *distinct_node =
          circuit_.add_node(std::make_unique<PlanDistinctNode>(
              circuit_.next_node_id(),
              [keys_node]() -> const DuckDBZSet & {
                return keys_node->output();
              },
              "plan_delim_keys"));
      delim_stack_.push_back([distinct_node]() -> const DuckDBZSet & {
        return distinct_node->output();
      });
      OutputFn right = build(*spec.children[1], keep_alive);
      delim_stack_.pop_back();

      std::vector<PlanJoinNode::KeyPair> keys;
      for (const auto &cond : spec.equi_conds) {
        PlanJoinNode::KeyPair kp;
        kp.left = std::make_unique<RowExprEval>(keep_alive, *cond.left,
                                                spec.left_types);
        kp.right = std::make_unique<RowExprEval>(keep_alive, *cond.right,
                                                 spec.right_types);
        keys.push_back(std::move(kp));
      }
      std::vector<const duckdb::Expression *> lkeys, rkeys;
      for (const auto &cond : spec.equi_conds) {
        lkeys.push_back(cond.left);
        rkeys.push_back(cond.right);
      }
      auto *node = circuit_.add_node(std::make_unique<PlanJoinNode>(
          circuit_.next_node_id(), std::move(left), std::move(right),
          std::move(keys), std::vector<PlanJoinNode::Residual>{},
          spec.join_type, spec.left_types, spec.right_types,
          spec.null_safe_keys, keep_alive, std::move(lkeys),
          std::move(rkeys), "plan_delim_join"));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::DELIM_REF:
      return delim_stack_.back();
    case PlanOpSpec::Kind::DISTINCT_ON: {
      OutputFn child = build(*spec.children[0], keep_alive);
      TableSchema vschema;
      vschema.table_name = name_ + "_distinct_on";
      std::vector<size_t> keys;
      keys.reserve(spec.column_idxs.size());
      for (auto idx : spec.column_idxs) {
        keys.push_back(static_cast<size_t>(idx));
      }
      std::vector<NativeDistinctOnView::SortColumn> order;
      order.reserve(spec.sort_columns.size());
      for (const auto &sc : spec.sort_columns) {
        order.push_back({sc.column_idx, sc.ascending, sc.nulls_first});
      }
      auto view = std::make_unique<NativeDistinctOnView>(
          name_ + "_distinct_on", "", EmbeddedViewNode::kInputName, vschema,
          std::move(keys), std::move(order));
      auto *node = circuit_.add_node(std::make_unique<EmbeddedViewNode>(
          circuit_.next_node_id(), std::move(child), std::move(view)));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
    case PlanOpSpec::Kind::REC_CTE: {
      OutputFn anchor = build(*spec.children[0], keep_alive);
      std::string sentinel =
          "__rec_cte_" + std::to_string(spec.cte_index) + "__";
      TableSchema step_schema;
      step_schema.table_name = name_ + "_rec_step";
      auto step_view = std::make_unique<PlannedCircuitView>(
          name_ + "_rec_step", "", step_schema, keep_alive,
          *spec.children[1]);
      // The inner view routes by source name; the outer circuit must own a
      // SourceNode for every base table the step reads (CDC pushes deltas
      // into outer sources only). The sentinel stays internal.
      std::vector<std::pair<std::string, PlanRecursiveNode::InputFn>>
          base_inputs;
      for (const auto &t : step_view->source_tables()) {
        if (t == sentinel) {
          continue;
        }
        PlanOpSpec src;
        src.kind = PlanOpSpec::Kind::SOURCE;
        src.table = t;
        base_inputs.emplace_back(t, build(src, keep_alive));
      }
      bool union_all = spec.set_op == PlanOpSpec::SetOp::UNION_ALL;
      auto *node = circuit_.add_node(std::make_unique<PlanRecursiveNode>(
          circuit_.next_node_id(), std::move(anchor), std::move(step_view),
          sentinel, union_all, std::move(base_inputs)));
      return [node]() -> const DuckDBZSet & { return node->output(); };
    }
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
      if (spec.limit >= 0 || spec.offset > 0 || spec.limit_percent >= 0) {
        view = std::make_unique<NativeLimitView>(
            name_ + "_limit", "", EmbeddedViewNode::kInputName, vschema,
            spec.limit, spec.offset, spec.sort_columns, project,
            spec.limit_percent);
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
  std::vector<OutputFn> delim_stack_; // enclosing DELIM joins' key outputs
  std::shared_ptr<PlanKeepAlive> keep_alive_;
  std::unordered_map<std::string, int> source_refs_; // SOURCE count per table
  std::vector<ArrangementRequest> arrangement_requests_;
  std::unordered_set<std::string> shared_init_skip_;
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
  //
  // mv_schemas: name + schema of every existing materialized view. MVs are
  // not in DuckDB's catalog, so views-on-views would fail to bind; empty
  // TEMP tables mirroring their schemas are created on the internal
  // connection (the temp schema shadows main, matching CDC's own
  // views-before-tables resolution order). They exist only for plan
  // extraction — no data ever flows through them.
  static Result translate(
      duckdb::ClientContext &context, const std::string &view_name,
      const std::string &sql,
      const std::vector<std::pair<std::string, TableSchema>> &mv_schemas = {}) {
    auto keep_alive = std::make_shared<PlanKeepAlive>();
    try {
      keep_alive->connection = std::make_unique<duckdb::Connection>(
          duckdb::DatabaseInstance::GetDatabase(context));
      for (const auto &[mv_name, mv_schema] : mv_schemas) {
        std::string ddl = "CREATE TEMP TABLE \"" + mv_name + "\" (";
        for (size_t i = 0; i < mv_schema.columns.size(); i++) {
          if (i > 0) {
            ddl += ", ";
          }
          ddl += "\"" + mv_schema.columns[i].name + "\" " +
                 mv_schema.columns[i].type.ToString();
        }
        ddl += ")";
        auto res = keep_alive->connection->Query(ddl);
        if (res->HasError()) {
          return {nullptr, "planner frontend: could not shadow view '" +
                               mv_name + "': " + res->GetError()};
        }
      }
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
    for (auto &e : walker.owned_exprs) {
      keep_alive->rewritten_exprs.push_back(std::move(e));
    }
    walker.owned_exprs.clear();
    if (root->kind == PlanOpSpec::Kind::SORT_LIMIT) {
      // Only a root sort/limit drives dbsp_query's scan order; nested ones
      // (subqueries) affect membership only
      root->presentation_root = true;
    }
    if (g_plan_ir_optimize) {
      plan_ir::optimize(root, *keep_alive);
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
    // Expressions synthesized during translation (NULL pads / GROUPING
    // constants for grouping-set branches). Moved into
    // PlanKeepAlive::rewritten_exprs after the walk so they outlive the
    // circuit's evaluators.
    std::vector<std::unique_ptr<duckdb::Expression>> owned_exprs;

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
        return visit_delim_join(op.Cast<duckdb::LogicalComparisonJoin>());
      case duckdb::LogicalOperatorType::LOGICAL_DELIM_GET:
        return visit_delim_get(op.Cast<duckdb::LogicalDelimGet>());
      case duckdb::LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
        return visit_recursive_cte(op.Cast<duckdb::LogicalRecursiveCTE>());
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
      double limit_percent = -1;
      using LT = duckdb::LimitNodeType;
      if (op.limit_val.Type() == LT::CONSTANT_VALUE) {
        limit = static_cast<int64_t>(op.limit_val.GetConstantValue());
      } else if (op.limit_val.Type() == LT::CONSTANT_PERCENTAGE) {
        limit_percent = op.limit_val.GetConstantPercentage();
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
        return visit_order(child.Cast<duckdb::LogicalOrder>(), limit, offset,
                           limit_percent);
      }
      auto child_spec = visit(child);
      if (!child_spec) {
        return nullptr;
      }
      return make_sort_limit(std::move(child_spec), {}, limit, offset,
                             limit_percent);
    }

    SpecPtr visit_order(duckdb::LogicalOrder &op, int64_t limit,
                        int64_t offset, double limit_percent = -1) {
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
                             offset, limit_percent);
    }

    // ORDER BY/LIMIT pass rows through (LIMIT changes membership, not
    // layout): columns stay as the child left them
    SpecPtr make_sort_limit(SpecPtr child,
                            std::vector<NativeSortView::SortColumn> cols,
                            int64_t limit, int64_t offset,
                            double limit_percent = -1) {
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::SORT_LIMIT;
      spec->sort_columns = std::move(cols);
      spec->limit = limit;
      spec->offset = offset;
      spec->limit_percent = limit_percent;
      spec->children.push_back(std::move(child));
      return spec;
    }

    SpecPtr visit_aggregate(duckdb::LogicalAggregate &op) {
      // Parse aggregate expressions once; grouping-set branches reuse them
      std::vector<PlanAggSpec> parsed;
      if (!parse_agg_exprs(op, parsed)) {
        return nullptr;
      }
      if (op.grouping_sets.size() > 1 || !op.grouping_functions.empty()) {
        return build_grouping_sets(op, parsed);
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
      spec->agg_specs = std::move(parsed);
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

    // GROUPING SETS / ROLLUP / CUBE: one aggregate branch per grouping
    // set over its own copy of the input subtree, each mapped to the full
    // output layout (excluded group columns → typed NULLs, GROUPING()
    // → per-branch constant bitmask), then UNION ALL. Each branch is an
    // ordinary incremental aggregate, so the whole construct is
    // incremental for free.
    SpecPtr build_grouping_sets(duckdb::LogicalAggregate &op,
                                const std::vector<PlanAggSpec> &parsed) {
      const size_t num_groups = op.groups.size();
      const size_t num_aggs = op.expressions.size();

      std::vector<duckdb::GroupingSet> sets = op.grouping_sets;
      if (sets.empty()) {
        duckdb::GroupingSet all;
        for (duckdb::idx_t j = 0; j < num_groups; j++) {
          all.insert(j);
        }
        sets.push_back(std::move(all));
      }

      auto union_spec = std::make_unique<PlanOpSpec>();
      union_spec->kind = PlanOpSpec::Kind::SET_OP;
      union_spec->set_op = PlanOpSpec::SetOp::UNION_ALL;

      for (const auto &gset : sets) {
        auto child = visit(*op.children[0]);
        if (!child) {
          return nullptr;
        }

        auto agg = std::make_unique<PlanOpSpec>();
        agg->kind = PlanOpSpec::Kind::AGGREGATE;
        agg->input_types = op.children[0]->types;
        std::unordered_map<duckdb::idx_t, size_t> pos_in_set;
        duckdb::vector<duckdb::LogicalType> agg_out_types;
        for (duckdb::idx_t j : gset) { // set<idx_t>: ascending
          pos_in_set[j] = agg->exprs.size();
          agg->exprs.push_back(op.groups[j].get());
          agg_out_types.push_back(op.types[j]);
        }
        agg->agg_specs = parsed;
        agg->children.push_back(std::move(child));
        for (size_t k = 0; k < num_aggs; k++) {
          agg_out_types.push_back(op.types[num_groups + k]);
        }

        // Map the branch to the full layout
        auto map = std::make_unique<PlanOpSpec>();
        map->kind = PlanOpSpec::Kind::MAP_EXPR;
        map->input_types = agg_out_types;
        for (duckdb::idx_t j = 0; j < num_groups; j++) {
          std::unique_ptr<duckdb::Expression> e;
          auto it = pos_in_set.find(j);
          if (it != pos_in_set.end()) {
            e = duckdb::make_uniq<duckdb::BoundReferenceExpression>(
                op.types[j], it->second);
          } else {
            e = duckdb::make_uniq<duckdb::BoundConstantExpression>(
                duckdb::Value(op.types[j]));
          }
          map->exprs.push_back(e.get());
          owned_exprs.push_back(std::move(e));
        }
        for (size_t k = 0; k < num_aggs; k++) {
          auto e = duckdb::make_uniq<duckdb::BoundReferenceExpression>(
              op.types[num_groups + k], gset.size() + k);
          map->exprs.push_back(e.get());
          owned_exprs.push_back(std::move(e));
        }
        for (size_t f = 0; f < op.grouping_functions.size(); f++) {
          // GROUPING(c1..cn): bit i (MSB-first) set when ci is NOT part
          // of this branch's grouping set
          const auto &args = op.grouping_functions[f];
          int64_t mask = 0;
          for (size_t a = 0; a < args.size(); a++) {
            mask <<= 1;
            if (!gset.count(args[a])) {
              mask |= 1;
            }
          }
          auto e = duckdb::make_uniq<duckdb::BoundConstantExpression>(
              duckdb::Value::BIGINT(mask));
          map->exprs.push_back(e.get());
          owned_exprs.push_back(std::move(e));
        }
        map->children.push_back(std::move(agg));
        union_spec->children.push_back(std::move(map));
      }

      columns.clear();
      for (duckdb::idx_t j = 0; j < num_groups; j++) {
        columns.push_back({op.groups[j]->GetName(), op.types[j]});
      }
      for (size_t k = 0; k < num_aggs; k++) {
        columns.push_back(
            {op.expressions[k]->GetName(), op.types[num_groups + k]});
      }
      for (size_t f = 0; f < op.grouping_functions.size(); f++) {
        columns.push_back({"grouping_" + std::to_string(f),
                           op.types[num_groups + num_aggs + f]});
      }

      if (union_spec->children.size() == 1) {
        return std::move(union_spec->children[0]);
      }
      return union_spec;
    }

    // Parse BoundAggregateExpressions into PlanAggSpecs (fn, argument,
    // FILTER predicate, DISTINCT). Returns false with error set on
    // unsupported constructs.
    bool parse_agg_exprs(duckdb::LogicalAggregate &op,
                         std::vector<PlanAggSpec> &out) {
      for (const auto &expr : op.expressions) {
        if (expr->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_AGGREGATE) {
          unsupported("non-aggregate expression in AGGREGATE");
          return false;
        }
        auto &agg = expr->Cast<duckdb::BoundAggregateExpression>();

        PlanAggSpec agg_spec;
        agg_spec.distinct = agg.IsDistinct();
        agg_spec.filter = agg.filter.get();
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
        } else if (fn == "first" || fn == "arbitrary") {
          // Scalar-subquery plans wrap the inner aggregate in first();
          // the input there is a single row, so any deterministic pick
          // (smallest value) is exact
          agg_spec.fn = PlanAggSpec::Fn::FIRST;
        } else if (fn == "string_agg" || fn == "group_concat" ||
                   fn == "listagg") {
          agg_spec.fn = PlanAggSpec::Fn::STRING_AGG;
        } else if (fn == "array_agg" || fn == "list") {
          agg_spec.fn = PlanAggSpec::Fn::ARRAY_AGG;
        } else if (fn == "median") {
          agg_spec.fn = PlanAggSpec::Fn::MEDIAN;
        } else if (fn == "quantile_cont" || fn == "quantile_disc" ||
                   fn == "quantile") {
          agg_spec.fn = fn == "quantile_cont"
                            ? PlanAggSpec::Fn::QUANTILE_CONT
                            : PlanAggSpec::Fn::QUANTILE_DISC;
          // The fraction argument is erased at bind time and stored in
          // QuantileBindData (public core_functions header — no layout
          // mirror needed, unlike string_agg's separator)
          if (!agg.bind_info) {
            unsupported(fn + " without bind data");
            return false;
          }
          const auto &qbd =
              agg.bind_info->Cast<duckdb::QuantileBindData>();
          if (qbd.quantiles.size() != 1) {
            unsupported(fn + " with a fraction LIST (single fraction only)");
            return false;
          }
          agg_spec.quantile = qbd.quantiles[0].dbl;
        } else if (fn == "mode") {
          agg_spec.fn = PlanAggSpec::Fn::MODE;
        } else if (fn == "mad") {
          agg_spec.fn = PlanAggSpec::Fn::MAD;
          // Temporal mad (DATE/TIMESTAMP → INTERVAL) needs interval
          // arithmetic we don't do — numeric only
          if (!agg.children.empty() &&
              !agg.children[0]->return_type.IsNumeric()) {
            unsupported("mad over " +
                        agg.children[0]->return_type.ToString());
            return false;
          }
        } else {
          unsupported("aggregate function " + fn);
          return false;
        }
        if ((agg_spec.fn == PlanAggSpec::Fn::MEDIAN ||
             agg_spec.fn == PlanAggSpec::Fn::QUANTILE_CONT ||
             agg_spec.fn == PlanAggSpec::Fn::QUANTILE_DISC ||
             agg_spec.fn == PlanAggSpec::Fn::MODE ||
             agg_spec.fn == PlanAggSpec::Fn::MAD) &&
            agg_spec.distinct) {
          unsupported("DISTINCT on " + fn);
          return false;
        }
        if (agg_spec.fn == PlanAggSpec::Fn::FIRST &&
            (agg_spec.distinct || agg.order_bys)) {
          // first() IS order/multiplicity sensitive — modifiers would
          // change its meaning
          unsupported("DISTINCT/ORDER BY on " + fn);
          return false;
        }
        // ORDER BY inside COUNT/SUM/AVG/MIN/MAX is semantically inert
        // (order-insensitive aggregates) — accept and ignore

        if (agg_spec.fn == PlanAggSpec::Fn::STRING_AGG ||
            agg_spec.fn == PlanAggSpec::Fn::ARRAY_AGG) {
          // Without an internal ORDER BY the result order is whatever
          // DuckDB's scan produced — unreproducible incrementally after
          // deletes/reinserts. Deterministic subset only.
          if (!agg.order_bys || agg.order_bys->orders.empty()) {
            unsupported(fn + " without ORDER BY inside the aggregate "
                             "(add e.g. " + fn + "(x ORDER BY x))");
            return false;
          }
          if (agg_spec.distinct) {
            unsupported("DISTINCT on " + fn);
            return false;
          }
          for (const auto &o : agg.order_bys->orders) {
            PlanAggSpec::OrderKey key;
            key.expr = o.expression.get();
            key.ascending = o.type != duckdb::OrderType::DESCENDING;
            key.nulls_first =
                o.null_order == duckdb::OrderByNullType::NULLS_FIRST;
            agg_spec.order_keys.push_back(key);
          }
          if (agg_spec.fn == PlanAggSpec::Fn::STRING_AGG &&
              agg.bind_info) {
            // DuckDB's bind erases the separator argument and stores it
            // in a TU-local StringAggBindData { string sep; }. The engine
            // is pinned (v1.5.4, built in-tree), so a layout mirror is
            // safe; sep is the sole member after the FunctionData base.
            struct SeparatorMirror : public duckdb::FunctionData {
              std::string sep;
              duckdb::unique_ptr<duckdb::FunctionData>
              Copy() const override {
                return nullptr;
              }
              bool Equals(const duckdb::FunctionData &) const override {
                return false;
              }
            };
            agg_spec.separator =
                reinterpret_cast<const SeparatorMirror *>(
                    agg.bind_info.get())
                    ->sep;
          }
          agg_spec.arg = agg.children[0].get();
          out.push_back(std::move(agg_spec));
          continue;
        }

        if (agg_spec.fn != PlanAggSpec::Fn::COUNT_STAR) {
          if (agg.children.size() != 1) {
            unsupported("aggregate with " +
                        std::to_string(agg.children.size()) +
                        " arguments (" + fn + ")");
            return false;
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
            case duckdb::LogicalTypeId::DECIMAL:
              if (agg_spec.fn == PlanAggSpec::Fn::SUM) {
                // SUM(DECIMAL) stays exact; AVG(DECIMAL) returns DOUBLE in
                // DuckDB, so the double path matches its semantics
                agg_spec.decimal_arg = true;
                agg_spec.decimal_scale =
                    duckdb::DecimalType::GetScale(agg_spec.arg->return_type);
              } else {
                agg_spec.integer_arg = false;
              }
              break;
            default:
              unsupported(fn + " over " +
                          agg_spec.arg->return_type.ToString());
              return false;
            }
          }
        }
        out.push_back(std::move(agg_spec));
      }
      return true;
    }

    // Correlated subqueries. DELIM_JOIN evaluates its right subplan with
    // DELIM_GET = the DISTINCT correlated-key values of the left side,
    // then joins back with null-safe (IS NOT DISTINCT FROM) conditions.
    // SINGLE (scalar subquery, inner is grouped by the key so at most one
    // match) maps onto the LEFT-join padding machinery; MARK (EXISTS) onto
    // the mark machinery.
    std::vector<std::vector<ColumnInfo>> delim_columns_stack;

    SpecPtr visit_delim_join(duckdb::LogicalComparisonJoin &op) {
      switch (op.join_type) {
      case duckdb::JoinType::SINGLE:
      case duckdb::JoinType::MARK:
      case duckdb::JoinType::INNER:
      case duckdb::JoinType::LEFT:
        break;
      default:
        return unsupported("DELIM join type " +
                           duckdb::EnumUtil::ToString(op.join_type));
      }
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::DELIM_JOIN;
      spec->join_type = op.join_type == duckdb::JoinType::SINGLE
                            ? duckdb::JoinType::LEFT
                            : op.join_type;
      for (const auto &cond : op.conditions) {
        PlanOpSpec::JoinCond jc{cond.left.get(), cond.right.get(),
                                cond.comparison};
        switch (cond.comparison) {
        case duckdb::ExpressionType::COMPARE_NOT_DISTINCT_FROM:
          spec->null_safe_keys = true;
          spec->equi_conds.push_back(jc);
          break;
        case duckdb::ExpressionType::COMPARE_EQUAL:
          spec->equi_conds.push_back(jc);
          break;
        default:
          return unsupported("DELIM join comparison " +
                             duckdb::EnumUtil::ToString(cond.comparison));
        }
      }

      auto left = visit(*op.children[0]);
      if (!left) {
        return nullptr;
      }
      std::vector<ColumnInfo> left_columns = columns;

      // Correlated (duplicate-eliminated) columns, evaluated over the left
      // output; DELIM_GET leaves in the right subplan read their DISTINCT
      std::vector<ColumnInfo> delim_cols;
      for (duckdb::idx_t i = 0; i < op.duplicate_eliminated_columns.size();
           i++) {
        const auto &e = op.duplicate_eliminated_columns[i];
        spec->exprs.push_back(e.get());
        delim_cols.push_back(
            {"delim_" + std::to_string(i), e->return_type});
      }
      if (spec->exprs.empty()) {
        return unsupported("DELIM join without correlated columns");
      }

      delim_columns_stack.push_back(delim_cols);
      auto right = visit(*op.children[1]);
      delim_columns_stack.pop_back();
      if (!right) {
        return nullptr;
      }

      spec->left_types = op.children[0]->types;
      spec->right_types = op.children[1]->types;
      spec->children.push_back(std::move(left));
      spec->children.push_back(std::move(right));

      if (op.join_type == duckdb::JoinType::MARK) {
        columns = std::move(left_columns);
        columns.push_back({"mark", duckdb::LogicalType::BOOLEAN});
      } else {
        std::vector<ColumnInfo> combined = std::move(left_columns);
        combined.insert(combined.end(), columns.begin(), columns.end());
        columns = std::move(combined);
      }
      return spec;
    }

    SpecPtr visit_delim_get(duckdb::LogicalDelimGet &op) {
      if (delim_columns_stack.empty()) {
        return unsupported("DELIM_GET outside a DELIM join");
      }
      (void)op;
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::DELIM_REF;
      columns = delim_columns_stack.back();
      return spec;
    }

    SpecPtr visit_join(duckdb::LogicalComparisonJoin &op) {
      switch (op.join_type) {
      case duckdb::JoinType::INNER:
      case duckdb::JoinType::LEFT:
      case duckdb::JoinType::RIGHT:
      case duckdb::JoinType::OUTER:
      case duckdb::JoinType::MARK:
        break;
      default:
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
      spec->join_type = op.join_type;
      for (const auto &cond : op.conditions) {
        PlanOpSpec::JoinCond jc{cond.left.get(), cond.right.get(),
                                cond.comparison};
        switch (cond.comparison) {
        case duckdb::ExpressionType::COMPARE_NOT_DISTINCT_FROM:
          // Null-safe equality (NULL matches NULL) — emitted by subquery
          // decorrelation; DuckDBRow key equality is already null-safe
          spec->null_safe_keys = true;
          spec->equi_conds.push_back(jc);
          break;
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
      if (spec->null_safe_keys) {
        // null_safe applies to ALL equi keys of the node; mixing = and
        // IS NOT DISTINCT FROM in one join would null-match the = key too
        for (const auto &cond : op.conditions) {
          if (cond.comparison == duckdb::ExpressionType::COMPARE_EQUAL) {
            return unsupported(
                "mixed null-safe and plain equality join keys");
          }
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

      if (op.join_type == duckdb::JoinType::MARK) {
        // MARK output: left columns plus one three-valued match column
        columns = std::move(left_columns);
        columns.push_back({"mark", duckdb::LogicalType::BOOLEAN});
        return spec;
      }
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
      if (op.distinct_type == duckdb::DistinctType::DISTINCT_ON) {
        return visit_distinct_on(op);
      }
      if (op.distinct_type != duckdb::DistinctType::DISTINCT) {
        return unsupported("DISTINCT variant");
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

    SpecPtr visit_distinct_on(duckdb::LogicalDistinct &op) {
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::DISTINCT_ON;
      for (const auto &target : op.distinct_targets) {
        if (target->GetExpressionClass() !=
            duckdb::ExpressionClass::BOUND_REF) {
          return unsupported("DISTINCT ON computed expression "
                             "(use a plain column)");
        }
        spec->column_idxs.push_back(
            target->Cast<duckdb::BoundReferenceExpression>().index);
      }
      // Winner-pick order (which row survives per key) rides on the DISTINCT
      // node itself; the ORDER_BY operator above is presentation only
      if (op.order_by) {
        for (const auto &o : op.order_by->orders) {
          if (o.expression->GetExpressionClass() !=
              duckdb::ExpressionClass::BOUND_REF) {
            return unsupported("DISTINCT ON ORDER BY expression "
                               "(use a plain column)");
          }
          auto &ref = o.expression->Cast<duckdb::BoundReferenceExpression>();
          NativeSortView::SortColumn sc;
          sc.column_idx = static_cast<size_t>(ref.index);
          sc.ascending = o.type != duckdb::OrderType::DESCENDING;
          sc.nulls_first =
              o.null_order == duckdb::OrderByNullType::NULLS_FIRST;
          spec->sort_columns.push_back(sc);
        }
      }
      auto child = visit(*op.children[0]);
      if (!child) {
        return nullptr;
      }
      spec->children.push_back(std::move(child));
      // Output keeps the full child row layout; columns stay as-is
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
      const std::vector<ColumnInfo> original_cols = columns;
      spec->window_source_cols = columns;

      // M1: PARTITION BY / ORDER BY / argument expressions that are not
      // plain column refs get projected into helper columns below the
      // window (a MAP_EXPR computing [child cols..., expr...]) and
      // stripped again above it — the user-visible layout is unchanged.
      const size_t ncols = op.children[0]->types.size();
      std::vector<const duckdb::Expression *> helper_exprs;
      auto resolve_col = [&](const duckdb::Expression &e) -> int {
        int idx = column_ref(e);
        if (idx >= 0) {
          return idx;
        }
        const std::string repr = e.ToString();
        for (size_t k = 0; k < helper_exprs.size(); k++) {
          if (helper_exprs[k]->ToString() == repr) {
            return static_cast<int>(ncols + k);
          }
        }
        helper_exprs.push_back(&e);
        return static_cast<int>(ncols + helper_exprs.size() - 1);
      };

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
          def.partition_indices.push_back(
              static_cast<size_t>(resolve_col(*p)));
        }
        for (const auto &o : w.orders) {
          NativeSortView::SortColumn sc;
          sc.column_idx = static_cast<size_t>(resolve_col(*o.expression));
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
          def.arg_column_idx = resolve_col(*w.children[0]);
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
          def.arg_column_idx = resolve_col(*w.children[0]);
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
            def.arg_column_idx = resolve_col(*w.children[0]);
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
      const size_t num_windows = op.expressions.size();
      std::vector<ColumnInfo> window_cols;
      for (duckdb::idx_t i = 0; i < num_windows; i++) {
        window_cols.push_back(
            {op.expressions[i]->GetName(), op.types[ncols + i]});
      }

      if (helper_exprs.empty()) {
        columns = original_cols;
        columns.insert(columns.end(), window_cols.begin(),
                       window_cols.end());
        spec->window_result_cols = columns;
        spec->children.push_back(std::move(child));
        return spec;
      }

      // Sandwich: MAP (add helper cols) → WINDOW → MAP (strip them)
      const size_t nhelp = helper_exprs.size();
      auto pre_map = std::make_unique<PlanOpSpec>();
      pre_map->kind = PlanOpSpec::Kind::MAP_EXPR;
      pre_map->input_types = op.children[0]->types;
      std::vector<ColumnInfo> widened_cols = original_cols;
      duckdb::vector<duckdb::LogicalType> widened_types =
          op.children[0]->types;
      for (size_t c = 0; c < ncols; c++) {
        auto e = duckdb::make_uniq<duckdb::BoundReferenceExpression>(
            op.children[0]->types[c], c);
        pre_map->exprs.push_back(e.get());
        owned_exprs.push_back(std::move(e));
      }
      for (size_t k = 0; k < nhelp; k++) {
        pre_map->exprs.push_back(helper_exprs[k]);
        widened_cols.push_back({"__w_expr_" + std::to_string(k),
                                helper_exprs[k]->return_type});
        widened_types.push_back(helper_exprs[k]->return_type);
      }
      pre_map->children.push_back(std::move(child));

      spec->window_source_cols = widened_cols;
      std::vector<ColumnInfo> window_out_cols = widened_cols;
      window_out_cols.insert(window_out_cols.end(), window_cols.begin(),
                             window_cols.end());
      spec->window_result_cols = window_out_cols;
      spec->children.push_back(std::move(pre_map));

      auto post_map = std::make_unique<PlanOpSpec>();
      post_map->kind = PlanOpSpec::Kind::MAP_EXPR;
      post_map->input_types = widened_types;
      for (duckdb::idx_t i = 0; i < num_windows; i++) {
        post_map->input_types.push_back(op.types[ncols + i]);
      }
      for (size_t c = 0; c < ncols; c++) {
        auto e = duckdb::make_uniq<duckdb::BoundReferenceExpression>(
            op.children[0]->types[c], c);
        post_map->exprs.push_back(e.get());
        owned_exprs.push_back(std::move(e));
      }
      for (duckdb::idx_t i = 0; i < num_windows; i++) {
        auto e = duckdb::make_uniq<duckdb::BoundReferenceExpression>(
            op.types[ncols + i], ncols + nhelp + i);
        post_map->exprs.push_back(e.get());
        owned_exprs.push_back(std::move(e));
      }
      post_map->children.push_back(std::move(spec));

      columns = original_cols;
      columns.insert(columns.end(), window_cols.begin(), window_cols.end());
      return post_map;
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

    // WITH RECURSIVE: the step's self-reference becomes a SOURCE with a
    // sentinel table name routed inside the PlanRecursiveNode's inner view
    std::set<duckdb::idx_t> recursive_cte_indexes;

    static std::string rec_cte_sentinel(duckdb::idx_t index) {
      return "__rec_cte_" + std::to_string(index) + "__";
    }

    SpecPtr visit_recursive_cte(duckdb::LogicalRecursiveCTE &op) {
      if (!op.key_targets.empty()) {
        return unsupported("recursive CTE USING KEY");
      }
      recursive_cte_indexes.insert(op.table_index);
      auto anchor = visit(*op.children[0]);
      if (!anchor) {
        return nullptr;
      }
      // Output layout = anchor layout (ResolveTypes: types = children[0])
      auto anchor_cols = columns;
      cte_columns[op.table_index] = anchor_cols; // step's CTE_SCAN resolves
      auto step = visit(*op.children[1]);
      if (!step) {
        return nullptr;
      }
      columns = std::move(anchor_cols);
      auto spec = std::make_unique<PlanOpSpec>();
      spec->kind = PlanOpSpec::Kind::REC_CTE;
      spec->set_op = op.union_all ? PlanOpSpec::SetOp::UNION_ALL
                                  : PlanOpSpec::SetOp::UNION;
      spec->cte_index = op.table_index;
      spec->children.push_back(std::move(anchor));
      spec->children.push_back(std::move(step));
      return spec;
    }

    SpecPtr visit_cte_ref(duckdb::LogicalCTERef &op) {
      if (recursive_cte_indexes.count(op.cte_index)) {
        auto rec_it = cte_columns.find(op.cte_index);
        if (rec_it == cte_columns.end()) {
          return unsupported("self-reference outside its recursive CTE");
        }
        auto spec = std::make_unique<PlanOpSpec>();
        spec->kind = PlanOpSpec::Kind::SOURCE;
        spec->table = rec_cte_sentinel(op.cte_index);
        columns.clear();
        for (duckdb::idx_t i = 0; i < op.chunk_types.size(); i++) {
          std::string name = i < op.bound_columns.size()
                                 ? op.bound_columns[i]
                                 : rec_it->second[i].name;
          columns.push_back({name, op.chunk_types[i]});
        }
        return spec;
      }
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
      // Virtual columns (rowid) become NULL: CDC deltas have no stable row
      // ids. Plans only request them when nothing reads the value (e.g. the
      // binder projects rowid as the cheapest column for COUNT(*)).
      const auto &column_ids = op.GetColumnIds();
      std::vector<duckdb::idx_t> idxs;
      bool identity = column_ids.size() == op.returned_types.size();
      columns.clear();
      for (duckdb::idx_t i = 0; i < column_ids.size(); i++) {
        duckdb::idx_t col = column_ids[i].GetPrimaryIndex();
        if (col >= op.returned_types.size()) {
          // Out-of-range index: the MAP_COLS lambda emits NULL for it
          identity = false;
          idxs.push_back(col);
          columns.push_back({"rowid", op.types[i]});
          continue;
        }
        if (col != i) {
          identity = false;
        }
        idxs.push_back(col);
        columns.push_back({op.names[col], op.returned_types[col]});
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

// Circuit-backed materialized views (Phase A)
//
// These implement the NativeMaterializedView interface but execute through a
// dbsp::Circuit of generic nodes instead of hand-wired per-view delta logic.
// Views are migrated here one type at a time; the goal is for all execution
// to flow through the circuit IR so optimization and (later) automatic
// incrementalization operate on a single substrate.

#pragma once

#include "dbsp_circuit.hpp"
#include "dbsp_duckdb_types.hpp"

namespace dbsp_native {

// Shared plumbing for single-source circuit views:
// SourceNode -> (operator nodes built by subclass) -> SinkNode.
// Subclasses construct their middle nodes in the constructor via build(),
// passing the function that yields the last operator's output.
class SingleSourceCircuitView : public NativeMaterializedView {
public:
  using RowSource = dbsp::SourceNode<DuckDBRow, DuckDBRowHash>;
  using RowSink = dbsp::SinkNode<DuckDBRow, DuckDBRowHash>;
  using OutputFn = std::function<const DuckDBZSet &()>;

  SingleSourceCircuitView(const std::string &name, const std::string &sql,
                          const std::string &source_table,
                          const TableSchema &schema)
      : NativeMaterializedView(name, sql), source_table_(source_table),
        schema_(schema) {
    schema_.table_name = name;
    source_ = circuit_.add_node(
        std::make_unique<RowSource>(circuit_.next_node_id(), source_table_));
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    if (table_name != source_table_) {
      // Step with empty input so delta() reflects "no change"
      circuit_.step();
      return;
    }
    source_->push_borrowed(changes);
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
    return {source_table_};
  }

  void reset() override {
    circuit_.reset();
    version_ = 0;
  }

  // --- Circuit-state checkpointing (D3b) -------------------------------
  bool checkpointable() const override {
    bool ok = true;
    circuit_.for_each_node([&](const dbsp::Node &n) {
      if (n.state_kind() == dbsp::Node::StateKind::UNSUPPORTED) {
        ok = false;
      }
    });
    return ok;
  }

  bool serialize_circuit_state(
      std::vector<std::pair<uint64_t, std::vector<uint8_t>>> &out)
      const override {
    if (!checkpointable()) {
      return false;
    }
    bool ok = true;
    circuit_.for_each_node([&](const dbsp::Node &n) {
      if (n.state_kind() == dbsp::Node::StateKind::SERIALIZABLE) {
        std::vector<uint8_t> blob;
        n.serialize_state(blob);
        out.emplace_back(n.id(), std::move(blob));
      }
    });
    return ok;
  }

  bool restore_circuit_state(
      const std::unordered_map<uint64_t, std::vector<uint8_t>> &blobs)
      override {
    if (!checkpointable()) {
      return false;
    }
    bool ok = true;
    circuit_.for_each_node([&](dbsp::Node &n) {
      if (!ok || n.state_kind() != dbsp::Node::StateKind::SERIALIZABLE) {
        return;
      }
      auto it = blobs.find(n.id());
      if (it == blobs.end() ||
          !n.restore_state(it->second.data(), it->second.size())) {
        ok = false;
      }
    });
    return ok;
  }

protected:
  // Subclass calls this once after adding its operator nodes
  void finish(OutputFn last_output) {
    sink_ = circuit_.add_node(std::make_unique<RowSink>(
        circuit_.next_node_id(), std::move(last_output), name_ + "_sink"));
  }

  const DuckDBZSet &source_output() const { return source_->output(); }

  dbsp::Circuit circuit_;

private:
  std::string source_table_;
  TableSchema schema_;
  RowSource *source_ = nullptr;
  RowSink *sink_ = nullptr;
};

// SELECT * FROM table WHERE condition  (Source -> Filter -> Sink)
class CircuitFilterView : public SingleSourceCircuitView {
public:
  using PredicateFn = std::function<bool(const DuckDBRow &)>;
  using RowFilter = dbsp::FilterNode<DuckDBRow, DuckDBRowHash>;

  CircuitFilterView(const std::string &name, const std::string &sql,
                    const std::string &source_table, const TableSchema &schema,
                    PredicateFn predicate)
      : SingleSourceCircuitView(name, sql, source_table, schema) {
    auto *filter = circuit_.add_node(std::make_unique<RowFilter>(
        circuit_.next_node_id(),
        [this]() -> const DuckDBZSet & { return source_output(); },
        std::move(predicate), "filter"));
    finish([filter]() -> const DuckDBZSet & { return filter->output(); });
  }
};

// SELECT col1, col2 FROM table  (Source -> Map -> Sink)
class CircuitProjectView : public SingleSourceCircuitView {
public:
  using ProjectFn = std::function<DuckDBRow(const DuckDBRow &)>;
  using RowMap =
      dbsp::MapNode<DuckDBRow, DuckDBRow, DuckDBRowHash, DuckDBRowHash>;

  CircuitProjectView(const std::string &name, const std::string &sql,
                     const std::string &source_table,
                     const TableSchema &result_schema, ProjectFn project)
      : SingleSourceCircuitView(name, sql, source_table, result_schema) {
    auto *map = circuit_.add_node(std::make_unique<RowMap>(
        circuit_.next_node_id(),
        [this]() -> const DuckDBZSet & { return source_output(); },
        std::move(project), "project"));
    finish([map]() -> const DuckDBZSet & { return map->output(); });
  }
};

// SELECT col1, col2 FROM table WHERE cond  (Source -> Filter -> Map -> Sink)
class CircuitFilterProjectView : public SingleSourceCircuitView {
public:
  using PredicateFn = std::function<bool(const DuckDBRow &)>;
  using ProjectFn = std::function<DuckDBRow(const DuckDBRow &)>;
  using RowFilter = dbsp::FilterNode<DuckDBRow, DuckDBRowHash>;
  using RowMap =
      dbsp::MapNode<DuckDBRow, DuckDBRow, DuckDBRowHash, DuckDBRowHash>;

  CircuitFilterProjectView(const std::string &name, const std::string &sql,
                           const std::string &source_table,
                           const TableSchema &result_schema,
                           PredicateFn predicate, ProjectFn project)
      : SingleSourceCircuitView(name, sql, source_table, result_schema) {
    auto *filter = circuit_.add_node(std::make_unique<RowFilter>(
        circuit_.next_node_id(),
        [this]() -> const DuckDBZSet & { return source_output(); },
        std::move(predicate), "filter"));
    auto *map = circuit_.add_node(std::make_unique<RowMap>(
        circuit_.next_node_id(),
        [filter]() -> const DuckDBZSet & { return filter->output(); },
        std::move(project), "project"));
    finish([map]() -> const DuckDBZSet & { return map->output(); });
  }
};

// Aggregate operator node: SELECT key, AGG(val) FROM t GROUP BY key [HAVING]
// Emits per-step deltas (-1 old group row, +1 new group row); downstream sink
// integrates them into the result. State: per-group running sum/count plus a
// multiset of values for O(log n) MIN/MAX. Same incremental algorithm as
// NativeAggregateView, minus result maintenance (the sink owns the result).
class RowAggregateNode : public dbsp::Node {
public:
  using AggType = NativeAggregateView::AggType;
  using KeyFn = NativeAggregateView::KeyFn;
  using ValueFn = NativeAggregateView::ValueFn;
  using HavingPredicate = NativeAggregateView::HavingPredicate;
  using InputFn = std::function<const DuckDBZSet &()>;

  RowAggregateNode(dbsp::NodeId id, InputFn input_fn, KeyFn key_fn,
                   ValueFn value_fn, AggType agg_type,
                   HavingPredicate having_predicate,
                   std::string name = "aggregate")
      : dbsp::Node(id, std::move(name)), input_fn_(std::move(input_fn)),
        key_fn_(std::move(key_fn)), value_fn_(std::move(value_fn)),
        agg_type_(agg_type), having_predicate_(std::move(having_predicate)) {}

  void step() override {
    output_.clear();
    const DuckDBZSet &changes = input_fn_();
    if (changes.empty()) {
      return;
    }

    std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> delta_sums;
    std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> delta_counts;
    std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> delta_null_counts;
    std::unordered_map<DuckDBRow, std::vector<std::pair<int64_t, int64_t>>,
                       DuckDBRowHash>
        value_changes;

    for (const auto &[row, weight] : changes) {
      DuckDBRow key = key_fn_(row);
      duckdb::Value val = value_fn_(row);

      // SQL Standard: NULL values are ignored in aggregates (except COUNT(*))
      if (val.IsNull()) {
        delta_null_counts[key] += weight;
      } else {
        int64_t int_val = 0;
        if (val.type().IsNumeric()) {
          int_val = val.GetValue<int64_t>();
        }
        delta_sums[key] += int_val * weight;
        delta_counts[key] += weight;
        if (agg_type_ == AggType::MIN || agg_type_ == AggType::MAX) {
          value_changes[key].push_back({int_val, weight});
        }
      }
    }

    std::set<DuckDBRow> all_keys;
    for (const auto &[key, _] : delta_sums)
      all_keys.insert(key);
    for (const auto &[key, _] : delta_counts)
      all_keys.insert(key);
    for (const auto &[key, _] : delta_null_counts)
      all_keys.insert(key);

    for (const auto &key : all_keys) {
      auto &state = agg_states_[key];

      // Retract old group row (if it existed and passed HAVING)
      if (state.count > 0 || state.null_count > 0) {
        DuckDBRow old_result = make_result_row(key, compute_agg(state));
        if (!having_predicate_ || having_predicate_(old_result)) {
          output_.insert(old_result, -1);
        }
      }

      state.sum += delta_sums[key];
      state.count += delta_counts[key];
      state.null_count += delta_null_counts[key];

      if (agg_type_ == AggType::MIN || agg_type_ == AggType::MAX) {
        auto vc_it = value_changes.find(key);
        if (vc_it != value_changes.end()) {
          for (const auto &[val, w] : vc_it->second) {
            if (w > 0) {
              for (int64_t i = 0; i < w; i++) {
                state.values.insert(val);
              }
            } else if (w < 0) {
              for (int64_t i = 0; i < -w; i++) {
                auto it = state.values.find(val);
                if (it != state.values.end()) {
                  state.values.erase(it);
                }
              }
            }
          }
        }
      }

      // Emit new group row (if group survives and passes HAVING)
      if (state.count > 0 || state.null_count > 0) {
        DuckDBRow new_result = make_result_row(key, compute_agg(state));
        if (!having_predicate_ || having_predicate_(new_result)) {
          output_.insert(new_result, 1);
        }
      } else {
        agg_states_.erase(key);
      }
    }

    has_output_ = !output_.empty();
  }

  void reset() override {
    agg_states_.clear();
    output_.clear();
    has_output_ = false;
  }

  bool has_output() const override { return has_output_; }

  const DuckDBZSet &output() const { return output_; }

private:
  struct AggState {
    int64_t sum = 0;
    int64_t count = 0;             // Non-NULL count
    int64_t null_count = 0;        // NULL count (for COUNT(*))
    std::multiset<int64_t> values; // For MIN/MAX
  };

  duckdb::Value compute_agg(const AggState &state) const {
    switch (agg_type_) {
    case AggType::SUM:
      if (state.count == 0) {
        return duckdb::Value(duckdb::LogicalType::BIGINT); // NULL
      }
      return duckdb::Value::BIGINT(state.sum);
    case AggType::COUNT:
      return duckdb::Value::BIGINT(state.count);
    case AggType::AVG:
      if (state.count == 0) {
        return duckdb::Value(duckdb::LogicalType::BIGINT); // NULL
      }
      return duckdb::Value::BIGINT(state.sum / state.count);
    case AggType::MIN:
      if (state.values.empty()) {
        return duckdb::Value(duckdb::LogicalType::BIGINT); // NULL
      }
      return duckdb::Value::BIGINT(*state.values.begin());
    case AggType::MAX:
      if (state.values.empty()) {
        return duckdb::Value(duckdb::LogicalType::BIGINT); // NULL
      }
      return duckdb::Value::BIGINT(*state.values.rbegin());
    }
    return duckdb::Value::BIGINT(state.sum);
  }

  DuckDBRow make_result_row(const DuckDBRow &key,
                            const duckdb::Value &agg_val) const {
    DuckDBRow result;
    result.columns = key.columns;
    result.columns.push_back(agg_val);
    return result;
  }

  InputFn input_fn_;
  KeyFn key_fn_;
  ValueFn value_fn_;
  AggType agg_type_;
  HavingPredicate having_predicate_;
  std::unordered_map<DuckDBRow, AggState, DuckDBRowHash> agg_states_;
  DuckDBZSet output_;
  bool has_output_ = false;
};

// SELECT key, AGG(val) FROM t GROUP BY key  (Source -> Aggregate -> Sink)
class CircuitAggregateView : public SingleSourceCircuitView {
public:
  using AggType = NativeAggregateView::AggType;

  CircuitAggregateView(const std::string &name, const std::string &sql,
                       const std::string &source_table,
                       const TableSchema &result_schema,
                       NativeAggregateView::KeyFn key_fn,
                       NativeAggregateView::ValueFn value_fn, AggType agg_type,
                       NativeAggregateView::HavingPredicate having = nullptr)
      : SingleSourceCircuitView(name, sql, source_table, result_schema) {
    auto *agg = circuit_.add_node(std::make_unique<RowAggregateNode>(
        circuit_.next_node_id(),
        [this]() -> const DuckDBZSet & { return source_output(); },
        std::move(key_fn), std::move(value_fn), agg_type, std::move(having)));
    finish([agg]() -> const DuckDBZSet & { return agg->output(); });
  }
};

// Adapter: runs an existing NativeMaterializedView as a single opaque circuit
// node. Used for view types not yet decomposed into fine-grained operator
// nodes (join, sort, window, ...). The node owns the view's state; result and
// delta stay inside the wrapped view (no duplicate integration in a sink).
class WrappedViewNode : public dbsp::Node {
public:
  WrappedViewNode(dbsp::NodeId id,
                  std::unique_ptr<NativeMaterializedView> view)
      : dbsp::Node(id, view->name() + "_op"), view_(std::move(view)) {}

  void stage(const std::string &table_name, const DuckDBZSet &changes) {
    staged_.emplace_back(table_name, changes);
  }

  void step() override {
    if (staged_.empty()) {
      // Some Native views early-return before clearing delta_ on a table
      // mismatch, so a stale delta can linger; applied_ gates get_delta.
      applied_ = false;
      return;
    }
    for (auto &[table, changes] : staged_) {
      view_->apply_changes(table, changes);
    }
    staged_.clear();
    applied_ = true;
  }

  void reset() override {
    view_->reset();
    staged_.clear();
    applied_ = false;
  }

  bool has_output() const override {
    return applied_ && !view_->get_delta().empty();
  }

  bool applied() const { return applied_; }
  NativeMaterializedView &view() { return *view_; }
  const NativeMaterializedView &view() const { return *view_; }

private:
  std::unique_ptr<NativeMaterializedView> view_;
  std::vector<std::pair<std::string, DuckDBZSet>> staged_;
  bool applied_ = false;
};

// Circuit facade over a wrapped view. Presents the same interface but all
// execution flows through Circuit::step, so these views participate in the
// same substrate as the fine-grained circuit views above.
class CircuitWrappedView : public NativeMaterializedView {
public:
  explicit CircuitWrappedView(std::unique_ptr<NativeMaterializedView> inner)
      : NativeMaterializedView(inner->name(), inner->sql()) {
    node_ = circuit_.add_node(std::make_unique<WrappedViewNode>(
        circuit_.next_node_id(), std::move(inner)));
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    node_->stage(table_name, changes);
    circuit_.step();
    ++version_;
  }

  const DuckDBZSet &get_result() const override {
    return node_->view().get_result();
  }

  void set_result(const DuckDBZSet &result) override {
    node_->view().set_result(result);
    version_++;
  }

  const DuckDBZSet &get_delta() const override {
    return node_->applied() ? node_->view().get_delta() : empty_delta_;
  }

  const TableSchema &result_schema() const override {
    return node_->view().result_schema();
  }

  std::vector<std::string> source_tables() const override {
    return node_->view().source_tables();
  }

  void reset() override {
    circuit_.reset();
    version_ = 0;
  }

private:
  dbsp::Circuit circuit_;
  WrappedViewNode *node_ = nullptr;
  DuckDBZSet empty_delta_;
};

// Wrap a factory-produced view in a circuit facade (nullptr passes through)
inline std::unique_ptr<NativeMaterializedView>
wrap_in_circuit(std::unique_ptr<NativeMaterializedView> view) {
  if (!view) {
    return nullptr;
  }
  return std::make_unique<CircuitWrappedView>(std::move(view));
}

} // namespace dbsp_native

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

// Filter view executed as a circuit: Source -> Filter -> Sink
// Equivalent to NativeFilterView; SELECT * FROM table WHERE condition.
class CircuitFilterView : public NativeMaterializedView {
public:
  using PredicateFn = std::function<bool(const DuckDBRow &)>;
  using RowSource = dbsp::SourceNode<DuckDBRow, DuckDBRowHash>;
  using RowFilter = dbsp::FilterNode<DuckDBRow, DuckDBRowHash>;
  using RowSink = dbsp::SinkNode<DuckDBRow, DuckDBRowHash>;

  CircuitFilterView(const std::string &name, const std::string &sql,
                    const std::string &source_table, const TableSchema &schema,
                    PredicateFn predicate)
      : NativeMaterializedView(name, sql), source_table_(source_table),
        schema_(schema) {
    schema_.table_name = name;

    source_ = circuit_.add_node(std::make_unique<RowSource>(
        circuit_.next_node_id(), source_table_));
    auto *filter = circuit_.add_node(std::make_unique<RowFilter>(
        circuit_.next_node_id(), [this]() -> const DuckDBZSet & {
          return source_->output();
        },
        std::move(predicate), "filter"));
    sink_ = circuit_.add_node(std::make_unique<RowSink>(
        circuit_.next_node_id(), [filter]() -> const DuckDBZSet & {
          return filter->output();
        },
        name + "_sink"));
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    if (table_name != source_table_) {
      // Still step with empty input so delta() reflects "no change"
      circuit_.step();
      return;
    }
    source_->push(changes);
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

private:
  std::string source_table_;
  TableSchema schema_;
  dbsp::Circuit circuit_;
  RowSource *source_ = nullptr;
  RowSink *sink_ = nullptr;
};

} // namespace dbsp_native

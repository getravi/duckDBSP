// DBSP Set Operations Implementation
// Supports UNION, INTERSECT, EXCEPT (ALL and DISTINCT)

#pragma once

#include "dbsp_duckdb_types.hpp"
#include "duckdb/common/enums/set_operation_type.hpp"
#include <algorithm>
#include <set>

namespace dbsp_native {

// NativeSetView: Combines two streams using set operations
class NativeSetView : public NativeMaterializedView {
public:
  NativeSetView(const std::string &name, const std::string &sql,
                std::unique_ptr<NativeMaterializedView> left_view,
                std::unique_ptr<NativeMaterializedView> right_view,
                const TableSchema &schema, duckdb::SetOperationType type,
                bool all)
      : NativeMaterializedView(name, sql), left_view_(std::move(left_view)),
        right_view_(std::move(right_view)), schema_(schema), type_(type),
        all_(all) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    delta_.clear();

    // 1. Propagate changes to children
    left_view_->apply_changes(table_name, changes);
    right_view_->apply_changes(table_name, changes);

    // 2. Use deltas from children to update our state
    // We need to process each unique row that changed in either child
    std::set<DuckDBRow> changed_rows;
    for (const auto &[row, weight] : left_view_->get_delta()) {
      changed_rows.insert(row);
    }
    for (const auto &[row, weight] : right_view_->get_delta()) {
      changed_rows.insert(row);
    }

    for (const auto &row : changed_rows) {
      // For each changed row, we need the PREVIOUS and CURRENT weights from
      // children previous = current - delta
      Weight new_l = left_view_->get_result().get(row);
      Weight delta_l = left_view_->get_delta().get(row);
      Weight old_l = new_l - delta_l;

      Weight new_r = right_view_->get_result().get(row);
      Weight delta_r = right_view_->get_delta().get(row);
      Weight old_r = new_r - delta_r;

      apply_op_delta(row, old_l, old_r, new_l, new_r);
    }

    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  void set_result(const DuckDBZSet &result) override { result_ = result; version_++; }
  const DuckDBZSet &get_delta() const override { return delta_; }
  const TableSchema &result_schema() const override { return schema_; }

  std::vector<std::string> source_tables() const override {
    std::set<std::string> sources;
    auto left_sources = left_view_->source_tables();
    sources.insert(left_sources.begin(), left_sources.end());
    auto right_sources = right_view_->source_tables();
    sources.insert(right_sources.begin(), right_sources.end());
    return std::vector<std::string>(sources.begin(), sources.end());
  }

  void reset() override {
    left_view_->reset();
    right_view_->reset();
    result_.clear();
    delta_.clear();
    version_ = 0;
  }

private:
  // Apply the set operation delta to the result
  void apply_op_delta(const DuckDBRow &row, Weight old_l, Weight old_r,
                      Weight new_l, Weight new_r) {
    Weight old_out = compute_out(old_l, old_r);
    Weight new_out = compute_out(new_l, new_r);

    Weight d = new_out - old_out;
    if (d != 0) {
      result_.insert(row, d);
      delta_.insert(row, d);
    }
  }

  // Compute the output weight for a single row based on input weights
  Weight compute_out(Weight l, Weight r) const {
    switch (type_) {
    case duckdb::SetOperationType::UNION:
      if (all_) {
        // UNION ALL: A(x) + B(x)
        return l + r;
      } else {
        // UNION: distinct(A(x) + B(x))
        return (l + r > 0) ? 1 : 0;
      }
    case duckdb::SetOperationType::INTERSECT:
      if (all_) {
        // INTERSECT ALL: min(A(x), B(x))
        return std::min(std::max((Weight)0, l), std::max((Weight)0, r));
      } else {
        // INTERSECT: distinct(A(x)) \cap distinct(B(x))
        return (l > 0 && r > 0) ? 1 : 0;
      }
    case duckdb::SetOperationType::EXCEPT:
      if (all_) {
        // EXCEPT ALL: max(0, A(x) - B(x))
        return std::max((Weight)0, l - r);
      } else {
        // EXCEPT: distinct(A(x)) \setminus distinct(B(x))
        return (l > 0 && r <= 0) ? 1 : 0;
      }
    default:
      return 0;
    }
  }

  std::unique_ptr<NativeMaterializedView> left_view_;
  std::unique_ptr<NativeMaterializedView> right_view_;
  TableSchema schema_;
  duckdb::SetOperationType type_;
  bool all_;

  DuckDBZSet result_;
  DuckDBZSet delta_;
};

} // namespace dbsp_native

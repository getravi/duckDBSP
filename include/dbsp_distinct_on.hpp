// Native DISTINCT ON View
// maintains partitions and outputs the first row for each partition
// optionally uses ORDER BY criteria within partitions

#pragma once

#include "dbsp_duckdb_types.hpp"
#include <map>
#include <set>

namespace dbsp_native {

class NativeDistinctOnView : public NativeMaterializedView {
public:
  struct SortColumn {
    size_t column_idx;
    bool ascending;
    bool nulls_first;
  };

  NativeDistinctOnView(const std::string &name, const std::string &sql,
                       const std::string &source_table,
                       const TableSchema &result_schema,
                       std::vector<size_t> partition_keys,
                       std::vector<SortColumn> sort_columns)
      : NativeMaterializedView(name, sql), source_table_(source_table),
        schema_(result_schema), partition_keys_(std::move(partition_keys)),
        sort_columns_(std::move(sort_columns)), comparator_(sort_columns_) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    delta_.clear();
    if (table_name != source_table_)
      return;

    for (const auto &[row, weight] : changes) {
      DuckDBRow partition_key = extract_partition_key(row);
      auto &partition = partitions_[partition_key];

      // Store old first row for incremental delta calculation
      std::optional<DuckDBRow> old_first;
      if (!partition.rows.empty()) {
        old_first = *partition.rows.begin();
      }

      // Update partition rows
      if (weight > 0) {
        for (Weight i = 0; i < weight; ++i) {
          partition.rows.insert(row);
        }
      } else if (weight < 0) {
        for (Weight i = 0; i > weight; --i) {
          auto it = partition.rows.find(row);
          if (it != partition.rows.end()) {
            partition.rows.erase(it);
          }
        }
      }

      // Identify new first row
      std::optional<DuckDBRow> new_first;
      if (!partition.rows.empty()) {
        new_first = *partition.rows.begin();
      }

      // Propagate changes if the first row changed
      bool changed = false;
      if (old_first.has_value() != new_first.has_value()) {
        changed = true;
      } else if (old_first.has_value() && new_first.has_value()) {
        if (old_first.value() != new_first.value()) {
          changed = true;
        }
      }

      if (changed) {
        if (old_first.has_value()) {
          result_.insert(old_first.value(), -1);
          delta_.insert(old_first.value(), -1);
        }
        if (new_first.has_value()) {
          result_.insert(new_first.value(), 1);
          delta_.insert(new_first.value(), 1);
        }
      }

      if (partition.rows.empty()) {
        partitions_.erase(partition_key);
      }
    }
    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  const DuckDBZSet &get_delta() const override { return delta_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    partitions_.clear();
    result_.clear();
    delta_.clear();
    version_ = 0;
  }

private:
  struct RowComparator {
    const std::vector<SortColumn> &cols;

    explicit RowComparator(const std::vector<SortColumn> &c) : cols(c) {}

    bool operator()(const DuckDBRow &a, const DuckDBRow &b) const {
      for (const auto &col : cols) {
        if (col.column_idx >= a.columns.size() ||
            col.column_idx >= b.columns.size())
          continue;

        const auto &val_a = a.columns[col.column_idx];
        const auto &val_b = b.columns[col.column_idx];

        if (val_a == val_b)
          continue;

        // Handle NULLs
        bool a_null = val_a.IsNull();
        bool b_null = val_b.IsNull();

        if (a_null && !b_null)
          return col.nulls_first;
        if (!a_null && b_null)
          return !col.nulls_first;
        if (a_null && b_null)
          continue;

        bool less = val_a < val_b;
        return col.ascending ? less : !less;
      }
      // Tie-breaker for stable sorting in partitions
      return a < b;
    }
  };

  struct PartitionState {
    std::multiset<DuckDBRow, RowComparator> rows;
    explicit PartitionState(const RowComparator &comp) : rows(comp) {}
    PartitionState() : rows(RowComparator({})) {} // Should not be used directly
  };

  DuckDBRow extract_partition_key(const DuckDBRow &row) const {
    DuckDBRow key;
    for (size_t idx : partition_keys_) {
      if (idx < row.columns.size()) {
        key.columns.push_back(row.columns[idx]);
      } else {
        key.columns.push_back(duckdb::Value());
      }
    }
    return key;
  }

  std::string source_table_;
  TableSchema schema_;
  std::vector<size_t> partition_keys_;
  std::vector<SortColumn> sort_columns_;
  RowComparator comparator_;

  // Custom map to handle PartitionState initialization with comparator
  struct PartitionMap {
    RowComparator comp;
    std::map<DuckDBRow, PartitionState> data;

    explicit PartitionMap(const RowComparator &c) : comp(c) {}

    PartitionState &operator[](const DuckDBRow &key) {
      auto it = data.find(key);
      if (it == data.end()) {
        return data
            .emplace(std::piecewise_construct, std::forward_as_tuple(key),
                     std::forward_as_tuple(comp))
            .first->second;
      }
      return it->second;
    }

    void erase(const DuckDBRow &key) { data.erase(key); }
    void clear() { data.clear(); }
  };

  PartitionMap partitions_{RowComparator(sort_columns_)};
  DuckDBZSet result_;
  DuckDBZSet delta_;
};

} // namespace dbsp_native

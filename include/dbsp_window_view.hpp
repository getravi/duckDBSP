#pragma once

#include "dbsp_duckdb_types.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include <map>
#include <set>
#include <vector>

namespace dbsp_native {

class NativeWindowView : public NativeMaterializedView {
public:
  struct WindowDef {
    std::string function;
    std::string alias;
    std::vector<size_t> partition_indices;
    std::vector<NativeSortView::SortColumn> sort_columns;
    int arg_column_idx = -1;
    duckdb::WindowBoundary start = duckdb::WindowBoundary::UNBOUNDED_PRECEDING;
    duckdb::WindowBoundary end = duckdb::WindowBoundary::CURRENT_ROW_ROWS;
    int offset = 1;       // For LAG/LEAD
    int start_offset = 0; // For ROWS BETWEEN N PRECEDING
    int end_offset = 0;   // For ROWS BETWEEN M FOLLOWING
  };

private:
  std::vector<WindowDef> windows_;
  TableSchema source_schema_;
  std::string source_table_;

  // Storage: Partition Key -> Sorted Rows
  // Partition Key is a vector of Values
  using PartitionKey = std::vector<duckdb::Value>;

  struct RowComparator {
    std::vector<NativeSortView::SortColumn> sort_columns;

    bool operator()(const DuckDBRow &a, const DuckDBRow &b) const {
      for (const auto &col : sort_columns) {
        // Bounds check
        if (col.column_idx >= a.columns.size() ||
            col.column_idx >= b.columns.size())
          continue;

        const auto &val_a = a.columns[col.column_idx];
        const auto &val_b = b.columns[col.column_idx];

        bool a_null = val_a.IsNull();
        bool b_null = val_b.IsNull();

        if (a_null && b_null)
          continue;
        if (a_null && !b_null)
          return col.nulls_first;
        if (!a_null && b_null)
          return !col.nulls_first;

        if (val_a == val_b)
          continue;

        bool less = val_a < val_b;
        if (col.ascending) {
          return less;
        } else {
          return !less;
        }
      }
      return false; // Equal
    }
  };

  struct PartitionState {
    std::multiset<DuckDBRow, RowComparator> sorted_rows;

    PartitionState(const std::vector<NativeSortView::SortColumn> &c)
        : sorted_rows(RowComparator{c}) {}
  };

  std::map<PartitionKey, PartitionState> partitions_;
  std::vector<NativeSortView::SortColumn> primary_sort_columns_;
  std::map<PartitionKey, DuckDBZSet> partition_outputs_;

  // Output state
  DuckDBZSet result_;
  DuckDBZSet delta_;
  TableSchema result_schema_;

public:
  NativeWindowView(std::string view_name, std::string sql,
                   std::string source_table, TableSchema result_schema,
                   TableSchema source_schema, std::vector<WindowDef> windows)
      : NativeMaterializedView(view_name, sql), windows_(windows),
        source_schema_(source_schema), source_table_(source_table),
        result_schema_(result_schema) {

    if (!windows_.empty()) {
      primary_sort_columns_ = windows_[0].sort_columns;
    }
  }

  // Implement required abstract methods
  const DuckDBZSet &get_result() const override { return result_; }
  const DuckDBZSet &get_delta() const override { return delta_; }
  const TableSchema &result_schema() const override { return result_schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    partitions_.clear();
    partition_outputs_.clear();
    result_.clear();
    delta_.clear();
    version_ = 0;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    delta_.clear();
    if (table_name != source_table_)
      return;

    // 1. Identify affected partitions
    std::set<PartitionKey> affected_partitions;

    for (const auto &[row, weight] : changes) {
      PartitionKey key;
      if (!windows_.empty()) {
        for (size_t idx : windows_[0].partition_indices) {
          if (idx < row.columns.size())
            key.push_back(row.columns[idx]);
        }
      }

      // Find or create partition
      auto it = partitions_.find(key);
      if (it == partitions_.end()) {
        it = partitions_.emplace(key, PartitionState(primary_sort_columns_))
                 .first;
      }

      // Apply change to state
      if (weight > 0) {
        for (Weight i = 0; i < weight; ++i)
          it->second.sorted_rows.insert(row);
      } else {
        for (Weight i = 0; i > weight; --i) {
          auto row_it = it->second.sorted_rows.find(row);
          if (row_it != it->second.sorted_rows.end()) {
            it->second.sorted_rows.erase(row_it);
          }
        }
      }
      affected_partitions.insert(key);
    }

    // 2. Recompute window functions for affected partitions
    for (const auto &key : affected_partitions) {
      // Retract old output for this partition
      auto &old_out = partition_outputs_[key];
      for (const auto &[row, w] : old_out) {
        if (w > 0) {
          delta_.insert(row, -w);
          result_.insert(row, -w);
        }
      }
      old_out.clear();

      // Compute new output
      auto it = partitions_.find(key);
      if (it == partitions_.end())
        continue;

      auto &state = it->second;
      if (state.sorted_rows.empty()) {
        partitions_.erase(it);
        continue;
      }

      // Convert multiset to vector for random access
      std::vector<DuckDBRow> partition_rows;
      for (const auto &row : state.sorted_rows) {
        partition_rows.push_back(row);
      }

      // Pre-calculate peer boundaries for RANGE/GROUPS
      std::vector<size_t> peer_start(partition_rows.size());
      std::vector<size_t> peer_end(partition_rows.size());

      if (!partition_rows.empty()) {
        size_t p_start = 0;

        for (size_t i = 1; i <= partition_rows.size(); ++i) {
          bool same = false;
          if (i < partition_rows.size()) {
            same = true;
            for (const auto &col : primary_sort_columns_) {
              if (partition_rows[i].columns[col.column_idx] !=
                  partition_rows[i - 1].columns[col.column_idx]) {
                same = false;
                break;
              }
            }
          }

          if (!same) {
            for (size_t j = p_start; j < i; ++j) {
              peer_start[j] = p_start;
              peer_end[j] = i - 1;
            }
            p_start = i;
          }
        }
      }

      int64_t row_number = 1;
      int64_t rank = 1;
      int64_t dense_rank = 1;

      for (size_t row_idx = 0; row_idx < partition_rows.size(); ++row_idx) {
        const auto &row = partition_rows[row_idx];
        DuckDBRow out_row = row; // Start with source row columns

        // Use pre-calculated peer boundaries for RANK/DENSE_RANK
        if (row_idx == 0 || peer_start[row_idx] == row_idx) {
          rank = row_idx + 1;
          dense_rank = (row_idx == 0) ? 1 : dense_rank + 1;
        }
        row_number = row_idx + 1;

        int64_t current_row_number = row_number;
        int64_t current_rank = rank;
        int64_t current_dense_rank = dense_rank;

        // Append window columns
        for (size_t i = 0; i < windows_.size(); ++i) {
          const auto &win = windows_[i];

          if (win.function == "ROW_NUMBER") {
            out_row.columns.push_back(duckdb::Value(current_row_number));
          } else if (win.function == "RANK") {
            out_row.columns.push_back(duckdb::Value(current_rank));
          } else if (win.function == "DENSE_RANK") {
            out_row.columns.push_back(duckdb::Value(current_dense_rank));
          } else if (win.function == "LAG") {
            int offset = win.offset;
            if (row_idx >= (size_t)offset && win.arg_column_idx >= 0) {
              out_row.columns.push_back(
                  partition_rows[row_idx - offset].columns[win.arg_column_idx]);
            } else {
              out_row.columns.push_back(duckdb::Value());
            }
          } else if (win.function == "LEAD") {
            int offset = win.offset;
            if (row_idx + offset < partition_rows.size() &&
                win.arg_column_idx >= 0) {
              out_row.columns.push_back(
                  partition_rows[row_idx + offset].columns[win.arg_column_idx]);
            } else {
              out_row.columns.push_back(duckdb::Value());
            }
          } else if (win.function == "FIRST_VALUE") {
            if (!partition_rows.empty() && win.arg_column_idx >= 0 &&
                (size_t)win.arg_column_idx < partition_rows[0].columns.size()) {
              out_row.columns.push_back(
                  partition_rows[0].columns[win.arg_column_idx]);
            } else {
              out_row.columns.push_back(duckdb::Value());
            }
          } else if (win.function == "LAST_VALUE") {
            if (!partition_rows.empty() && win.arg_column_idx >= 0) {
              // LAST_VALUE uses the frame end (default: CURRENT ROW)
              size_t frame_end_idx = row_idx; // default CURRENT_ROW
              if (win.end == duckdb::WindowBoundary::UNBOUNDED_FOLLOWING) {
                frame_end_idx = partition_rows.size() - 1;
              } else if (win.end ==
                         duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS) {
                frame_end_idx = std::min(row_idx + (size_t)win.end_offset,
                                         partition_rows.size() - 1);
              }
              if ((size_t)win.arg_column_idx <
                  partition_rows[frame_end_idx].columns.size()) {
                out_row.columns.push_back(
                    partition_rows[frame_end_idx].columns[win.arg_column_idx]);
              } else {
                out_row.columns.push_back(duckdb::Value());
              }
            } else {
              out_row.columns.push_back(duckdb::Value());
            }
          } else if (win.function == "NTH_VALUE") {
            int n = win.offset; // NTH_VALUE uses offset as N
            if (n > 0 && (size_t)(n - 1) < partition_rows.size() &&
                win.arg_column_idx >= 0 &&
                (size_t)win.arg_column_idx <
                    partition_rows[n - 1].columns.size()) {
              out_row.columns.push_back(
                  partition_rows[n - 1].columns[win.arg_column_idx]);
            } else {
              out_row.columns.push_back(duckdb::Value());
            }
          } else if (win.function == "NTILE") {
            int num_buckets = win.offset; // NTILE(N)
            if (num_buckets <= 0)
              num_buckets = 1;
            int64_t total = partition_rows.size();
            int64_t bucket = ((int64_t)row_idx * num_buckets) / total + 1;
            out_row.columns.push_back(duckdb::Value(bucket));
          } else {
            // Aggregates: Evaluated over frame
            // Frame start
            size_t frame_start = 0;

            switch (win.start) {
            case duckdb::WindowBoundary::UNBOUNDED_PRECEDING:
              frame_start = 0;
              break;
            case duckdb::WindowBoundary::CURRENT_ROW_ROWS:
              frame_start = row_idx;
              break;
            case duckdb::WindowBoundary::CURRENT_ROW_RANGE:
            case duckdb::WindowBoundary::CURRENT_ROW_GROUPS:
              frame_start = peer_start[row_idx];
              break;
            case duckdb::WindowBoundary::EXPR_PRECEDING_ROWS:
              frame_start = row_idx >= (size_t)win.start_offset
                                ? row_idx - win.start_offset
                                : 0;
              break;
            case duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS:
              frame_start = std::min(row_idx + win.start_offset,
                                     partition_rows.size() - 1);
              break;
            default:
              frame_start = 0;
            }

            // Frame end
            size_t frame_end = partition_rows.size() - 1;
            switch (win.end) {
            case duckdb::WindowBoundary::UNBOUNDED_FOLLOWING:
              frame_end = partition_rows.size() - 1;
              break;
            case duckdb::WindowBoundary::CURRENT_ROW_ROWS:
              frame_end = row_idx;
              break;
            case duckdb::WindowBoundary::CURRENT_ROW_RANGE:
            case duckdb::WindowBoundary::CURRENT_ROW_GROUPS:
              frame_end = peer_end[row_idx];
              break;
            case duckdb::WindowBoundary::EXPR_PRECEDING_ROWS:
              frame_end = row_idx >= (size_t)win.end_offset
                              ? row_idx - win.end_offset
                              : 0;
              break;
            case duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS:
              frame_end =
                  std::min(row_idx + win.end_offset, partition_rows.size() - 1);
              break;
            default:
              frame_end = row_idx;
            }

            // Ensure valid frame
            if (frame_start > frame_end) {
              out_row.columns.push_back(duckdb::Value());
              continue;
            }

            // Compute aggregate over frame [frame_start, frame_end]
            double sum = 0;
            int64_t count = 0;
            duckdb::Value min_val, max_val;

            for (size_t f = frame_start; f <= frame_end; ++f) {
              const auto &frow = partition_rows[f];
              if (win.arg_column_idx >= 0 &&
                  (size_t)win.arg_column_idx < frow.columns.size()) {
                const auto &val = frow.columns[win.arg_column_idx];
                if (!val.IsNull()) {
                  count++;
                  if (win.function == "SUM" || win.function == "AVG") {
                    sum += val.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                               .GetValue<double>();
                  }
                  if (win.function == "MIN") {
                    if (min_val.IsNull() || val < min_val)
                      min_val = val;
                  }
                  if (win.function == "MAX") {
                    if (max_val.IsNull() || val > max_val)
                      max_val = val;
                  }
                }
              } else {
                count++;
              }
            }

            if (win.function == "SUM") {
              out_row.columns.push_back(duckdb::Value(sum));
            } else if (win.function == "COUNT") {
              out_row.columns.push_back(duckdb::Value(count));
            } else if (win.function == "AVG") {
              double avg = count > 0 ? sum / count : 0;
              out_row.columns.push_back(duckdb::Value(avg));
            } else if (win.function == "MIN") {
              out_row.columns.push_back(min_val);
            } else if (win.function == "MAX") {
              out_row.columns.push_back(max_val);
            } else {
              out_row.columns.push_back(duckdb::Value());
            }
          }
        }

        // Emit
        delta_.insert(out_row, 1);
        result_.insert(out_row, 1);
        old_out.insert(out_row, 1);
      }
    }

    ++version_;
  }
};

} // namespace dbsp_native

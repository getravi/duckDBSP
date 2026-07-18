#pragma once

#include "dbsp_duckdb_types.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include <algorithm>
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
    // Kept sorted by `cmp` (the ORDER BY comparator). A vector (not a
    // multiset) gives O(1) positional/random access so fast paths can
    // re-render only the affected rows without copying the whole partition.
    std::vector<DuckDBRow> sorted_rows;
    RowComparator cmp;

    PartitionState(const std::vector<NativeSortView::SortColumn> &c)
        : cmp(RowComparator{c}) {}
  };

  std::map<PartitionKey, PartitionState> partitions_;
  std::vector<NativeSortView::SortColumn> primary_sort_columns_;
  // Rendered output row per partition, index-aligned with the sorted
  // partition, so individual rows can be retracted/re-emitted (fast paths).
  std::map<PartitionKey, std::vector<DuckDBRow>> partition_outputs_;

  // Output state
  DuckDBZSet result_;
  DuckDBZSet delta_;
  TableSchema result_schema_;

  // Render the full output row (source columns + every window column) for one
  // ordered index. Ranking scalars (row_number/rank/dense_rank) are passed in
  // by the caller (running sweep on the full path; unused on fast paths, which
  // the eligibility gate guarantees carry no ranking windows). peer_start /
  // peer_end are only read by RANGE/GROUPS frames and rank windows, so fast
  // paths may pass empty vectors.
  DuckDBRow render_row(const std::vector<DuckDBRow> &rows, size_t row_idx,
                       const std::vector<size_t> &peer_start,
                       const std::vector<size_t> &peer_end,
                       int64_t current_row_number, int64_t current_rank,
                       int64_t current_dense_rank) const {
    DuckDBRow out_row = rows[row_idx];
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
              rows[row_idx - offset].columns[win.arg_column_idx]);
        } else {
          out_row.columns.push_back(duckdb::Value());
        }
      } else if (win.function == "LEAD") {
        int offset = win.offset;
        if (row_idx + offset < rows.size() && win.arg_column_idx >= 0) {
          out_row.columns.push_back(
              rows[row_idx + offset].columns[win.arg_column_idx]);
        } else {
          out_row.columns.push_back(duckdb::Value());
        }
      } else if (win.function == "FIRST_VALUE") {
        if (!rows.empty() && win.arg_column_idx >= 0 &&
            (size_t)win.arg_column_idx < rows[0].columns.size()) {
          out_row.columns.push_back(rows[0].columns[win.arg_column_idx]);
        } else {
          out_row.columns.push_back(duckdb::Value());
        }
      } else if (win.function == "LAST_VALUE") {
        // LAST_VALUE ... IGNORE NULLS (fillforward): last non-null value in
        // the frame, scanning backward from the frame end to the frame start.
        duckdb::Value result;
        if (!rows.empty() && win.arg_column_idx >= 0) {
          size_t frame_end_idx = row_idx; // default CURRENT_ROW
          if (win.end == duckdb::WindowBoundary::UNBOUNDED_FOLLOWING)
            frame_end_idx = rows.size() - 1;
          else if (win.end == duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS)
            frame_end_idx =
                std::min(row_idx + (size_t)win.end_offset, rows.size() - 1);
          size_t scan_lo = 0;
          if (win.start == duckdb::WindowBoundary::CURRENT_ROW_ROWS)
            scan_lo = row_idx;
          else if (win.start == duckdb::WindowBoundary::EXPR_PRECEDING_ROWS)
            scan_lo = row_idx >= (size_t)win.start_offset
                          ? row_idx - (size_t)win.start_offset
                          : 0;
          for (size_t f = frame_end_idx + 1; f-- > scan_lo;) {
            if ((size_t)win.arg_column_idx < rows[f].columns.size() &&
                !rows[f].columns[win.arg_column_idx].IsNull()) {
              result = rows[f].columns[win.arg_column_idx];
              break;
            }
          }
        }
        out_row.columns.push_back(result);
      } else if (win.function == "NTH_VALUE") {
        int n = win.offset;
        if (n > 0 && (size_t)(n - 1) < rows.size() && win.arg_column_idx >= 0 &&
            (size_t)win.arg_column_idx < rows[n - 1].columns.size()) {
          out_row.columns.push_back(rows[n - 1].columns[win.arg_column_idx]);
        } else {
          out_row.columns.push_back(duckdb::Value());
        }
      } else if (win.function == "NTILE") {
        int num_buckets = win.offset;
        if (num_buckets <= 0)
          num_buckets = 1;
        int64_t total = rows.size();
        int64_t bucket = ((int64_t)row_idx * num_buckets) / total + 1;
        out_row.columns.push_back(duckdb::Value(bucket));
      } else {
        // Aggregates over frame [frame_start, frame_end]
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
          frame_start =
              row_idx >= (size_t)win.start_offset ? row_idx - win.start_offset : 0;
          break;
        case duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS:
          frame_start = std::min(row_idx + win.start_offset, rows.size() - 1);
          break;
        default:
          frame_start = 0;
        }

        size_t frame_end = rows.size() - 1;
        switch (win.end) {
        case duckdb::WindowBoundary::UNBOUNDED_FOLLOWING:
          frame_end = rows.size() - 1;
          break;
        case duckdb::WindowBoundary::CURRENT_ROW_ROWS:
          frame_end = row_idx;
          break;
        case duckdb::WindowBoundary::CURRENT_ROW_RANGE:
        case duckdb::WindowBoundary::CURRENT_ROW_GROUPS:
          frame_end = peer_end[row_idx];
          break;
        case duckdb::WindowBoundary::EXPR_PRECEDING_ROWS:
          frame_end =
              row_idx >= (size_t)win.end_offset ? row_idx - win.end_offset : 0;
          break;
        case duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS:
          frame_end = std::min(row_idx + win.end_offset, rows.size() - 1);
          break;
        default:
          frame_end = row_idx;
        }

        if (frame_start > frame_end) {
          out_row.columns.push_back(duckdb::Value());
          continue;
        }

        double sum = 0;
        int64_t count = 0;
        duckdb::Value min_val, max_val;
        for (size_t f = frame_start; f <= frame_end; ++f) {
          const auto &frow = rows[f];
          if (win.arg_column_idx >= 0 &&
              (size_t)win.arg_column_idx < frow.columns.size()) {
            const auto &val = frow.columns[win.arg_column_idx];
            if (!val.IsNull()) {
              count++;
              if (win.function == "SUM" || win.function == "AVG") {
                sum +=
                    val.DefaultCastAs(duckdb::LogicalType::DOUBLE).GetValue<double>();
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
    return out_row;
  }

  // True iff every window is a fast-pathable shape: offset (LAG/LEAD),
  // ROWS-frame aggregate (SUM/COUNT/AVG/MIN/MAX), or LAST_VALUE. Any
  // RANK/DENSE_RANK/ROW_NUMBER/NTILE/NTH_VALUE/FIRST_VALUE, or any
  // RANGE/GROUPS boundary, forces the full-partition renderer.
  bool all_windows_fast_eligible() const {
    for (const auto &w : windows_) {
      const bool offset = (w.function == "LAG" || w.function == "LEAD");
      const bool fill = (w.function == "LAST_VALUE");
      const bool agg = (w.function == "SUM" || w.function == "COUNT" ||
                        w.function == "AVG" || w.function == "MIN" ||
                        w.function == "MAX");
      auto rows_boundary = [](duckdb::WindowBoundary b) {
        return b == duckdb::WindowBoundary::UNBOUNDED_PRECEDING ||
               b == duckdb::WindowBoundary::UNBOUNDED_FOLLOWING ||
               b == duckdb::WindowBoundary::CURRENT_ROW_ROWS ||
               b == duckdb::WindowBoundary::EXPR_PRECEDING_ROWS ||
               b == duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS;
      };
      const bool rows_frame = rows_boundary(w.start) && rows_boundary(w.end);
      if (!(offset || fill || (agg && rows_frame)))
        return false;
    }
    return true;
  }

  // Union of output-row indices dirtied by changes at `anchors`, per window
  // shape. Returns false if any window lacks a fast rule (caller full-renders).
  bool affected_indices(const std::vector<DuckDBRow> &rows,
                        const std::vector<size_t> &anchors,
                        std::vector<size_t> &out) const {
    const size_t n = rows.size();
    for (const auto &win : windows_) {
      const size_t off = (size_t)win.offset;
      if (win.function == "LAG") {
        for (size_t p : anchors) {
          out.push_back(p);
          if (p + off < n)
            out.push_back(p + off); // reader r where r-off == p
        }
      } else if (win.function == "LEAD") {
        for (size_t p : anchors) {
          out.push_back(p);
          if (p >= off)
            out.push_back(p - off); // reader r where r+off == p
        }
      } else if (win.function == "SUM" || win.function == "COUNT" ||
                 win.function == "AVG" || win.function == "MIN" ||
                 win.function == "MAX") {
        // ROWS frame(r) = [r+lo_off, r+hi_off]; p is in frame(r) iff
        // p-hi_off <= r <= p-lo_off. Unbounded ends use sentinels. This one
        // formula covers bounded rolling frames AND unbounded-preceding
        // running sums (suffix) AND unbounded-following (prefix).
        const int64_t NEG = -(1LL << 60), POS = (1LL << 60);
        int64_t lo_off, hi_off;
        switch (win.start) {
        case duckdb::WindowBoundary::UNBOUNDED_PRECEDING:
          lo_off = NEG;
          break;
        case duckdb::WindowBoundary::CURRENT_ROW_ROWS:
          lo_off = 0;
          break;
        case duckdb::WindowBoundary::EXPR_PRECEDING_ROWS:
          lo_off = -(int64_t)win.start_offset;
          break;
        case duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS:
          lo_off = (int64_t)win.start_offset;
          break;
        default:
          return false; // RANGE/GROUPS not fast
        }
        switch (win.end) {
        case duckdb::WindowBoundary::UNBOUNDED_FOLLOWING:
          hi_off = POS;
          break;
        case duckdb::WindowBoundary::CURRENT_ROW_ROWS:
          hi_off = 0;
          break;
        case duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS:
          hi_off = (int64_t)win.end_offset;
          break;
        case duckdb::WindowBoundary::EXPR_PRECEDING_ROWS:
          hi_off = -(int64_t)win.end_offset;
          break;
        default:
          return false;
        }
        for (size_t p : anchors) {
          int64_t rlo = (hi_off >= POS) ? 0 : (int64_t)p - hi_off;
          int64_t rhi = (lo_off <= NEG) ? (int64_t)n - 1 : (int64_t)p - lo_off;
          if (rlo < 0)
            rlo = 0;
          if (rhi > (int64_t)n - 1)
            rhi = (int64_t)n - 1;
          for (int64_t r = rlo; r <= rhi; ++r)
            out.push_back((size_t)r);
        }
      } else if (win.function == "LAST_VALUE") {
        // fillforward: value at r = last non-null in the frame. A change at p
        // affects the forward run until the next non-null value (including
        // that row is harmless — it re-renders to its own value).
        for (size_t p : anchors) {
          out.push_back(p);
          for (size_t r = p + 1; r < n; ++r) {
            out.push_back(r);
            if (win.arg_column_idx >= 0 &&
                (size_t)win.arg_column_idx < rows[r].columns.size() &&
                !rows[r].columns[win.arg_column_idx].IsNull())
              break;
          }
        }
      } else {
        return false; // shape not yet fast-handled
      }
    }
    return true;
  }

  // Re-emit only `affected` output rows: retract the cached row, render the
  // new one, insert it, update the cache slot. Requires the cache to be
  // index-aligned with `rows` (caller guarantees size unchanged).
  void emit_affected(const PartitionKey &key, const std::vector<DuckDBRow> &rows,
                     std::vector<size_t> affected) {
    std::sort(affected.begin(), affected.end());
    affected.erase(std::unique(affected.begin(), affected.end()),
                   affected.end());
    static const std::vector<size_t> kNoPeers;
    auto &cache = partition_outputs_[key];
    for (size_t idx : affected) {
      if (idx >= rows.size() || idx >= cache.size())
        continue;
      delta_.insert(cache[idx], -1);
      result_.insert(cache[idx], -1);
      DuckDBRow nr = render_row(rows, idx, kNoPeers, kNoPeers,
                               (int64_t)idx + 1, 0, 0);
      delta_.insert(nr, 1);
      result_.insert(nr, 1);
      cache[idx] = nr;
    }
  }

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
  void set_result(const DuckDBZSet &result) override { result_ = result; version_++; }
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

    // 1. Group the delta by partition into inserts / deletes.
    struct PartDelta {
      std::vector<DuckDBRow> inserts;
      std::vector<DuckDBRow> deletes;
    };
    std::map<PartitionKey, PartDelta> by_part;
    for (const auto &[row, weight] : changes) {
      PartitionKey key;
      if (!windows_.empty()) {
        for (size_t idx : windows_[0].partition_indices)
          if (idx < row.columns.size())
            key.push_back(row.columns[idx]);
      }
      if (weight > 0)
        for (Weight i = 0; i < weight; ++i)
          by_part[key].inserts.push_back(row);
      else
        for (Weight i = 0; i > weight; --i)
          by_part[key].deletes.push_back(row);
    }

    // 2. Apply per partition. A PURE VALUE UPDATE — every delete pairs with an
    // insert of the same sort key — overwrites rows in place (O(k log n)),
    // leaving positions and size unchanged, so the output cache stays aligned
    // and the fast path can re-emit only affected rows. Anything else shifts
    // the vector (O(n)) and is marked structural -> full re-render.
    std::set<PartitionKey> affected_partitions;
    std::map<PartitionKey, size_t> size_before;
    std::map<PartitionKey, std::vector<DuckDBRow>> anchors_by_part;
    std::map<PartitionKey, bool> structural_change;

    for (auto &kv : by_part) {
      const PartitionKey &key = kv.first;
      PartDelta &pd = kv.second;
      auto it = partitions_.find(key);
      if (it == partitions_.end())
        it = partitions_.emplace(key, PartitionState(primary_sort_columns_))
                 .first;
      auto &vec = it->second.sorted_rows;
      const auto &cmp = it->second.cmp;
      size_before[key] = vec.size();
      affected_partitions.insert(key);

      bool structural = pd.inserts.size() != pd.deletes.size();
      std::vector<bool> ins_used(pd.inserts.size(), false);
      std::vector<std::pair<size_t, size_t>> overwrites; // (vec_idx, ins_idx)
      if (!structural) {
        for (const auto &d : pd.deletes) {
          size_t m = pd.inserts.size();
          for (size_t j = 0; j < pd.inserts.size(); ++j)
            if (!ins_used[j] && !cmp(d, pd.inserts[j]) &&
                !cmp(pd.inserts[j], d)) { // equal sort key
              m = j;
              break;
            }
          if (m == pd.inserts.size()) {
            structural = true;
            break;
          }
          auto lo = std::lower_bound(vec.begin(), vec.end(), d, cmp);
          auto hi = std::upper_bound(vec.begin(), vec.end(), d, cmp);
          auto match = hi;
          for (auto k = lo; k != hi; ++k)
            if (*k == d) {
              match = k;
              break;
            }
          if (match == hi) {
            structural = true;
            break;
          }
          ins_used[m] = true;
          overwrites.push_back({(size_t)(match - vec.begin()), m});
        }
      }

      if (!structural) {
        for (const auto &ow : overwrites) {
          vec[ow.first] = pd.inserts[ow.second];
          anchors_by_part[key].push_back(pd.inserts[ow.second]);
        }
        structural_change[key] = false;
      } else {
        for (const auto &d : pd.deletes) {
          auto lo = std::lower_bound(vec.begin(), vec.end(), d, cmp);
          auto hi = std::upper_bound(vec.begin(), vec.end(), d, cmp);
          auto match = hi;
          for (auto k = lo; k != hi; ++k)
            if (*k == d) {
              match = k;
              break;
            }
          if (match != hi)
            vec.erase(match);
          else if (lo != hi)
            vec.erase(lo);
        }
        for (const auto &ins : pd.inserts) {
          auto pos = std::lower_bound(vec.begin(), vec.end(), ins, cmp);
          vec.insert(pos, ins);
          anchors_by_part[key].push_back(ins);
        }
        structural_change[key] = true;
      }
    }

    // 2. Recompute window functions for affected partitions
    for (const auto &key : affected_partitions) {
      auto &cache = partition_outputs_[key];
      auto it = partitions_.find(key);

      // Partition emptied: retract all cached output and drop it.
      if (it == partitions_.end() || it->second.sorted_rows.empty()) {
        for (const auto &row : cache) {
          delta_.insert(row, -1);
          result_.insert(row, -1);
        }
        cache.clear();
        if (it != partitions_.end())
          partitions_.erase(it);
        partition_outputs_.erase(key);
        continue;
      }

      // Persistent sorted vector — direct positional access, no copy.
      const std::vector<DuckDBRow> &partition_rows = it->second.sorted_rows;

      // FAST PATH: size unchanged (pure value updates) and cache index-aligned
      // — re-emit only the rows each window makes dirty. affected_indices
      // returns false if any window shape has no fast rule, in which case we
      // fall through to the full re-render below.
      if (!structural_change[key] && cache.size() == partition_rows.size() &&
          size_before[key] == partition_rows.size()) {
        RowComparator cmp{primary_sort_columns_};
        std::vector<size_t> anchors;
        for (const auto &r : anchors_by_part[key]) {
          auto lo = std::lower_bound(partition_rows.begin(),
                                     partition_rows.end(), r, cmp);
          auto hi = std::upper_bound(partition_rows.begin(),
                                     partition_rows.end(), r, cmp);
          size_t idx = (size_t)(lo - partition_rows.begin());
          for (auto j = lo; j != hi; ++j)
            if (*j == r) {
              idx = (size_t)(j - partition_rows.begin());
              break;
            }
          anchors.push_back(idx);
        }
        std::vector<size_t> affected;
        if (affected_indices(partition_rows, anchors, affected)) {
          emit_affected(key, partition_rows, affected);
          continue;
        }
      }

      // FULL PATH: retract all cached output, then re-render every row.
      for (const auto &row : cache) {
        delta_.insert(row, -1);
        result_.insert(row, -1);
      }
      cache.clear();

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
              const auto &va = partition_rows[i].columns[col.column_idx];
              const auto &vb = partition_rows[i - 1].columns[col.column_idx];
              // duckdb::Value comparisons throw on NULL — treat NULLs as
              // peers of each other (matches the sort comparator)
              const bool an = va.IsNull(), bn = vb.IsNull();
              if (an != bn || (!an && va != vb)) {
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
            // LAST_VALUE ... IGNORE NULLS (fillforward): last non-null value
            // in the frame, scanning backward from the frame end.
            duckdb::Value ff_result;
            if (!partition_rows.empty() && win.arg_column_idx >= 0) {
              size_t frame_end_idx = row_idx; // default CURRENT_ROW
              if (win.end == duckdb::WindowBoundary::UNBOUNDED_FOLLOWING)
                frame_end_idx = partition_rows.size() - 1;
              else if (win.end == duckdb::WindowBoundary::EXPR_FOLLOWING_ROWS)
                frame_end_idx = std::min(row_idx + (size_t)win.end_offset,
                                         partition_rows.size() - 1);
              size_t scan_lo = 0;
              if (win.start == duckdb::WindowBoundary::CURRENT_ROW_ROWS)
                scan_lo = row_idx;
              else if (win.start == duckdb::WindowBoundary::EXPR_PRECEDING_ROWS)
                scan_lo = row_idx >= (size_t)win.start_offset
                              ? row_idx - (size_t)win.start_offset
                              : 0;
              for (size_t f = frame_end_idx + 1; f-- > scan_lo;) {
                if ((size_t)win.arg_column_idx <
                        partition_rows[f].columns.size() &&
                    !partition_rows[f].columns[win.arg_column_idx].IsNull()) {
                  ff_result = partition_rows[f].columns[win.arg_column_idx];
                  break;
                }
              }
            }
            out_row.columns.push_back(ff_result);
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
        cache.push_back(out_row);
      }
    }

    ++version_;
  }
};

} // namespace dbsp_native

// DBSP DuckDB Native Type Integration
// Uses DuckDB's Value type directly for seamless integration

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dbsp_native {

using Weight = int64_t;

// DuckDB Row - uses native Value types
struct DuckDBRow {
  std::vector<duckdb::Value> columns;

  // Default equality: NULL-aware for GROUP BY/DISTINCT semantics
  // In GROUP BY and DISTINCT: NULL == NULL (same group)
  bool operator==(const DuckDBRow &other) const {
    if (columns.size() != other.columns.size())
      return false;
    for (size_t i = 0; i < columns.size(); i++) {
      bool this_null = columns[i].IsNull();
      bool other_null = other.columns[i].IsNull();

      // For GROUP BY/DISTINCT: NULL == NULL
      if (this_null && other_null) continue;
      if (this_null || other_null) return false;

      // Use DuckDB's value comparison for non-NULL
      if (columns[i] != other.columns[i])
        return false;
    }
    return true;
  }
};

// Hash function for DuckDBRow - NULL-aware
struct DuckDBRowHash {
  static constexpr size_t NULL_HASH = 0x9e3779b97f4a7c15ULL;

  size_t operator()(const DuckDBRow &row) const noexcept {
    size_t hash = 0;
    for (const auto &col : row.columns) {
      size_t col_hash;
      if (col.IsNull()) {
        col_hash = NULL_HASH;  // Special hash for NULL
      } else {
        col_hash = col.Hash();
      }
      hash ^= col_hash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    return hash;
  }
};

// JOIN-specific equality: NULL never matches NULL (SQL standard for JOINs)
struct JoinRowEqual {
  bool operator()(const DuckDBRow &a, const DuckDBRow &b) const {
    if (a.columns.size() != b.columns.size())
      return false;

    for (size_t i = 0; i < a.columns.size(); i++) {
      // In JOIN: NULL != NULL (no match)
      if (a.columns[i].IsNull() || b.columns[i].IsNull()) {
        return false;
      }

      if (a.columns[i] != b.columns[i]) {
        return false;
      }
    }
    return true;
  }
};

// Hash function for DuckDB Value
struct DuckDBValueHash {
  size_t operator()(const duckdb::Value &val) const noexcept {
    return val.Hash();
  }
};

// Z-Set using DuckDB native types
class DuckDBZSet {
public:
  using DataMap = std::unordered_map<DuckDBRow, Weight, DuckDBRowHash>;
  using iterator = DataMap::iterator;
  using const_iterator = DataMap::const_iterator;

  void insert(const DuckDBRow &row, Weight weight = 1) {
    if (weight == 0)
      return;
    auto &w = data_[row];
    w += weight;
    if (w == 0) {
      data_.erase(row);
    }
  }

  void insert(DuckDBRow &&row, Weight weight = 1) {
    if (weight == 0)
      return;
    auto &w = data_[std::move(row)];
    w += weight;
    if (w == 0) {
      data_.erase(row);
    }
  }

  Weight get(const DuckDBRow &row) const {
    auto it = data_.find(row);
    return it != data_.end() ? it->second : 0;
  }

  void clear() { data_.clear(); }
  bool empty() const { return data_.empty(); }
  size_t size() const { return data_.size(); }

  iterator begin() { return data_.begin(); }
  iterator end() { return data_.end(); }
  const_iterator begin() const { return data_.begin(); }
  const_iterator end() const { return data_.end(); }

  // Z-Set operations
  DuckDBZSet operator+(const DuckDBZSet &other) const {
    DuckDBZSet result = *this;
    for (const auto &[row, weight] : other.data_) {
      result.insert(row, weight);
    }
    return result;
  }

  DuckDBZSet operator-() const {
    DuckDBZSet result;
    for (const auto &[row, weight] : data_) {
      result.insert(row, -weight);
    }
    return result;
  }

private:
  DataMap data_;
};

// Column metadata for tracked tables
struct ColumnInfo {
  std::string name;
  duckdb::LogicalType type;
};

// Table schema
struct TableSchema {
  std::string table_name;
  std::vector<ColumnInfo> columns;

  duckdb::LogicalType GetColumnType(size_t idx) const {
    return idx < columns.size() ? columns[idx].type
                                : duckdb::LogicalType::VARCHAR;
  }

  std::string GetColumnName(size_t idx) const {
    return idx < columns.size() ? columns[idx].name
                                : "col" + std::to_string(idx);
  }
};

// Change record for CDC
struct ChangeRecord {
  enum Type { INSERT, DELETE, UPDATE };
  Type type;
  DuckDBRow row;
  DuckDBRow old_row; // For UPDATE
  uint64_t timestamp;
  uint64_t sequence;
};

// Tracked table that captures all changes
class TrackedTable {
public:
  TrackedTable(const std::string &name, const TableSchema &schema)
      : name_(name), schema_(schema), sequence_(0) {}

  const std::string &name() const { return name_; }
  const TableSchema &schema() const { return schema_; }

  // Apply changes and record them
  void insert(const DuckDBRow &row) {
    current_state_.insert(row, 1);
    pending_changes_.insert(row, 1);
    record_change(ChangeRecord::INSERT, row, {});
  }

  void remove(const DuckDBRow &row) {
    current_state_.insert(row, -1);
    pending_changes_.insert(row, -1);
    record_change(ChangeRecord::DELETE, row, {});
  }

  void update(const DuckDBRow &old_row, const DuckDBRow &new_row) {
    current_state_.insert(old_row, -1);
    current_state_.insert(new_row, 1);
    pending_changes_.insert(old_row, -1);
    pending_changes_.insert(new_row, 1);
    record_change(ChangeRecord::UPDATE, new_row, old_row);
  }

  // Get and clear pending changes (for view updates)
  DuckDBZSet consume_changes() {
    DuckDBZSet changes = std::move(pending_changes_);
    pending_changes_ = DuckDBZSet();
    return changes;
  }

  const DuckDBZSet &current_state() const { return current_state_; }
  const std::vector<ChangeRecord> &change_log() const { return change_log_; }

  // Clear change log (after checkpoint)
  void clear_log() { change_log_.clear(); }

private:
  void record_change(ChangeRecord::Type type, const DuckDBRow &row,
                     const DuckDBRow &old_row) {
    ChangeRecord rec;
    rec.type = type;
    rec.row = row;
    rec.old_row = old_row;
    rec.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    rec.sequence = ++sequence_;
    change_log_.push_back(rec);
  }

  std::string name_;
  TableSchema schema_;
  DuckDBZSet current_state_;
  DuckDBZSet pending_changes_;
  std::vector<ChangeRecord> change_log_;
  uint64_t sequence_;
};

// Base class for native materialized views
class NativeMaterializedView {
public:
  explicit NativeMaterializedView(const std::string &name,
                                  const std::string &sql)
      : name_(name), sql_(sql), version_(0) {}

  virtual ~NativeMaterializedView() = default;

  const std::string &name() const { return name_; }
  const std::string &sql() const { return sql_; }
  uint64_t version() const { return version_; }

  // Apply incremental changes
  virtual void apply_changes(const std::string &table_name,
                             const DuckDBZSet &changes) = 0;

  // Get current result
  virtual const DuckDBZSet &get_result() const = 0;

  // Get result schema
  virtual const TableSchema &result_schema() const = 0;

  // Get source tables this view depends on
  virtual std::vector<std::string> source_tables() const = 0;

  // Reset the view
  virtual void reset() = 0;

protected:
  std::string name_;
  std::string sql_;
  uint64_t version_;
};

// Filter view: SELECT * FROM table WHERE condition
// NOTE: PredicateFn must implement three-valued logic (TRUE/FALSE/UNKNOWN)
// where UNKNOWN (NULL comparisons) returns false (filtered out)
class NativeFilterView : public NativeMaterializedView {
public:
  using PredicateFn = std::function<bool(const DuckDBRow &)>;

  NativeFilterView(const std::string &name, const std::string &sql,
                   const std::string &source_table, const TableSchema &schema,
                   PredicateFn predicate)
      : NativeMaterializedView(name, sql), source_table_(source_table),
        schema_(schema), predicate_(std::move(predicate)) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    if (table_name != source_table_)
      return;

    for (const auto &[row, weight] : changes) {
      if (predicate_(row)) {
        result_.insert(row, weight);
      }
    }
    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    result_.clear();
    version_ = 0;
  }

private:
  std::string source_table_;
  TableSchema schema_;
  PredicateFn predicate_;
  DuckDBZSet result_;
};

// Projection view: SELECT col1, col2 FROM table
class NativeProjectView : public NativeMaterializedView {
public:
  using ProjectFn = std::function<DuckDBRow(const DuckDBRow &)>;

  NativeProjectView(const std::string &name, const std::string &sql,
                    const std::string &source_table,
                    const TableSchema &result_schema, ProjectFn project)
      : NativeMaterializedView(name, sql), source_table_(source_table),
        schema_(result_schema), project_(std::move(project)) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    if (table_name != source_table_)
      return;

    for (const auto &[row, weight] : changes) {
      DuckDBRow projected = project_(row);
      result_.insert(std::move(projected), weight);
    }
    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    result_.clear();
    version_ = 0;
  }

private:
  std::string source_table_;
  TableSchema schema_;
  ProjectFn project_;
  DuckDBZSet result_;
};

// Aggregate view: SELECT key, AGG(val) FROM table GROUP BY key
class NativeAggregateView : public NativeMaterializedView {
public:
  enum class AggType { SUM, COUNT, AVG, MIN, MAX };

  using KeyFn = std::function<DuckDBRow(const DuckDBRow &)>;
  using ValueFn = std::function<duckdb::Value(const DuckDBRow &)>;

  NativeAggregateView(const std::string &name, const std::string &sql,
                      const std::string &source_table,
                      const TableSchema &result_schema, KeyFn key_fn,
                      ValueFn value_fn, AggType agg_type)
      : NativeMaterializedView(name, sql), source_table_(source_table),
        schema_(result_schema), key_fn_(std::move(key_fn)),
        value_fn_(std::move(value_fn)), agg_type_(agg_type) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    if (table_name != source_table_)
      return;

    // Group changes by key - NULL-aware
    std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> delta_sums;
    std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> delta_counts;
    std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> delta_null_counts;

    for (const auto &[row, weight] : changes) {
      DuckDBRow key = key_fn_(row);
      duckdb::Value val = value_fn_(row);

      // SQL Standard: NULL values are ignored in aggregates (except COUNT(*))
      if (val.IsNull()) {
        delta_null_counts[key] += weight;
        // Don't update delta_sums or delta_counts for NULL values
      } else {
        int64_t int_val = 0;
        if (val.type().IsNumeric()) {
          int_val = val.GetValue<int64_t>();
        }

        delta_sums[key] += int_val * weight;
        delta_counts[key] += weight;
      }
    }

    // Update aggregates - NULL-aware
    std::set<DuckDBRow> all_keys;
    for (const auto &[key, _] : delta_sums) all_keys.insert(key);
    for (const auto &[key, _] : delta_counts) all_keys.insert(key);
    for (const auto &[key, _] : delta_null_counts) all_keys.insert(key);

    for (const auto &key : all_keys) {
      auto &state = agg_states_[key];

      int64_t delta_sum = delta_sums[key];
      int64_t delta_count = delta_counts[key];
      int64_t delta_null_count = delta_null_counts[key];

      // Remove old result
      if (state.count > 0 || state.null_count > 0) {
        DuckDBRow old_result = make_result_row(key, compute_agg(state));
        result_.insert(old_result, -1);
      }

      // Update state
      state.sum += delta_sum;
      state.count += delta_count;
      state.null_count += delta_null_count;

      // Add new result
      if (state.count > 0 || state.null_count > 0) {
        DuckDBRow new_result = make_result_row(key, compute_agg(state));
        result_.insert(new_result, 1);
      } else {
        agg_states_.erase(key);
      }
    }

    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    agg_states_.clear();
    result_.clear();
    version_ = 0;
  }

private:
  struct AggState {
    int64_t sum = 0;
    int64_t count = 0;         // Non-NULL count (for COUNT(column))
    int64_t null_count = 0;    // NULL count (for COUNT(*))
  };

  duckdb::Value compute_agg(const AggState &state) const {
    switch (agg_type_) {
    case AggType::SUM:
      // SQL Standard: SUM of all NULLs returns NULL
      if (state.count == 0) {
        return duckdb::Value(duckdb::LogicalType::BIGINT);  // NULL value
      }
      return duckdb::Value::BIGINT(state.sum);

    case AggType::COUNT:
      // COUNT(column) excludes NULLs, returns non-NULL count
      return duckdb::Value::BIGINT(state.count);

    case AggType::AVG:
      // SQL Standard: AVG of all NULLs returns NULL
      if (state.count == 0) {
        return duckdb::Value(duckdb::LogicalType::BIGINT);  // NULL value
      }
      return duckdb::Value::BIGINT(state.sum / state.count);

    default:
      return duckdb::Value::BIGINT(state.sum);
    }
  }

  DuckDBRow make_result_row(const DuckDBRow &key, const duckdb::Value &agg_val) const {
    DuckDBRow result;
    result.columns = key.columns;
    result.columns.push_back(agg_val);  // Can be NULL now
    return result;
  }

  std::string source_table_;
  TableSchema schema_;
  KeyFn key_fn_;
  ValueFn value_fn_;
  AggType agg_type_;
  std::unordered_map<DuckDBRow, AggState, DuckDBRowHash> agg_states_;
  DuckDBZSet result_;
};

// Join view with incremental bilinear formula
class NativeJoinView : public NativeMaterializedView {
public:
  using KeyFn = std::function<duckdb::Value(const DuckDBRow &)>;

  NativeJoinView(const std::string &name, const std::string &sql,
                 const std::string &left_table, const std::string &right_table,
                 const TableSchema &result_schema, KeyFn left_key,
                 KeyFn right_key)
      : NativeMaterializedView(name, sql), left_table_(left_table),
        right_table_(right_table), schema_(result_schema),
        left_key_(std::move(left_key)), right_key_(std::move(right_key)) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    if (table_name == left_table_) {
      // Index changes by key
      for (const auto &[row, weight] : changes) {
        duckdb::Value key = left_key_(row);

        // SQL Standard: NULL never matches NULL in JOINs
        if (key.IsNull()) {
          // Don't index NULL keys, they never produce join results
          continue;
        }

        // Join with existing right rows
        auto it = right_indexed_.find(key);
        if (it != right_indexed_.end()) {
          for (const auto &[right_row, right_weight] : it->second) {
            add_join_result(row, right_row, weight * right_weight);
          }
        }

        // Update integrated left
        left_indexed_[key].insert(row, weight);
      }
    } else if (table_name == right_table_) {
      for (const auto &[row, weight] : changes) {
        duckdb::Value key = right_key_(row);

        // SQL Standard: NULL never matches NULL in JOINs
        if (key.IsNull()) {
          // Don't index NULL keys, they never produce join results
          continue;
        }

        // Join with existing left rows
        auto it = left_indexed_.find(key);
        if (it != left_indexed_.end()) {
          for (const auto &[left_row, left_weight] : it->second) {
            add_join_result(left_row, row, left_weight * weight);
          }
        }

        // Update integrated right
        right_indexed_[key].insert(row, weight);
      }
    }
    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {left_table_, right_table_};
  }

  void reset() override {
    left_indexed_.clear();
    right_indexed_.clear();
    result_.clear();
    version_ = 0;
  }

private:
  void add_join_result(const DuckDBRow &left, const DuckDBRow &right,
                       Weight weight) {
    DuckDBRow joined;
    joined.columns.reserve(left.columns.size() + right.columns.size());
    for (const auto &col : left.columns) {
      joined.columns.push_back(col);
    }
    for (const auto &col : right.columns) {
      joined.columns.push_back(col);
    }
    result_.insert(std::move(joined), weight);
  }

  std::string left_table_;
  std::string right_table_;
  TableSchema schema_;
  KeyFn left_key_;
  KeyFn right_key_;
  std::unordered_map<duckdb::Value, DuckDBZSet, dbsp_native::DuckDBValueHash>
      left_indexed_;
  std::unordered_map<duckdb::Value, DuckDBZSet, dbsp_native::DuckDBValueHash>
      right_indexed_;
  DuckDBZSet result_;
};

// Distinct view - NULL-aware (SQL Standard: multiple NULLs -> single NULL)
// Uses DuckDBRowHash and DuckDBRow::operator== which treat NULL == NULL
class NativeDistinctView : public NativeMaterializedView {
public:
  NativeDistinctView(const std::string &name, const std::string &sql,
                     const std::string &source_table, const TableSchema &schema)
      : NativeMaterializedView(name, sql), source_table_(source_table),
        schema_(schema) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    if (table_name != source_table_)
      return;

    for (const auto &[row, weight] : changes) {
      auto it = counts_.find(row);
      int64_t old_count = (it != counts_.end()) ? it->second : 0;
      int64_t new_count = old_count + weight;

      // Distinct logic: output +1 when 0->positive, -1 when positive->0
      bool was_present = old_count > 0;
      bool is_present = new_count > 0;

      if (!was_present && is_present) {
        result_.insert(row, 1);
      } else if (was_present && !is_present) {
        result_.insert(row, -1);
      }

      if (new_count == 0) {
        counts_.erase(row);
      } else {
        counts_[row] = new_count;
      }
    }
    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    counts_.clear();
    result_.clear();
    version_ = 0;
  }

private:
  std::string source_table_;
  TableSchema schema_;
  std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> counts_;
  DuckDBZSet result_;
};

} // namespace dbsp_native

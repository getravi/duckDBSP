// DBSP DuckDB Native Type Integration
// Uses DuckDB's Value type directly for seamless integration

#pragma once

#include "dbsp_zset.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <set>
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
      if (this_null && other_null)
        continue;
      if (this_null || other_null)
        return false;

      // Use DuckDB's value comparison for non-NULL
      if (columns[i] != other.columns[i])
        return false;
    }
    return true;
  }

  bool operator!=(const DuckDBRow &other) const { return !(*this == other); }

  // Ordering operator for std::set compatibility
  // Provides strict weak ordering with NULL < non-NULL
  bool operator<(const DuckDBRow &other) const {
    if (columns.size() != other.columns.size())
      return columns.size() < other.columns.size();

    for (size_t i = 0; i < columns.size(); i++) {
      bool this_null = columns[i].IsNull();
      bool other_null = other.columns[i].IsNull();

      // NULL < non-NULL
      if (this_null && !other_null)
        return true;
      if (!this_null && other_null)
        return false;
      if (this_null && other_null)
        continue; // Both NULL, check next column

      // Both non-NULL: use DuckDB value comparison
      if (columns[i] < other.columns[i])
        return true;
      if (other.columns[i] < columns[i])
        return false;
      // Equal, continue to next column
    }
    return false; // All columns equal
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
        col_hash = NULL_HASH; // Special hash for NULL
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
// DuckDBZSet is the generic circuit Z-set specialized for DuckDB rows.
// This makes the dbsp:: circuit nodes (dbsp_circuit.hpp) operate directly on
// production data with no conversion at the boundary.
using DuckDBZSet = dbsp::ZSet<DuckDBRow, DuckDBRowHash>;

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

  // Set result (for checkpoint restore)
  virtual void set_result(const DuckDBZSet &result) = 0;

  // Get last delta (changes from most recent apply_changes)
  virtual const DuckDBZSet &get_delta() const = 0;

  // Get result schema
  virtual const TableSchema &result_schema() const = 0;

  // Get source tables this view depends on
  virtual std::vector<std::string> source_tables() const = 0;

  // Reset the view
  virtual void reset() = 0;

  virtual void
  scan(const std::function<void(const DuckDBRow &, Weight)> &callback) const {
    for (const auto &[row, weight] : get_result()) {
      callback(row, weight);
    }
  }

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
    delta_.clear();
    if (table_name != source_table_)
      return;

    for (const auto &[row, weight] : changes) {
      if (predicate_(row)) {
        result_.insert(row, weight);
        delta_.insert(row, weight);
      }
    }
    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  void set_result(const DuckDBZSet &result) override { result_ = result; version_++; }
  const DuckDBZSet &get_delta() const override { return delta_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    result_.clear();
    delta_.clear();
    version_ = 0;
  }

private:
  std::string source_table_;
  TableSchema schema_;
  PredicateFn predicate_;
  DuckDBZSet result_;
  DuckDBZSet delta_;
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
    delta_.clear();
    if (table_name != source_table_)
      return;

    for (const auto &[row, weight] : changes) {
      DuckDBRow projected = project_(row);
      result_.insert(projected, weight); // copied for delta
      delta_.insert(std::move(projected), weight);
    }
    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  void set_result(const DuckDBZSet &result) override { result_ = result; version_++; }
  const DuckDBZSet &get_delta() const override { return delta_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    result_.clear();
    delta_.clear();
    version_ = 0;
  }

private:
  std::string source_table_;
  TableSchema schema_;
  ProjectFn project_;
  DuckDBZSet result_;
  DuckDBZSet delta_;
};

// Aggregate view: SELECT key, AGG(val) FROM table GROUP BY key
class NativeAggregateView : public NativeMaterializedView {
public:
  enum class AggType { SUM, COUNT, AVG, MIN, MAX };

  using KeyFn = std::function<DuckDBRow(const DuckDBRow &)>;
  using ValueFn = std::function<duckdb::Value(const DuckDBRow &)>;
  using HavingPredicate = std::function<bool(const DuckDBRow &)>;

  NativeAggregateView(const std::string &name, const std::string &sql,
                      const std::string &source_table,
                      const TableSchema &result_schema, KeyFn key_fn,
                      ValueFn value_fn, AggType agg_type,
                      HavingPredicate having_predicate = nullptr)
      : NativeMaterializedView(name, sql), source_table_(source_table),
        schema_(result_schema), key_fn_(std::move(key_fn)),
        value_fn_(std::move(value_fn)), agg_type_(agg_type),
        having_predicate_(std::move(having_predicate)) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    if (table_name != source_table_)
      return;

    delta_.clear();

    // Group changes by key - NULL-aware
    std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> delta_sums;
    std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> delta_counts;
    std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> delta_null_counts;
    // For MIN/MAX: track individual value changes per group
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

        // Track individual values for MIN/MAX
        if (agg_type_ == AggType::MIN || agg_type_ == AggType::MAX) {
          value_changes[key].push_back({int_val, weight});
        }
      }
    }

    // Update aggregates - NULL-aware
    std::set<DuckDBRow> all_keys;
    for (const auto &[key, _] : delta_sums)
      all_keys.insert(key);
    for (const auto &[key, _] : delta_counts)
      all_keys.insert(key);
    for (const auto &[key, _] : delta_null_counts)
      all_keys.insert(key);

    for (const auto &key : all_keys) {
      auto &state = agg_states_[key];

      int64_t delta_sum = delta_sums[key];
      int64_t delta_count = delta_counts[key];
      int64_t delta_null_count = delta_null_counts[key];

      // Remove old result if it was in results (passes HAVING)
      if (state.count > 0 || state.null_count > 0) {
        DuckDBRow old_result = make_result_row(key, compute_agg(state));
        if (!having_predicate_ || having_predicate_(old_result)) {
          result_.insert(old_result, -1);
          delta_.insert(old_result, -1);
        }
      }

      // Update state
      state.sum += delta_sum;
      state.count += delta_count;
      state.null_count += delta_null_count;

      // Update MIN/MAX tracking
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

      // Add new result if count > 0 AND passes HAVING
      if (state.count > 0 || state.null_count > 0) {
        DuckDBRow new_result = make_result_row(key, compute_agg(state));
        if (!having_predicate_ || having_predicate_(new_result)) {
          result_.insert(new_result, 1);
          delta_.insert(new_result, 1);
        }
      } else {
        agg_states_.erase(key);
      }
    }

    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  void set_result(const DuckDBZSet &result) override { result_ = result; version_++; }
  const DuckDBZSet &get_delta() const override { return delta_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    agg_states_.clear();
    result_.clear();
    delta_.clear();
    version_ = 0;
  }

private:
  struct AggState {
    int64_t sum = 0;
    int64_t count = 0;             // Non-NULL count (for COUNT(column))
    int64_t null_count = 0;        // NULL count (for COUNT(*))
    std::multiset<int64_t> values; // For MIN/MAX: all individual values
  };

  duckdb::Value compute_agg(const AggState &state) const {
    switch (agg_type_) {
    case AggType::SUM:
      // SQL Standard: SUM of all NULLs returns NULL
      if (state.count == 0) {
        return duckdb::Value(duckdb::LogicalType::BIGINT); // NULL value
      }
      return duckdb::Value::BIGINT(state.sum);

    case AggType::COUNT:
      // COUNT(column) excludes NULLs, returns non-NULL count
      return duckdb::Value::BIGINT(state.count);

    case AggType::AVG:
      // SQL Standard: AVG of all NULLs returns NULL
      if (state.count == 0) {
        return duckdb::Value(duckdb::LogicalType::BIGINT); // NULL value
      }
      return duckdb::Value::BIGINT(state.sum / state.count);

    case AggType::MIN:
      if (state.values.empty()) {
        return duckdb::Value(duckdb::LogicalType::BIGINT); // NULL if no values
      }
      return duckdb::Value::BIGINT(*state.values.begin());

    case AggType::MAX:
      if (state.values.empty()) {
        return duckdb::Value(duckdb::LogicalType::BIGINT); // NULL if no values
      }
      return duckdb::Value::BIGINT(*state.values.rbegin());
    }
    return duckdb::Value::BIGINT(state.sum);
  }

  DuckDBRow make_result_row(const DuckDBRow &key,
                            const duckdb::Value &agg_val) const {
    DuckDBRow result;
    result.columns = key.columns;
    result.columns.push_back(agg_val); // Can be NULL now
    return result;
  }

  std::string source_table_;
  TableSchema schema_;
  KeyFn key_fn_;
  ValueFn value_fn_;
  AggType agg_type_;
  HavingPredicate having_predicate_;
  std::unordered_map<DuckDBRow, AggState, DuckDBRowHash> agg_states_;
  DuckDBZSet result_;
  DuckDBZSet delta_;
};

// Join view with incremental bilinear formula
// Supports multi-column JOIN keys: ON a.x = b.x AND a.y = b.y
// Also supports non-equi predicates via optional filter
class NativeJoinView : public NativeMaterializedView {
public:
  // Key function returns DuckDBRow to support composite keys
  using KeyFn = std::function<DuckDBRow(const DuckDBRow &)>;
  // Projection function for join result
  using ProjectFn = std::function<DuckDBRow(const DuckDBRow &)>;
  // Filter predicate for non-equi conditions (applied after equi-join match)
  using JoinPredicate =
      std::function<bool(const DuckDBRow &left, const DuckDBRow &right)>;
  // Filter predicate for input tables (pushed down filters)
  using FilterFn = std::function<bool(const DuckDBRow &)>;

  NativeJoinView(const std::string &name, const std::string &sql,
                 const std::string &left_table, const std::string &right_table,
                 const TableSchema &result_schema, KeyFn left_key,
                 KeyFn right_key, ProjectFn project = nullptr,
                 JoinPredicate filter = nullptr, FilterFn left_filter = nullptr,
                 FilterFn right_filter = nullptr)
      : NativeMaterializedView(name, sql), left_table_(left_table),
        right_table_(right_table), schema_(result_schema),
        left_key_(std::move(left_key)), right_key_(std::move(right_key)),
        project_(std::move(project)), filter_(std::move(filter)),
        left_filter_(std::move(left_filter)),
        right_filter_(std::move(right_filter)) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    delta_.clear();
    if (table_name == left_table_) {
      for (const auto &[row, weight] : changes) {
        if (left_filter_ && !left_filter_(row)) {
          continue;
        }

        DuckDBRow key = left_key_(row);

        // SQL Standard: NULL never matches NULL in JOINs
        // Skip if any key column is NULL
        if (has_null_column(key)) {
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
        if (right_filter_ && !right_filter_(row)) {
          continue;
        }

        DuckDBRow key = right_key_(row);

        // SQL Standard: NULL never matches NULL in JOINs
        if (has_null_column(key)) {
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
  void set_result(const DuckDBZSet &result) override { result_ = result; version_++; }
  const DuckDBZSet &get_delta() const override { return delta_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {left_table_, right_table_};
  }

  void reset() override {
    left_indexed_.clear();
    right_indexed_.clear();
    result_.clear();
    delta_.clear();
    version_ = 0;
  }

private:
  // Check if any column in the key is NULL
  static bool has_null_column(const DuckDBRow &key) {
    for (const auto &col : key.columns) {
      if (col.IsNull())
        return true;
    }
    return false;
  }

  void add_join_result(const DuckDBRow &left, const DuckDBRow &right,
                       Weight weight) {
    // Apply non-equi filter predicate if present
    if (filter_ && !filter_(left, right)) {
      return; // Filter rejects this pair
    }

    DuckDBRow joined;
    joined.columns.reserve(left.columns.size() + right.columns.size());
    for (const auto &col : left.columns) {
      joined.columns.push_back(col);
    }
    for (const auto &col : right.columns) {
      joined.columns.push_back(col);
    }

    // Apply projection if present
    if (project_) {
      DuckDBRow projected = project_(joined);
      result_.insert(projected, weight);
      delta_.insert(std::move(projected), weight);
    } else {
      result_.insert(joined, weight);
      delta_.insert(std::move(joined), weight);
    }
  }

  std::string left_table_;
  std::string right_table_;
  TableSchema schema_;
  KeyFn left_key_;
  KeyFn right_key_;
  ProjectFn project_;
  JoinPredicate filter_;
  FilterFn left_filter_;
  FilterFn right_filter_;
  // Use DuckDBRow as key for composite key support
  std::unordered_map<DuckDBRow, DuckDBZSet, DuckDBRowHash> left_indexed_;
  std::unordered_map<DuckDBRow, DuckDBZSet, DuckDBRowHash> right_indexed_;
  DuckDBZSet result_;
  DuckDBZSet delta_;
};

// Distinct view - NULL-aware (SQL Standard: multiple NULLs -> single NULL)
// Uses DuckDBRowHash and DuckDBRow::operator== which treat NULL == NULL
// NOTE: For SELECT DISTINCT col1, col2 FROM t, we project first then apply
// distinct
class NativeDistinctView : public NativeMaterializedView {
public:
  using ProjectFn = std::function<DuckDBRow(const DuckDBRow &)>;

  NativeDistinctView(const std::string &name, const std::string &sql,
                     const std::string &source_table, const TableSchema &schema,
                     ProjectFn project = nullptr)
      : NativeMaterializedView(name, sql), source_table_(source_table),
        schema_(schema), project_(std::move(project)) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    if (table_name != source_table_)
      return;

    for (const auto &[row, weight] : changes) {
      // Project if we have a projection function, otherwise use full row
      DuckDBRow projected = project_ ? project_(row) : row;

      auto it = counts_.find(projected);
      int64_t old_count = (it != counts_.end()) ? it->second : 0;
      int64_t new_count = old_count + weight;

      // Distinct logic: output +1 when 0->positive, -1 when positive->0
      bool was_present = old_count > 0;
      bool is_present = new_count > 0;

      if (!was_present && is_present) {
        result_.insert(projected, 1);
        delta_.insert(projected, 1);
      } else if (was_present && !is_present) {
        result_.insert(projected, -1);
        delta_.insert(projected, -1);
      }

      if (new_count == 0) {
        counts_.erase(projected);
      } else {
        counts_[projected] = new_count;
      }
    }
    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  void set_result(const DuckDBZSet &result) override { result_ = result; version_++; }
  const DuckDBZSet &get_delta() const override { return delta_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    counts_.clear();
    result_.clear();
    delta_.clear();
    version_ = 0;
  }

private:
  std::string source_table_;
  TableSchema schema_;
  ProjectFn project_;
  std::unordered_map<DuckDBRow, int64_t, DuckDBRowHash> counts_;
  DuckDBZSet result_;
  DuckDBZSet delta_;
};

// Sort View: ORDER BY
// Maintains a sorted multiset of rows to support ordered iteration
class NativeSortView : public NativeMaterializedView {
public:
  struct SortColumn {
    size_t column_idx;
    bool ascending;
    bool nulls_first;
  };

  using ProjectFn = std::function<DuckDBRow(const DuckDBRow &)>;

  NativeSortView(const std::string &name, const std::string &sql,
                 const std::string &source_table,
                 const TableSchema &result_schema,
                 std::vector<SortColumn> sort_columns,
                 ProjectFn project = nullptr)
      : NativeMaterializedView(name, sql), source_table_(source_table),
        schema_(result_schema), sort_columns_(std::move(sort_columns)),
        comparator_(sort_columns_), sorted_rows_(comparator_),
        project_(std::move(project)) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    delta_.clear();
    if (table_name != source_table_)
      return;

    for (const auto &[row, weight] : changes) {
      if (weight > 0) {
        for (Weight i = 0; i < weight; ++i) {
          sorted_rows_.insert(row);
        }
      } else if (weight < 0) {
        for (Weight i = 0; i > weight; --i) {
          auto it = sorted_rows_.find(row);
          if (it != sorted_rows_.end()) {
            sorted_rows_.erase(it);
          }
        }
      }

      DuckDBRow result_row = project_ ? project_(row) : row;
      result_.insert(result_row, weight);
      delta_.insert(result_row, weight);
    }
    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  void set_result(const DuckDBZSet &result) override { result_ = result; version_++; }
  const DuckDBZSet &get_delta() const override { return delta_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    sorted_rows_.clear();
    result_.clear();
    delta_.clear();
    version_ = 0;
  }

  // Custom scan that iterates in sorted order
  void scan(const std::function<void(const DuckDBRow &, Weight)> &callback)
      const override {
    if (sorted_rows_.empty())
      return;

    DuckDBRow prev = *sorted_rows_.begin();
    bool first = true;
    Weight count = 0;

    for (const auto &row : sorted_rows_) {
      if (!first && row == prev) {
        count++;
      } else {
        if (!first) {
          callback(project_ ? project_(prev) : prev, count);
        }
        prev = row;
        count = 1;
        first = false;
      }
    }
    if (!first) {
      callback(project_ ? project_(prev) : prev, count);
    }
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

        // Handle NULLs
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
        return col.ascending ? less : !less;
      }
      // Tie-breaker: full row comparison to distinguish different rows with
      // same sort key
      return a < b;
    }
  };

  std::string source_table_;
  TableSchema schema_;
  std::vector<SortColumn> sort_columns_;
  RowComparator comparator_;
  std::multiset<DuckDBRow, RowComparator> sorted_rows_;
  ProjectFn project_;
  DuckDBZSet result_;
  DuckDBZSet delta_;
};

// Limit View: LIMIT N [OFFSET M]
class NativeLimitView : public NativeMaterializedView {
public:
  using SortColumn = NativeSortView::SortColumn;

  using ProjectFn = NativeSortView::ProjectFn;

  NativeLimitView(const std::string &name, const std::string &sql,
                  const std::string &source_table,
                  const TableSchema &result_schema, int64_t limit,
                  int64_t offset, std::vector<SortColumn> sort_columns,
                  ProjectFn project = nullptr)
      : NativeMaterializedView(name, sql), source_table_(source_table),
        schema_(result_schema), limit_(limit), offset_(offset),
        sort_columns_(std::move(sort_columns)), comparator_(sort_columns_),
        sorted_rows_(comparator_), project_(std::move(project)) {
    schema_.table_name = name;
  }

  void apply_changes(const std::string &table_name,
                     const DuckDBZSet &changes) override {
    delta_.clear();
    if (table_name != source_table_)
      return;

    // Apply changes to full state
    for (const auto &[row, weight] : changes) {
      if (weight > 0) {
        for (Weight i = 0; i < weight; ++i)
          sorted_rows_.insert(row);
      } else if (weight < 0) {
        for (Weight i = 0; i > weight; --i) {
          auto it = sorted_rows_.find(row);
          if (it != sorted_rows_.end())
            sorted_rows_.erase(it);
        }
      }
    }

    // Recompute result
    DuckDBZSet new_result;

    int64_t current_idx = 0;
    int64_t count = 0;

    for (const auto &row : sorted_rows_) {
      if (current_idx >= offset_) {
        if (limit_ < 0 || count < limit_) {
          DuckDBRow result_row = project_ ? project_(row) : row;
          new_result.insert(result_row, 1);
          count++;
        } else {
          break;
        }
      }
      current_idx++;
    }

    // Compute delta (new - old)
    delta_.clear();
    // delta = new_result - result_
    // iterate new_result: if weight > old_weight, +diff.
    // iterate result_: if weight > new_weight, -diff.
    // Simpler: delta = new_result + (-result_)
    delta_ = new_result + (-result_);
    result_ = new_result;

    ++version_;
  }

  const DuckDBZSet &get_result() const override { return result_; }
  void set_result(const DuckDBZSet &result) override { result_ = result; version_++; }
  const DuckDBZSet &get_delta() const override { return delta_; }
  const TableSchema &result_schema() const override { return schema_; }
  std::vector<std::string> source_tables() const override {
    return {source_table_};
  }

  void reset() override {
    sorted_rows_.clear();
    result_.clear();
    delta_.clear();
    version_ = 0;
  }

  void scan(const std::function<void(const DuckDBRow &, Weight)> &callback)
      const override {
    // Iterate local sorted result (which is limited)
    // Since result_ is an unordered map, we cannot use it for ordered scan.
    // We must iterate the sorted_rows_ and apply limit/offset again
    // OR store the limited rows in a sorted structure too?
    // Re-iterating sorted_rows_ is O(offset+limit). Fine.

    int64_t current_idx = 0;
    int64_t count = 0;

    // We need to group identical rows to pass correct weight
    // But sorted_rows_ contains individual instances.
    // So logic is:

    // We can't easily group because limit cuts off arbitrarily.
    // e.g. 5 'A's, limit 3. We return 3 'A's.
    // Callback expects (Row, Weight).

    DuckDBRow prev;
    bool first_in_output = true;
    Weight current_weight = 0;

    for (const auto &row : sorted_rows_) {
      if (current_idx >= offset_) {
        if (limit_ < 0 || count < limit_) {
          // This row is in output
          if (first_in_output) {
            prev = row;
            current_weight = 1;
            first_in_output = false;
          } else {
            if (row == prev) {
              current_weight++;
            } else {
              callback(project_ ? project_(prev) : prev, current_weight);
              prev = row;
              current_weight = 1;
            }
          }
          count++;
        } else {
          break;
        }
      }
      current_idx++;
    }
    if (!first_in_output) {
      callback(project_ ? project_(prev) : prev, current_weight);
    }
  }

private:
  // Comparator needs to be same as SortView (duplicated for now or shared?)
  // Using nested struct from NativeSortView public interface if possible?
  // I defined RowComparator private in NativeSortView.
  // I should define it publicly or copy it.
  // I'll copy it for safety working with limited context.
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
        return col.ascending ? less : !less;
      }
      return a < b;
    }
  };

  std::string source_table_;
  TableSchema schema_;
  int64_t limit_;
  int64_t offset_;
  std::vector<SortColumn> sort_columns_;
  RowComparator comparator_;
  std::multiset<DuckDBRow, RowComparator> sorted_rows_;
  ProjectFn project_;
  DuckDBZSet result_;
  DuckDBZSet delta_;
};

} // namespace dbsp_native

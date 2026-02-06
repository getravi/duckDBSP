// DBSP Materialized View Implementation
// Provides real-time incrementally maintained materialized views

#pragma once

#include "dbsp_circuit.hpp"
#include "dbsp_stream.hpp"
#include "dbsp_zset.hpp"
#include "../dbsp_errors.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace dbsp {

// Validation result for view updates
struct ValidationResult {
    bool success = true;
    std::string error_message;
    dbsp_native::ErrorCode error_code = dbsp_native::ErrorCode::VIEW_UPDATE_FAILED;
};

// Represents a row in a table (simplified for demonstration)
// In a real implementation, this would integrate with DuckDB's Value/Vector types
struct Row {
    std::vector<std::variant<int64_t, double, std::string, bool>> columns;

    bool operator==(const Row& other) const {
        return columns == other.columns;
    }
};

// Hash function for Row
struct RowHash {
    size_t operator()(const Row& row) const noexcept {
        size_t hash = 0;
        for (const auto& col : row.columns) {
            size_t col_hash = std::visit([](const auto& v) {
                return std::hash<std::decay_t<decltype(v)>>{}(v);
            }, col);
            hash ^= col_hash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

// Hash function for pair of Rows
struct RowPairHash {
    size_t operator()(const std::pair<Row, Row>& pair) const noexcept {
        RowHash rh;
        size_t h1 = rh(pair.first);
        size_t h2 = rh(pair.second);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

// Change type for CDC (Change Data Capture)
enum class ChangeType {
    INSERT,
    DELETE,
    UPDATE  // Represented as DELETE + INSERT
};

// A change record
struct Change {
    ChangeType type;
    Row row;
    std::optional<Row> old_row;  // For UPDATE, contains the previous row
};

// Table definition
struct TableDef {
    std::string name;
    std::vector<std::string> column_names;
    std::vector<std::string> column_types;
};

// Base class for materialized views
class MaterializedView {
public:
    using RowZSet = ZSet<Row, RowHash>;

    explicit MaterializedView(std::string name)
        : name_(std::move(name)) {}

    virtual ~MaterializedView() = default;

    const std::string& name() const { return name_; }

    // Apply changes to the view
    virtual void apply_changes(const std::string& table_name, const RowZSet& changes) = 0;

    // Get the current materialized result
    virtual const RowZSet& get_result() const = 0;

    // Get the latest delta (changes since last query)
    virtual const RowZSet& get_delta() const = 0;

    // Reset the view
    virtual void reset() = 0;

    // Get statistics
    virtual size_t row_count() const = 0;
    virtual uint64_t version() const = 0;

    // Validate changes before applying (for ACID compliance)
    virtual ValidationResult validate_changes(const RowZSet& changes) {
        // Default: accept all changes
        // Subclasses can override for type checking, constraint validation, etc.
        return ValidationResult{true, "", dbsp_native::ErrorCode::VIEW_UPDATE_FAILED};
    }

protected:
    std::string name_;
};

// Simple filter materialized view: SELECT * FROM table WHERE predicate
class FilteredView : public MaterializedView {
public:
    using PredicateFn = std::function<bool(const Row&)>;

    FilteredView(std::string name, std::string source_table, PredicateFn predicate)
        : MaterializedView(std::move(name))
        , source_table_(std::move(source_table))
        , predicate_(std::move(predicate)) {}

    void apply_changes(const std::string& table_name, const RowZSet& changes) override {
        if (table_name != source_table_) return;

        delta_.clear();
        for (const auto& [row, weight] : changes) {
            if (predicate_(row)) {
                delta_.insert(row, weight);
                result_.insert(row, weight);
            }
        }
        ++version_;
    }

    const RowZSet& get_result() const override { return result_; }
    const RowZSet& get_delta() const override { return delta_; }

    void reset() override {
        result_.clear();
        delta_.clear();
        version_ = 0;
    }

    size_t row_count() const override { return result_.support_size(); }
    uint64_t version() const override { return version_; }

private:
    std::string source_table_;
    PredicateFn predicate_;
    RowZSet result_;
    RowZSet delta_;
    uint64_t version_ = 0;
};

// Projection materialized view: SELECT columns FROM table
class ProjectedView : public MaterializedView {
public:
    using ProjectionFn = std::function<Row(const Row&)>;

    ProjectedView(std::string name, std::string source_table, ProjectionFn projection)
        : MaterializedView(std::move(name))
        , source_table_(std::move(source_table))
        , projection_(std::move(projection)) {}

    void apply_changes(const std::string& table_name, const RowZSet& changes) override {
        if (table_name != source_table_) return;

        delta_.clear();
        for (const auto& [row, weight] : changes) {
            Row projected = projection_(row);
            delta_.insert(projected, weight);
            result_.insert(projected, weight);
        }
        ++version_;
    }

    const RowZSet& get_result() const override { return result_; }
    const RowZSet& get_delta() const override { return delta_; }

    void reset() override {
        result_.clear();
        delta_.clear();
        version_ = 0;
    }

    size_t row_count() const override { return result_.support_size(); }
    uint64_t version() const override { return version_; }

private:
    std::string source_table_;
    ProjectionFn projection_;
    RowZSet result_;
    RowZSet delta_;
    uint64_t version_ = 0;
};

// Join materialized view: SELECT * FROM t1 JOIN t2 ON t1.key = t2.key
// Implements incremental join using the bilinear formula:
// join^Δ(a, b) = a×b + I(z^-1(a))×b + a×I(z^-1(b))
class JoinView : public MaterializedView {
public:
    using KeyFn = std::function<std::string(const Row&)>;
    using JoinResultZSet = ZSet<std::pair<Row, Row>, RowPairHash>;
    using IndexedRows = std::unordered_map<std::string, RowZSet>;

    JoinView(std::string name,
             std::string left_table, std::string right_table,
             KeyFn left_key, KeyFn right_key)
        : MaterializedView(std::move(name))
        , left_table_(std::move(left_table))
        , right_table_(std::move(right_table))
        , left_key_(std::move(left_key))
        , right_key_(std::move(right_key)) {}

    void apply_changes(const std::string& table_name, const RowZSet& changes) override {
        flattened_delta_.clear();

        if (table_name == left_table_) {
            // Index the changes by key
            IndexedRows delta_indexed;
            for (const auto& [row, weight] : changes) {
                std::string key = left_key_(row);
                delta_indexed[key].insert(row, weight);
            }

            // Join delta_left with integrated_right
            for (const auto& [key, left_rows] : delta_indexed) {
                auto it = right_indexed_.find(key);
                if (it != right_indexed_.end()) {
                    for (const auto& [left_row, left_weight] : left_rows) {
                        for (const auto& [right_row, right_weight] : it->second) {
                            add_join_result(left_row, right_row, left_weight * right_weight);
                        }
                    }
                }
            }

            // Update integrated left
            for (const auto& [row, weight] : changes) {
                std::string key = left_key_(row);
                left_indexed_[key].insert(row, weight);
                if (left_indexed_[key].empty()) {
                    left_indexed_.erase(key);
                }
            }
        } else if (table_name == right_table_) {
            // Index the changes by key
            IndexedRows delta_indexed;
            for (const auto& [row, weight] : changes) {
                std::string key = right_key_(row);
                delta_indexed[key].insert(row, weight);
            }

            // Join integrated_left with delta_right
            for (const auto& [key, right_rows] : delta_indexed) {
                auto it = left_indexed_.find(key);
                if (it != left_indexed_.end()) {
                    for (const auto& [left_row, left_weight] : it->second) {
                        for (const auto& [right_row, right_weight] : right_rows) {
                            add_join_result(left_row, right_row, left_weight * right_weight);
                        }
                    }
                }
            }

            // Update integrated right
            for (const auto& [row, weight] : changes) {
                std::string key = right_key_(row);
                right_indexed_[key].insert(row, weight);
                if (right_indexed_[key].empty()) {
                    right_indexed_.erase(key);
                }
            }
        }
        ++version_;
    }

    const RowZSet& get_result() const override { return flattened_result_; }
    const RowZSet& get_delta() const override { return flattened_delta_; }

    void reset() override {
        left_indexed_.clear();
        right_indexed_.clear();
        flattened_result_.clear();
        flattened_delta_.clear();
        version_ = 0;
    }

    size_t row_count() const override { return flattened_result_.support_size(); }
    uint64_t version() const override { return version_; }

private:
    void add_join_result(const Row& left, const Row& right, Weight weight) {
        // Flatten the pair into a single row
        Row flattened;
        flattened.columns.reserve(left.columns.size() + right.columns.size());
        for (const auto& col : left.columns) {
            flattened.columns.push_back(col);
        }
        for (const auto& col : right.columns) {
            flattened.columns.push_back(col);
        }
        flattened_delta_.insert(flattened, weight);
        flattened_result_.insert(flattened, weight);
    }

    std::string left_table_;
    std::string right_table_;
    KeyFn left_key_;
    KeyFn right_key_;
    IndexedRows left_indexed_;   // I(left) indexed by key
    IndexedRows right_indexed_;  // I(right) indexed by key
    RowZSet flattened_result_;
    RowZSet flattened_delta_;
    uint64_t version_ = 0;
};

// Aggregate materialized view: SELECT key, AGG(value) FROM table GROUP BY key
class AggregateView : public MaterializedView {
public:
    using KeyFn = std::function<Row(const Row&)>;  // Extract grouping key
    using ValueFn = std::function<int64_t(const Row&)>;  // Extract value for aggregation

    enum class AggType { SUM, COUNT, AVG, MIN, MAX };

    AggregateView(std::string name, std::string source_table,
                  KeyFn key_fn, ValueFn value_fn, AggType agg_type)
        : MaterializedView(std::move(name))
        , source_table_(std::move(source_table))
        , key_fn_(std::move(key_fn))
        , value_fn_(std::move(value_fn))
        , agg_type_(agg_type) {}

    void apply_changes(const std::string& table_name, const RowZSet& changes) override {
        if (table_name != source_table_) return;

        delta_.clear();

        // Group changes by key
        std::unordered_map<Row, int64_t, RowHash> delta_values;
        std::unordered_map<Row, int64_t, RowHash> delta_counts;

        for (const auto& [row, weight] : changes) {
            Row key = key_fn_(row);
            int64_t value = value_fn_(row);

            delta_values[key] += value * weight;
            delta_counts[key] += weight;
        }

        // Apply aggregation logic
        for (const auto& [key, delta_val] : delta_values) {
            auto& state = agg_states_[key];
            int64_t delta_cnt = delta_counts[key];

            // Remove old result if exists
            if (state.count > 0) {
                Row old_result = make_result_row(key, compute_agg(state));
                result_.insert(old_result, -1);
                delta_.insert(old_result, -1);
            }

            // Update state
            state.sum += delta_val;
            state.count += delta_cnt;

            // Add new result if count > 0
            if (state.count > 0) {
                Row new_result = make_result_row(key, compute_agg(state));
                result_.insert(new_result, 1);
                delta_.insert(new_result, 1);
            } else {
                // Remove the state if count is 0
                agg_states_.erase(key);
            }
        }

        ++version_;
    }

    const RowZSet& get_result() const override { return result_; }
    const RowZSet& get_delta() const override { return delta_; }

    void reset() override {
        agg_states_.clear();
        result_.clear();
        delta_.clear();
        version_ = 0;
    }

    size_t row_count() const override { return result_.support_size(); }
    uint64_t version() const override { return version_; }

private:
    struct AggState {
        int64_t sum = 0;
        int64_t count = 0;
        int64_t min = std::numeric_limits<int64_t>::max();
        int64_t max = std::numeric_limits<int64_t>::min();
    };

    int64_t compute_agg(const AggState& state) const {
        switch (agg_type_) {
            case AggType::SUM:
                return state.sum;
            case AggType::COUNT:
                return state.count;
            case AggType::AVG:
                return state.count > 0 ? state.sum / state.count : 0;
            case AggType::MIN:
                return state.min;
            case AggType::MAX:
                return state.max;
        }
        return 0;
    }

    Row make_result_row(const Row& key, int64_t agg_value) const {
        Row result = key;
        result.columns.push_back(agg_value);
        return result;
    }

    std::string source_table_;
    KeyFn key_fn_;
    ValueFn value_fn_;
    AggType agg_type_;
    std::unordered_map<Row, AggState, RowHash> agg_states_;
    RowZSet result_;
    RowZSet delta_;
    uint64_t version_ = 0;
};

// Distinct materialized view: SELECT DISTINCT * FROM table
class DistinctView : public MaterializedView {
public:
    DistinctView(std::string name, std::string source_table)
        : MaterializedView(std::move(name))
        , source_table_(std::move(source_table)) {}

    void apply_changes(const std::string& table_name, const RowZSet& changes) override {
        if (table_name != source_table_) return;

        delta_ = distinct_op_.process(changes);
        for (const auto& [row, weight] : delta_) {
            result_.insert(row, weight);
        }
        ++version_;
    }

    const RowZSet& get_result() const override { return result_; }
    const RowZSet& get_delta() const override { return delta_; }

    void reset() override {
        distinct_op_.reset();
        result_.clear();
        delta_.clear();
        version_ = 0;
    }

    size_t row_count() const override { return result_.support_size(); }
    uint64_t version() const override { return version_; }

private:
    std::string source_table_;
    IncrementalDistinct<Row, RowHash> distinct_op_;
    RowZSet result_;
    RowZSet delta_;
    uint64_t version_ = 0;
};

// Manager for all materialized views
class MaterializedViewManager {
public:
    using RowZSet = ZSet<Row, RowHash>;

    MaterializedViewManager() = default;

    // Register a new materialized view
    void register_view(std::unique_ptr<MaterializedView> view) {
        std::lock_guard<std::mutex> lock(mutex_);
        views_[view->name()] = std::move(view);
    }

    // Get a view by name
    MaterializedView* get_view(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = views_.find(name);
        return it != views_.end() ? it->second.get() : nullptr;
    }

    // Apply changes to all relevant views
    void apply_changes(const std::string& table_name, const RowZSet& changes) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, view] : views_) {
            view->apply_changes(table_name, changes);
        }
    }

    // Convenience method to apply a single row change
    void insert_row(const std::string& table_name, const Row& row) {
        RowZSet changes;
        changes.insert(row, 1);
        apply_changes(table_name, changes);
    }

    void delete_row(const std::string& table_name, const Row& row) {
        RowZSet changes;
        changes.insert(row, -1);
        apply_changes(table_name, changes);
    }

    void update_row(const std::string& table_name, const Row& old_row, const Row& new_row) {
        RowZSet changes;
        changes.insert(old_row, -1);
        changes.insert(new_row, 1);
        apply_changes(table_name, changes);
    }

    // Get all view names
    std::vector<std::string> list_views() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        for (const auto& [name, view] : views_) {
            names.push_back(name);
        }
        return names;
    }

    // Remove a view
    bool remove_view(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        return views_.erase(name) > 0;
    }

    // Reset all views
    void reset_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, view] : views_) {
            view->reset();
        }
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<MaterializedView>> views_;
};

} // namespace dbsp

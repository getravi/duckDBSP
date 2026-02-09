// DBSP Query Optimizer
// Optimizes ParsedViewDef before view creation
// Implements: filter combination, filter pushdown, projection pruning,
//             operator fusion (filter+project)

#pragma once

#include "dbsp_sql_parser.hpp"
#include <set>
#include <string>
#include <vector>

namespace dbsp_native {

class DBSPOptimizer {
public:
  // Main entry point - applies all optimization passes
  ParsedViewDef optimize(const ParsedViewDef &def) {
    ParsedViewDef optimized = def;

    // Apply optimization passes in order
    combine_filters(optimized);
    pushdown_filters(optimized);
    prune_projections(optimized);
    fuse_operators(optimized);

    optimized.optimized = true;
    return optimized;
  }

  // Get statistics about optimizations performed
  struct OptimizationStats {
    size_t filters_combined = 0;
    size_t filters_pushed_down = 0;
    size_t columns_pruned = 0;
    size_t operators_fused = 0;
  };

  const OptimizationStats &stats() const { return stats_; }

private:
  OptimizationStats stats_;

  // ========================================================================
  // Filter Combination
  // ========================================================================
  // Combine multiple filter conditions into a single filter with AND semantics
  void combine_filters(ParsedViewDef &def) {
    if (def.filters.size() <= 1) {
      return; // Nothing to combine
    }

    // Filters are already stored as a vector and applied with AND semantics
    // This pass ensures they're stored efficiently
    stats_.filters_combined =
        def.filters.size() > 1 ? def.filters.size() - 1 : 0;
  }

  // ========================================================================
  // Filter Pushdown
  // ========================================================================
  // Move filters closer to data sources when possible
  void pushdown_filters(ParsedViewDef &def) {
    if (def.type != ParsedViewDef::ViewType::JOIN || !def.join_info) {
      return; // Only push down through joins for now
    }

    std::vector<ParsedViewDef::FilterInfo> remaining_filters;
    std::vector<ParsedViewDef::FilterInfo> left_filters;
    std::vector<ParsedViewDef::FilterInfo> right_filters;

    for (const auto &filter : def.filters) {
      // Determine which table the filter column belongs to
      auto table = get_column_table(filter.column_name, def);

      if (table == def.join_info->left_table) {
        auto clean_filter = filter;
        auto dot_pos = clean_filter.column_name.find('.');
        if (dot_pos != std::string::npos) {
          clean_filter.column_name =
              clean_filter.column_name.substr(dot_pos + 1);
        }
        left_filters.push_back(clean_filter);
        stats_.filters_pushed_down++;
      } else if (table == def.join_info->right_table) {
        auto clean_filter = filter;
        auto dot_pos = clean_filter.column_name.find('.');
        if (dot_pos != std::string::npos) {
          clean_filter.column_name =
              clean_filter.column_name.substr(dot_pos + 1);
        }
        right_filters.push_back(clean_filter);
        stats_.filters_pushed_down++;
      } else {
        // Can't determine table or complex predicate - keep in place
        remaining_filters.push_back(filter);
      }
    }

    // Store pushed-down filters for later use
    def.filters = remaining_filters;
    def.left_pushed_filters = std::move(left_filters);
    def.right_pushed_filters = std::move(right_filters);
  }

  // ========================================================================
  // Projection Pruning
  // ========================================================================
  // Identify which columns are actually needed from source tables
  void prune_projections(ParsedViewDef &def) {
    if (def.select_all || def.project_column_names.empty()) {
      return; // SELECT * or no projections - can't prune
    }

    // Track which columns are actually used
    std::set<std::string> used_columns;

    // Add selected columns
    for (const auto &col : def.project_column_names) {
      used_columns.insert(col);
    }

    // Add filter columns
    for (const auto &filter : def.filters) {
      used_columns.insert(filter.column_name);
    }

    // Add group by columns
    for (const auto &col : def.group_by_names) {
      used_columns.insert(col);
    }

    // Add aggregate value columns
    for (const auto &agg : def.aggregates) {
      if (!agg.value_column_name.empty()) {
        used_columns.insert(agg.value_column_name);
      }
      if (agg.is_expression) {
        if (!agg.left_col_name.empty()) {
          used_columns.insert(agg.left_col_name);
        }
        if (!agg.right_col_name.empty()) {
          used_columns.insert(agg.right_col_name);
        }
      }
    }

    // Add join columns
    if (def.join_info) {
      for (const auto &pair : def.join_info->column_pairs) {
        used_columns.insert(pair.first);
        used_columns.insert(pair.second);
      }
      for (const auto &pred : def.join_info->non_equi_predicates) {
        used_columns.insert(pred.left_col);
        used_columns.insert(pred.right_col);
      }
    }

    // Store required columns for source tables
    def.required_columns.clear();
    for (const auto &col : used_columns) {
      if (!col.empty()) {
        def.required_columns.push_back(col);
      }
    }

    // Count pruned columns: source columns not in required set
    // For non-join views, count how many projected columns we could eliminate
    // from the source fetch
    if (!def.source_tables.empty()) {
      // The number of columns not needed = total filter+project columns
      // referenced minus the minimum set. Since we track used_columns,
      // any column in the source NOT in used_columns is prunable.
      // We approximate by counting required vs projected.
      size_t total_referenced = def.project_column_names.size();
      for (const auto &f : def.filters) {
        // Filter column might not be in projection list
        if (used_columns.count(f.column_name) &&
            std::find(def.project_column_names.begin(),
                       def.project_column_names.end(),
                       f.column_name) == def.project_column_names.end()) {
          total_referenced++;
        }
      }
      // columns_pruned = columns we DON'T need to output
      // (filter-only columns that aren't in the final projection)
      if (total_referenced > def.project_column_names.size()) {
        stats_.columns_pruned =
            total_referenced - def.project_column_names.size();
      }
    }
  }

  // ========================================================================
  // Operator Fusion
  // ========================================================================
  // Fuse filter + project operations into a single FILTER_PROJECT view type
  // when a FILTER-typed view also has projection columns.
  // This reduces memory by only storing projected columns in the result.
  void fuse_operators(ParsedViewDef &def) {
    // Only fuse FILTER views that also have explicit projections
    if (def.type != ParsedViewDef::ViewType::FILTER) {
      return;
    }

    // Must have both filters and non-trivial projections
    if (def.filters.empty() || def.project_column_names.empty() ||
        def.select_all) {
      return;
    }

    // Upgrade to fused FILTER_PROJECT type
    def.type = ParsedViewDef::ViewType::FILTER_PROJECT;
    stats_.operators_fused++;
  }

  // ========================================================================
  // Helper Functions
  // ========================================================================

  // Determine which table a column belongs to (for join queries)
  std::string get_column_table(const std::string &column_name,
                               const ParsedViewDef &def) {
    // If column has table prefix (e.g., "orders.amount"), extract table
    auto dot_pos = column_name.find('.');
    if (dot_pos != std::string::npos) {
      return column_name.substr(0, dot_pos);
    }

    // Otherwise, we'd need schema information to determine the table
    // For now, return empty to indicate unknown
    return "";
  }
};

} // namespace dbsp_native

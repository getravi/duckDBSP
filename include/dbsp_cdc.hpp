// DBSP Change Data Capture (CDC) Manager
// Automatically tracks changes to DuckDB tables and propagates to views
// Supports cascading views (views on views) and persistence

#pragma once

#include "dbsp_duckdb_types.hpp"
#include "dbsp_plan_translator.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dbsp_native {

// Marks queries DBSP issues on internal helper connections. Transaction hooks
// must not recurse into CDCManager for these (deadlock: the issuing thread may
// already hold struct_mutex_, which is not recursive).
inline thread_local int internal_query_depth = 0;

struct InternalQueryGuard {
  InternalQueryGuard() { ++internal_query_depth; }
  ~InternalQueryGuard() { --internal_query_depth; }
  InternalQueryGuard(const InternalQueryGuard &) = delete;
  InternalQueryGuard &operator=(const InternalQueryGuard &) = delete;
};

// Security validation functions

// Enhanced identifier validation with detailed error codes
inline bool validate_identifier(const std::string &name, std::string &error_msg,
                                ErrorCode &error_code) {
  error_msg.clear();
  error_code = ErrorCode::INVALID_IDENTIFIER;

  if (name.empty()) {
    error_msg = "Identifier cannot be empty";
    error_code = ErrorCode::INVALID_IDENTIFIER;
    return false;
  }

  if (name.length() > 255) {
    error_msg = "Identifier too long (max 255 characters): " +
                std::to_string(name.length());
    error_code = ErrorCode::IDENTIFIER_TOO_LONG;
    return false;
  }

  // First character must be letter or underscore
  if (!std::isalpha(name[0]) && name[0] != '_') {
    error_msg =
        "Identifier must start with letter or underscore: '" + name + "'";
    error_code = ErrorCode::INVALID_IDENTIFIER;
    return false;
  }

  // Remaining characters must be alphanumeric or underscore
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    if (!std::isalnum(c) && c != '_') {
      error_msg = "Identifier contains invalid character '" +
                  std::string(1, c) + "' at position " + std::to_string(i) +
                  ": '" + name + "'";
      error_code = ErrorCode::INVALID_IDENTIFIER;
      return false;
    }
  }

  return true;
}

// Legacy function for backward compatibility
inline bool is_valid_identifier(const std::string &name) {
  std::string error_msg;
  ErrorCode error_code;
  return validate_identifier(name, error_msg, error_code);
}

// Validate and canonicalize file path to prevent path traversal
// Returns canonicalized path if valid, empty string if invalid
inline std::string validate_filepath(const std::string &filepath) {
  // Check for null bytes
  if (filepath.find('\0') != std::string::npos) {
    return "";
  }

  // Check for path traversal patterns
  if (filepath.find("..") != std::string::npos) {
    return "";
  }

  // Must not be absolute path starting with /
  if (!filepath.empty() && filepath[0] == '/') {
    return "";
  }

  // Must not start with ~ (home directory)
  if (!filepath.empty() && filepath[0] == '~') {
    return "";
  }

  // Check for Windows-style paths on all platforms for consistency
  // Reject drive letters (C:\, D:\, etc.)
  if (filepath.length() >= 2 && filepath[1] == ':') {
    return ""; // Reject C:\ style paths
  }

  // Reject backslashes (used in Windows paths and potential traversal)
  if (filepath.find('\\') != std::string::npos) {
    return ""; // Reject backslashes
  }

  // The string checks above cannot see symlinks: a relative path through a
  // link pointing outside the working directory would escape. Resolve the
  // path (weakly_canonical follows links in existing components and
  // tolerates a not-yet-created tail) and require it to stay under the
  // canonical working directory.
  try {
    namespace fs = std::filesystem;
    const fs::path base = fs::canonical(fs::current_path());
    const fs::path resolved = fs::weakly_canonical(base / filepath);
    const auto rel = resolved.lexically_relative(base);
    if (rel.empty() || rel.native().rfind("..", 0) == 0) {
      return "";
    }
  } catch (const std::exception &) {
    return ""; // unresolvable path: reject
  }

  // Valid relative path confined to the working directory
  return filepath;
}

// View definition for persistence
struct ViewDefinition {
  std::string name;
  std::string sql;
  std::vector<std::string> source_tables; // Can include other views
  uint64_t created_at;
};

// Dependency graph for cascading views
class DependencyGraph {
public:
  // Add a dependency: dependent depends on dependency
  void add_dependency(const std::string &dependent,
                      const std::string &dependency) {
    dependencies_[dependent].insert(dependency);
    dependents_[dependency].insert(dependent);
  }

  // Remove all dependencies for a node
  void remove_node(const std::string &node) {
    // Remove from dependents of its dependencies
    if (dependencies_.count(node)) {
      for (const auto &dep : dependencies_[node]) {
        dependents_[dep].erase(node);
      }
      dependencies_.erase(node);
    }

    // Remove from dependencies of its dependents
    if (dependents_.count(node)) {
      for (const auto &dep : dependents_[node]) {
        dependencies_[dep].erase(node);
      }
      dependents_.erase(node);
    }
  }

  // Get all nodes that depend on the given node (direct and transitive)
  std::vector<std::string> get_all_dependents(const std::string &node) const {
    std::vector<std::string> result;
    std::unordered_set<std::string> visited;
    std::queue<std::string> queue;

    auto it = dependents_.find(node);
    if (it != dependents_.end()) {
      for (const auto &dep : it->second) {
        queue.push(dep);
      }
    }

    while (!queue.empty()) {
      std::string current = queue.front();
      queue.pop();

      if (visited.count(current))
        continue;
      visited.insert(current);
      result.push_back(current);

      auto dep_it = dependents_.find(current);
      if (dep_it != dependents_.end()) {
        for (const auto &dep : dep_it->second) {
          if (!visited.count(dep)) {
            queue.push(dep);
          }
        }
      }
    }

    return result;
  }

  // Get topological order for updating dependents of a node
  // Returns nodes in order they should be updated
  std::vector<std::string>
  topological_order(const std::string &changed_node) const {
    std::vector<std::string> dependents = get_all_dependents(changed_node);
    if (dependents.empty())
      return {};

    // Build in-degree map for affected nodes
    std::unordered_map<std::string, int> in_degree;
    std::unordered_set<std::string> affected(dependents.begin(),
                                             dependents.end());

    for (const auto &node : dependents) {
      in_degree[node] = 0;
    }

    for (const auto &node : dependents) {
      auto it = dependencies_.find(node);
      if (it != dependencies_.end()) {
        for (const auto &dep : it->second) {
          // Only count dependencies within the affected set
          // Don't count changed_node since it's already processed
          if (affected.count(dep)) {
            in_degree[node]++;
          }
        }
      }
    }

    // Kahn's algorithm
    std::vector<std::string> result;
    std::queue<std::string> queue;

    for (const auto &[node, degree] : in_degree) {
      if (degree == 0) {
        queue.push(node);
      }
    }

    while (!queue.empty()) {
      std::string current = queue.front();
      queue.pop();
      result.push_back(current);

      auto it = dependents_.find(current);
      if (it != dependents_.end()) {
        for (const auto &dep : it->second) {
          if (in_degree.count(dep)) {
            in_degree[dep]--;
            if (in_degree[dep] == 0) {
              queue.push(dep);
            }
          }
        }
      }
    }

    return result;
  }

  // Check if adding a dependency would create a cycle
  bool would_create_cycle(const std::string &dependent,
                          const std::string &dependency) const {
    // Check if dependent is reachable from dependency
    std::unordered_set<std::string> visited;
    std::queue<std::string> queue;
    queue.push(dependent);

    while (!queue.empty()) {
      std::string current = queue.front();
      queue.pop();

      if (current == dependency)
        return true;
      if (visited.count(current))
        continue;
      visited.insert(current);

      auto it = dependents_.find(current);
      if (it != dependents_.end()) {
        for (const auto &dep : it->second) {
          queue.push(dep);
        }
      }
    }

    return false;
  }

  // Get direct dependencies of a node
  std::vector<std::string> get_dependencies(const std::string &node) const {
    std::vector<std::string> result;
    auto it = dependencies_.find(node);
    if (it != dependencies_.end()) {
      result.assign(it->second.begin(), it->second.end());
    }
    return result;
  }

private:
  std::unordered_map<std::string, std::unordered_set<std::string>>
      dependencies_; // node -> what it depends on
  std::unordered_map<std::string, std::unordered_set<std::string>>
      dependents_; // node -> what depends on it
};

// CDC Manager - coordinates tracked tables, views, and change propagation
//
// Three-tier locking (always acquire in this order — NEVER reverse):
//   Tier 1: struct_mutex_  — guards map membership (track/untrack tables,
//           create/drop views, dep_graph_, table_schemas_, view_definitions_,
//           auto_sync_enabled_, use_parallel_sync_)
//   Tier 2: table_locks_[name] — one per tracked table; guards that table's
//           TrackedTable state (current_state_, pending_changes_)
//   Tier 3: view_mutex_  — guards view content (Z-set state, propagation)
//
class CDCManager {
public:
  CDCManager() = default;

  // Reset all state (for testing)
  void reset() {
    std::unique_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::unique_lock<std::shared_mutex> view_lock(view_mutex_);
    tracked_tables_.clear();
    table_locks_.clear();
    views_.clear();
    table_schemas_.clear();
    view_definitions_.clear();
    dep_graph_ = DependencyGraph();
    arrangements_.clear();
    arrangements_by_table_.clear();
    last_error_.clear();
    auto_sync_enabled_ = false;
  }

  // Auto-sync (automatic CDC) control. Flag is atomic so transaction hooks
  // can check it without touching struct_mutex_ (hooks may fire on threads
  // that already hold it).
  void enable_auto_sync() { auto_sync_enabled_ = true; }

  void disable_auto_sync() { auto_sync_enabled_ = false; }

  bool is_auto_sync_enabled() const { return auto_sync_enabled_; }

  bool parallel_sync_enabled() const { return use_parallel_sync_; }

  // The planner frontend is the only frontend since Phase C5 (the bespoke
  // parser was deleted). Kept so the dbsp_use_planner() table function stays
  // callable; toggling is a no-op.
  bool is_planner_enabled() const { return true; }

  // Auto-sync all tracked tables (called from TransactionCommit hook)
  void auto_sync_all(duckdb::ClientContext &context,
                     duckdb::MetaTransaction *meta_transaction = nullptr) {
    if (!is_auto_sync_enabled())
      return;
    sync_all(context, meta_transaction);
  }

  // Track a DuckDB table for CDC
  bool track_table(duckdb::ClientContext &context,
                   const std::string &table_name) {
    std::unique_lock<std::shared_mutex> lock(struct_mutex_);

    // Validate table name to prevent SQL injection
    if (!is_valid_identifier(table_name)) {
      last_error_ =
          "Invalid table name (must be alphanumeric/underscore only): " +
          table_name;
      return false;
    }

    if (tracked_tables_.count(table_name)) {
      return true; // Already tracked
    }

    TableSchema schema;
    if (!get_table_schema(context, table_name, schema)) {
      return false;
    }

    tracked_tables_[table_name] =
        std::make_unique<TrackedTable>(table_name, schema);
    table_schemas_[table_name] = schema;
    table_locks_[table_name] = std::make_unique<std::shared_mutex>();

    // Note: Initial table sync deferred to avoid SQL query deadlock
    // User should call dbsp_sync() after dbsp_track() to populate initial data

    return true;
  }

  // Untrack a table
  void untrack_table(const std::string &table_name) {
    std::unique_lock<std::shared_mutex> lock(struct_mutex_);
    tracked_tables_.erase(table_name);
    table_schemas_.erase(table_name);
    table_locks_.erase(table_name);
    dep_graph_.remove_node(table_name);
  }

  // Create a materialized view from SQL
  // Supports referencing other views (cascading views)
  bool create_view(duckdb::ClientContext &context, const std::string &view_name,
                   const std::string &sql) {
    std::unique_lock<std::shared_mutex> struct_lock(struct_mutex_);

    // Validate view name to prevent SQL injection
    if (!is_valid_identifier(view_name)) {
      last_error_ =
          "Invalid view name (must be alphanumeric/underscore only): " +
          view_name;
      return false;
    }

    // Check if view already exists
    if (views_.count(view_name)) {
      last_error_ = "View already exists: " + view_name;
      return false;
    }

    // Planner frontend (the only frontend since Phase C5): view SQL is
    // parsed/bound/planned by DuckDB itself and translated into a circuit.
    // Translation failure is a hard error carrying the DBSP-E110 (or binder)
    // message — the bespoke SQL parser was deleted once the planner covered
    // everything it did.
    std::unique_ptr<NativeMaterializedView> view;
    {
      InternalQueryGuard guard;
      // MVs aren't in DuckDB's catalog: hand the translator their schemas so
      // views-on-views bind (shadowed as empty temp tables during ExtractPlan)
      std::vector<std::pair<std::string, TableSchema>> mv_schemas;
      mv_schemas.reserve(views_.size());
      for (const auto &[existing_name, existing_view] : views_) {
        mv_schemas.emplace_back(existing_name, existing_view->result_schema());
      }
      auto translated =
          PlanTranslator::translate(context, view_name, sql, mv_schemas);
      view = std::move(translated.view);
      if (!view) {
        last_error_ = translated.error;
        return false;
      }
    }

    // Resolve sources - can be tables or other views
    const std::vector<std::string> requested_sources = view->source_tables();
    std::vector<std::string> resolved_sources;
    for (const auto &source : requested_sources) {
      // A view can reference the same table more than once (e.g. the anchor
      // and recursive parts of WITH RECURSIVE both scan the base table).
      // Resolve each source only once — the initialization loop below
      // applies table state once per resolved source, so a duplicate here
      // would double the view's initial contents.
      if (std::find(resolved_sources.begin(), resolved_sources.end(),
                    source) != resolved_sources.end()) {
        continue;
      }
      // Check for cycles
      if (dep_graph_.would_create_cycle(view_name, source)) {
        last_error_ =
            "Circular dependency detected: " + view_name + " -> " + source;
        return false;
      }

      // Check if source is a view
      if (views_.count(source)) {
        // Source is another view - add to schema from view's result schema
        table_schemas_[source] = views_[source]->result_schema();
        resolved_sources.push_back(source);
      } else if (tracked_tables_.count(source)) {
        // Source is a tracked table
        resolved_sources.push_back(source);
      } else {
        // Try to auto-track as a table
        if (!track_table_internal(context, source)) {
          last_error_ = "Could not find source: " + source;
          return false;
        }
        resolved_sources.push_back(source);
      }
    }

    // Add dependencies
    for (const auto &source : resolved_sources) {
      dep_graph_.add_dependency(view_name, source);
    }

    // Store view definition for persistence
    ViewDefinition def;
    def.name = view_name;
    def.sql = sql;
    def.source_tables = resolved_sources;
    def.created_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    view_definitions_[view_name] = def;

    // Save to _dbsp_views table (ignore errors for now)
    try {
      std::string sources_str;
      for (size_t i = 0; i < resolved_sources.size(); i++) {
        if (i > 0) sources_str += ",";
        sources_str += resolved_sources[i];
      }

      // Escape single quotes in SQL
      std::string escaped_sql = sql;
      size_t pos = 0;
      while ((pos = escaped_sql.find("'", pos)) != std::string::npos) {
        escaped_sql.replace(pos, 1, "''");
        pos += 2;
      }

      std::string insert_sql =
        "INSERT OR REPLACE INTO _dbsp_views (name, sql, sources, created_at) "
        "VALUES ('" + view_name + "', '" + escaped_sql + "', '" + sources_str + "', " +
        std::to_string(def.created_at) + ")";

      // Use a fresh connection: `context` is mid-query (we run inside a table
      // function on it), so context.Query() would block on the context lock.
      InternalQueryGuard guard;
      duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
      auto res = con.Query(insert_sql);
      // Ignore errors - persistence is best-effort
    } catch (const std::exception &e) {
      // Ignore persistence errors - log for debugging
      last_error_ = std::string("Persistence error: ") + e.what();
    } catch (...) {
      last_error_ = "Unknown persistence error";
    }

    // Register the view's result schema for dependent views
    table_schemas_[view_name] = view->result_schema();

    // Acquire view_mutex_ to initialize view content and insert into views_
    // Lock ordering: struct_mutex_ (already held) → view_mutex_
    {
      std::unique_lock<std::shared_mutex> view_lock(view_mutex_);

      // I1: resolve shared-arrangement requests before initialization —
      // a shared join side reads a CDC-maintained arrangement instead of
      // integrating its own copy, and its source table is then EXCLUDED
      // from init replay (the arrangement already holds full state;
      // replaying it would double-count through Δother ⋈ arrangement)
      auto *pview = dynamic_cast<PlannedCircuitView *>(view.get());
      if (pview) {
        register_arrangements(*pview);
      }

      // Initialize view with current data from sources
      // struct_mutex_ held exclusively so no concurrent mutations to tracked
      // tables or other views can occur; reads are safe without table_locks_
      for (const auto &source : resolved_sources) {
        if (pview && pview->shared_init_skip().count(source)) {
          continue;
        }
        if (tracked_tables_.count(source)) {
          const auto &table = tracked_tables_.at(source);
          const auto &state = table->current_state();
          view->apply_changes(source, state);
        } else if (views_.count(source)) {
          // Source is a view - use its result
          view->apply_changes(source, views_[source]->get_result());
        }
      }

      views_[view_name] = std::move(view);
    }

    return true;
  }

  // I1 shared join arrangements: one arrangement per (table, key exprs,
  // flags) fingerprint, shared by every join side that matches it. The
  // registry holds weak refs — nodes own the arrangement; when the last
  // consuming view is dropped it dies and the entry is pruned lazily.
  // Caller holds struct_mutex_ (exclusive) and view_mutex_ (exclusive).
  void register_arrangements(PlannedCircuitView &pview) {
    for (const auto &req : pview.arrangement_requests()) {
      const bool source_is_table = tracked_tables_.count(req.table) > 0;
      const bool source_is_view = views_.count(req.table) > 0;
      if (!source_is_table && !source_is_view) {
        continue; // untracked — node keeps its local index
      }
      std::shared_ptr<SharedArrangement> arr;
      auto it = arrangements_.find(req.fingerprint);
      if (it != arrangements_.end()) {
        arr = it->second.lock();
      }
      if (!arr) {
        arr = std::make_shared<SharedArrangement>();
        arr->table = req.table;
        arr->null_safe = req.null_safe;
        arr->track_weights = req.track_weights;
        arr->track_counters = req.track_counters;
        arr->project = req.project;
        arr->column_idxs = req.column_idxs;
        arr->keep_alive = req.keep_alive;
        if (!req.key_exprs.empty()) {
          arr->key_eval = std::make_unique<BatchEvaluator>(
              req.keep_alive, req.key_exprs, req.side_types);
          for (const auto *e : req.key_exprs) {
            arr->row_key_evals.push_back(std::make_unique<RowExprEval>(
                req.keep_alive, *e, req.side_types));
          }
        }
        // Backfill full current state so init replay can skip this table
        if (source_is_table) {
          arr->apply(tracked_tables_.at(req.table)->current_state());
        } else {
          arr->apply(views_.at(req.table)->get_result());
        }
        arrangements_[req.fingerprint] = arr;
        arrangements_by_table_[req.table].push_back(arr);
      }
      req.node->set_shared_arrangement(req.left_side, arr);
      if (req.init_skip) {
        pview.mark_shared_init_skip(req.table);
      }
    }
  }

  // Drop a view (with persistence)
  bool drop_view(const std::string &view_name, duckdb::ClientContext *context = nullptr) {
    std::unique_lock<std::shared_mutex> struct_lock(struct_mutex_);

    // Validate view name to prevent SQL injection
    if (!is_valid_identifier(view_name)) {
      last_error_ = "Invalid view name (must be alphanumeric/underscore only): " + view_name;
      return false;
    }

    // Check if other views depend on this one
    auto dependents = dep_graph_.get_all_dependents(view_name);
    if (!dependents.empty()) {
      last_error_ =
          "Cannot drop view: other views depend on it: " + dependents[0];
      return false;
    }

    // Delete from _dbsp_views table if context provided
    if (context) {
      try {
        std::string delete_sql = "DELETE FROM _dbsp_views WHERE name = '" + view_name + "'";
        auto res = context->Query(delete_sql, false);
        // Ignore errors - persistence is best-effort
      } catch (const std::exception &e) {
        // Ignore persistence errors - logged for debugging
      } catch (...) {
        // Ignore unknown persistence errors
      }
    }

    std::unique_lock<std::shared_mutex> view_lock(view_mutex_);
    dep_graph_.remove_node(view_name);
    view_definitions_.erase(view_name);
    table_schemas_.erase(view_name);
    return views_.erase(view_name) > 0;
  }

  // Force drop a view and all dependents (with persistence)
  bool drop_view_cascade(const std::string &view_name, duckdb::ClientContext *context = nullptr) {
    std::unique_lock<std::shared_mutex> struct_lock(struct_mutex_);

    // Validate view name to prevent SQL injection
    if (!is_valid_identifier(view_name)) {
      last_error_ = "Invalid view name (must be alphanumeric/underscore only): " + view_name;
      return false;
    }

    auto dependents = dep_graph_.get_all_dependents(view_name);

    // Drop in reverse topological order
    std::reverse(dependents.begin(), dependents.end());

    std::unique_lock<std::shared_mutex> view_lock(view_mutex_);

    for (const auto &dep : dependents) {
      // Delete from _dbsp_views table if context provided
      if (context) {
        try {
          std::string delete_sql = "DELETE FROM _dbsp_views WHERE name = '" + dep + "'";
          auto res = context->Query(delete_sql, false);
          // Ignore errors - persistence is best-effort
        } catch (...) {
          // Ignore persistence errors
        }
      }

      dep_graph_.remove_node(dep);
      view_definitions_.erase(dep);
      table_schemas_.erase(dep);
      views_.erase(dep);
    }

    // Delete from _dbsp_views table if context provided
    if (context) {
      try {
        std::string delete_sql = "DELETE FROM _dbsp_views WHERE name = '" + view_name + "'";
        auto res = context->Query(delete_sql, false);
        // Ignore errors - persistence is best-effort
      } catch (const std::exception &e) {
        // Ignore persistence errors - logged for debugging
      } catch (...) {
        // Ignore unknown persistence errors
      }
    }

    dep_graph_.remove_node(view_name);
    view_definitions_.erase(view_name);
    table_schemas_.erase(view_name);
    return views_.erase(view_name) > 0;
  }

  // Record an INSERT
  void on_insert(const std::string &table_name, const DuckDBRow &row) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);

    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end())
      return;

    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end())
      return;

    DuckDBZSet delta;
    {
      std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
      it->second->insert(row);
      delta = it->second->consume_changes();
    }

    if (!delta.empty()) {
      propagate_changes(table_name, delta);
    }
  }

  // Record a DELETE
  void on_delete(const std::string &table_name, const DuckDBRow &row) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);

    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end())
      return;

    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end())
      return;

    DuckDBZSet delta;
    {
      std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
      it->second->remove(row);
      delta = it->second->consume_changes();
    }

    if (!delta.empty()) {
      propagate_changes(table_name, delta);
    }
  }

  // Record an UPDATE
  void on_update(const std::string &table_name, const DuckDBRow &old_row,
                 const DuckDBRow &new_row) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);

    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end())
      return;

    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end())
      return;

    DuckDBZSet delta;
    {
      std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
      it->second->update(old_row, new_row);
      delta = it->second->consume_changes();
    }

    if (!delta.empty()) {
      propagate_changes(table_name, delta);
    }
  }

  // Batch insert
  void on_batch_insert(const std::string &table_name,
                       const std::vector<DuckDBRow> &rows) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);

    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end())
      return;

    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end())
      return;

    DuckDBZSet delta;
    {
      std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
      for (const auto &row : rows) {
        it->second->insert(row);
      }
      delta = it->second->consume_changes();
    }

    if (!delta.empty()) {
      propagate_changes(table_name, delta);
    }
  }

  // Sync tracked table with actual DuckDB table
  bool sync_table(duckdb::ClientContext &context,
                  const std::string &table_name) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);

    if (tracked_tables_.find(table_name) == tracked_tables_.end())
      return false;

    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end())
      return false;

    std::optional<DuckDBZSet> delta_opt;
    {
      std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
      delta_opt = sync_table_scan_and_consume(context, table_name);
    }

    if (!delta_opt.has_value())
      return false;

    if (!delta_opt->empty()) {
      propagate_changes(table_name, *delta_opt);
    }

    return true;
  }

  // Sync all tracked tables (sequential or parallel based on use_parallel_sync_)
  void sync_all(duckdb::ClientContext &context,
                duckdb::MetaTransaction *meta_transaction = nullptr) {
    // Snapshot table names and parallel flag under a shared lock, then release
    // before spawning threads. Holding struct_mutex_ while waiting on futures
    // that also need struct_mutex_ would block — snapshot it instead.
    bool do_parallel;
    std::vector<std::string> table_names;
    {
      std::shared_lock<std::shared_mutex> lock(struct_mutex_);
      do_parallel = use_parallel_sync_ && tracked_tables_.size() > 1;
      for (const auto &entry : tracked_tables_) {
        table_names.push_back(entry.first);
      }
    }
    sync_tables(context, table_names, do_parallel, meta_transaction);
  }

  // Sync only the named tables (H1 touched-table scoping: the transaction
  // hooks know which tables a transaction wrote, so a commit need not scan
  // every tracked table). Unknown names are skipped.
  void sync_tables(duckdb::ClientContext &context,
                   const std::vector<std::string> &table_names,
                   bool do_parallel,
                   duckdb::MetaTransaction *meta_transaction = nullptr) {

    if (do_parallel) {
      // TRUE parallelism: each thread acquires its own per-table lock.
      // DB scans for different tables run simultaneously.
      // Propagation serializes at view_mutex_ (fast; not the bottleneck).
      std::vector<std::future<void>> futures;
      for (const auto &table_name : table_names) {
        futures.push_back(std::async(std::launch::async,
            [this, &context, table_name, meta_transaction]() {
          std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);

          auto lock_it = table_locks_.find(table_name);
          if (lock_it == table_locks_.end())
            return;

          std::optional<DuckDBZSet> delta_opt;
          {
            std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
            delta_opt = sync_table_scan_and_consume(context, table_name,
                                                    meta_transaction);
          }

          if (!delta_opt.has_value())
            return;

          if (!delta_opt->empty()) {
            propagate_changes(table_name, *delta_opt);
          }
        }));
      }
      for (auto &f : futures) {
        f.wait();
      }
    } else {
      // Sequential: hold struct_mutex_ shared for the entire loop so the
      // table_locks_ map stays stable; acquire each table lock in turn.
      std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
      for (const auto &name : table_names) {
        auto lock_it = table_locks_.find(name);
        if (lock_it == table_locks_.end())
          continue;

        std::optional<DuckDBZSet> delta_opt;
        {
          std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
          delta_opt = sync_table_scan_and_consume(context, name,
                                                  meta_transaction);
        }

        if (!delta_opt.has_value())
          continue;

        if (!delta_opt->empty()) {
          propagate_changes(name, *delta_opt);
        }
      }
    }
  }

  // Enable/disable parallel sync
  void set_parallel_sync(bool enable) {
    std::unique_lock<std::shared_mutex> lock(struct_mutex_);
    use_parallel_sync_ = enable;
  }

  bool get_parallel_sync() const {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    return use_parallel_sync_;
  }

  // ========================================================================
  // Persistence
  // ========================================================================

  // Save all view definitions to a DuckDB table
  bool save_to_table(duckdb::ClientContext &context,
                     const std::string &storage_table = "_dbsp_views") {
    std::unique_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::shared_lock<std::shared_mutex> view_lock(view_mutex_);
    return save_to_table_internal(context, storage_table, view_definitions_);
  }

  bool save_view_to_table(duckdb::ClientContext &context,
                          const std::string &view_name,
                          const std::string &storage_table) {
    std::unique_lock<std::shared_mutex> struct_lock(struct_mutex_);
    // Save just the specified view
    std::unordered_map<std::string, ViewDefinition> single_view;
    auto it = view_definitions_.find(view_name);
    if (it != view_definitions_.end()) {
      single_view[view_name] = it->second;
    } else {
      last_error_ = "View not found: " + view_name;
      return false;
    }
    return save_to_table_internal(context, storage_table, single_view);
  }

private:
  bool save_to_table_internal(
      duckdb::ClientContext &context, const std::string &storage_table,
      const std::unordered_map<std::string, ViewDefinition> &defs) {
    try {
      auto &catalog = duckdb::Catalog::GetCatalog(context, INVALID_CATALOG);
      auto &schema_entry = catalog.GetSchema(context, DEFAULT_SCHEMA);
      auto catalog_txn = catalog.GetCatalogTransaction(context);

      // Table must exist - we can't create tables from within table functions
      auto existing = schema_entry.GetEntry(
          catalog_txn, duckdb::CatalogType::TABLE_ENTRY, storage_table);
      if (!existing) {
        last_error_ =
            "Storage table '" + storage_table +
            "' does not exist. Create it first with: CREATE TABLE " +
            storage_table +
            " (name VARCHAR, sql VARCHAR, sources VARCHAR, created_at BIGINT)";
        return false;
      }

      auto &table_entry = existing->Cast<duckdb::TableCatalogEntry>();

      // Use InternalAppender to insert data (no SQL queries)
      duckdb::InternalAppender appender(context, table_entry);

      for (const auto &[name, def] : defs) {
        std::string sources;
        for (size_t i = 0; i < def.source_tables.size(); i++) {
          if (i > 0)
            sources += ",";
          sources += def.source_tables[i];
        }

        appender.BeginRow();
        appender.Append(duckdb::string_t(name));
        appender.Append(duckdb::string_t(def.sql));
        appender.Append(duckdb::string_t(sources));
        appender.Append<int64_t>(def.created_at);
        appender.EndRow();
      }

      appender.Flush();
      appender.Close();
      return true;

    } catch (const std::exception &e) {
      last_error_ = std::string("Exception during save: ") + e.what();
      return false;
    }
  }

public:
  // Load view definitions from a DuckDB table and recreate views
  bool load_from_table(duckdb::ClientContext &context,
                       const std::string &storage_table = "_dbsp_views") {
    std::unique_lock<std::shared_mutex> lock(struct_mutex_);

    try {
      auto &catalog = duckdb::Catalog::GetCatalog(context, INVALID_CATALOG);
      auto &schema_entry = catalog.GetSchema(context, DEFAULT_SCHEMA);
      auto catalog_txn = catalog.GetCatalogTransaction(context);

      // Check if storage table exists
      auto table_ptr = schema_entry.GetEntry(
          catalog_txn, duckdb::CatalogType::TABLE_ENTRY, storage_table);
      if (!table_ptr) {
        // Table doesn't exist - nothing to load
        return true;
      }

      auto &table_entry = table_ptr->Cast<duckdb::TableCatalogEntry>();
      auto &data_table = table_entry.GetStorage();

      // Scan the storage table using Catalog API
      auto &transaction = duckdb::DuckTransaction::Get(context, catalog);
      duckdb::TableScanState scan_state;

      duckdb::vector<duckdb::StorageIndex> column_ids;
      auto &columns = table_entry.GetColumns();
      for (idx_t i = 0; i < columns.PhysicalColumnCount(); i++) {
        column_ids.push_back(duckdb::StorageIndex(i));
      }

      data_table.InitializeScan(context, transaction, scan_state, column_ids);

      duckdb::DataChunk chunk;
      chunk.Initialize(context, data_table.GetTypes());

      // Collect view definitions
      std::vector<ViewDefinition> defs;
      while (true) {
        chunk.Reset();
        data_table.Scan(transaction, chunk, scan_state);
        if (chunk.size() == 0)
          break;

        for (idx_t row_idx = 0; row_idx < chunk.size(); row_idx++) {
          ViewDefinition def;
          def.name = chunk.GetValue(0, row_idx).GetValue<std::string>();
          def.sql = chunk.GetValue(1, row_idx).GetValue<std::string>();

          std::string sources =
              chunk.GetValue(2, row_idx).GetValue<std::string>();
          if (!sources.empty()) {
            std::stringstream ss(sources);
            std::string source;
            while (std::getline(ss, source, ',')) {
              def.source_tables.push_back(source);
            }
          }

          def.created_at = chunk.GetValue(3, row_idx).GetValue<int64_t>();
          defs.push_back(def);
        }
      }

      // Sort by creation time to handle dependencies
      std::sort(defs.begin(), defs.end(),
                [](const ViewDefinition &a, const ViewDefinition &b) {
                  return a.created_at < b.created_at;
                });

      // Recreate views (unlock for create_view which takes struct_mutex_)
      lock.unlock();
      for (const auto &def : defs) {
        create_view(context, def.name, def.sql);
      }
      lock.lock();

      return true;

    } catch (const std::exception &e) {
      last_error_ = std::string("Exception during load: ") + e.what();
      return false;
    }
  }

  // Save to JSON file
  bool save_to_file(const std::string &filepath) {
    std::unique_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::shared_lock<std::shared_mutex> view_lock(view_mutex_);

    try {
      // Validate filepath to prevent path traversal
      std::string validated_path = validate_filepath(filepath);
      if (validated_path.empty()) {
        last_error_ =
            "Invalid file path (must be relative, no path traversal): " +
            filepath;
        return false;
      }

      std::ofstream file(validated_path);
      if (!file.is_open()) {
        last_error_ = "Could not open file: " + validated_path;
        return false;
      }

      file << "{\n";

      // Save tracked tables
      file << "  \"tracked_tables\": [";
      bool first = true;
      for (const auto &[name, _] : tracked_tables_) {
        if (!first)
          file << ", ";
        file << "\"" << name << "\"";
        first = false;
      }
      file << "],\n";

      // Save views
      file << "  \"views\": [\n";
      first = true;
      for (const auto &[name, def] : view_definitions_) {
        if (!first)
          file << ",\n";
        file << "    {\n";
        file << "      \"name\": \"" << def.name << "\",\n";
        file << "      \"sql\": \"" << escape_json(def.sql) << "\",\n";
        file << "      \"sources\": [";
        for (size_t i = 0; i < def.source_tables.size(); i++) {
          if (i > 0)
            file << ", ";
          file << "\"" << def.source_tables[i] << "\"";
        }
        file << "],\n";
        file << "      \"created_at\": " << def.created_at << "\n";
        file << "    }";
        first = false;
      }
      file << "\n  ]\n";

      file << "}\n";
      file.close();

      return true;

    } catch (const std::exception &e) {
      last_error_ = std::string("Exception during file save: ") + e.what();
      return false;
    } catch (...) {
      last_error_ = "Unknown exception during file save";
      return false;
    }
  }

  // Load from JSON file
  bool load_from_file(duckdb::ClientContext &context,
                      const std::string &filepath) {
    std::unique_lock<std::shared_mutex> lock(struct_mutex_);

    try {
      // Validate filepath to prevent path traversal
      std::string validated_path = validate_filepath(filepath);
      if (validated_path.empty()) {
        last_error_ =
            "Invalid file path (must be relative, no path traversal): " +
            filepath;
        return false;
      }

      std::ifstream file(validated_path);
      if (!file.is_open()) {
        last_error_ = "Could not open file: " + validated_path;
        return false;
      }

      std::stringstream buffer;
      buffer << file.rdbuf();
      std::string content = buffer.str();
      file.close();

      // Simple JSON parsing for our specific format
      // Parse tracked tables
      size_t tables_start = content.find("\"tracked_tables\"");
      if (tables_start != std::string::npos) {
        size_t arr_start = content.find('[', tables_start);
        size_t arr_end = content.find(']', arr_start);
        std::string tables_arr =
            content.substr(arr_start + 1, arr_end - arr_start - 1);

        size_t pos = 0;
        while ((pos = tables_arr.find('"', pos)) != std::string::npos) {
          size_t end = tables_arr.find('"', pos + 1);
          if (end != std::string::npos) {
            std::string table = tables_arr.substr(pos + 1, end - pos - 1);
            track_table_internal(context, table);
            pos = end + 1;
          } else {
            break;
          }
        }
      }

      // Parse views - extract each view object
      std::vector<ViewDefinition> defs;
      size_t views_start = content.find("\"views\"");
      if (views_start != std::string::npos) {
        size_t arr_start = content.find('[', views_start);
        size_t search_pos = arr_start;

        while (true) {
          size_t obj_start = content.find('{', search_pos);
          if (obj_start == std::string::npos)
            break;

          size_t obj_end = content.find('}', obj_start);
          if (obj_end == std::string::npos)
            break;

          std::string obj = content.substr(obj_start, obj_end - obj_start + 1);

          ViewDefinition def;

          // Extract name
          size_t name_pos = obj.find("\"name\"");
          if (name_pos != std::string::npos) {
            size_t val_start = obj.find('"', name_pos + 6);
            size_t val_end = obj.find('"', val_start + 1);
            def.name = obj.substr(val_start + 1, val_end - val_start - 1);
          }

          // Extract sql
          size_t sql_pos = obj.find("\"sql\"");
          if (sql_pos != std::string::npos) {
            size_t val_start = obj.find('"', sql_pos + 5);
            size_t val_end = find_string_end(obj, val_start + 1);
            def.sql = unescape_json(
                obj.substr(val_start + 1, val_end - val_start - 1));
          }

          // Extract created_at
          size_t time_pos = obj.find("\"created_at\"");
          if (time_pos != std::string::npos) {
            size_t val_start = obj.find(':', time_pos) + 1;
            while (obj[val_start] == ' ')
              val_start++;
            size_t val_end = val_start;
            while (val_end < obj.size() && (isdigit(obj[val_end])))
              val_end++;
            def.created_at =
                std::stoull(obj.substr(val_start, val_end - val_start));
          }

          if (!def.name.empty() && !def.sql.empty()) {
            defs.push_back(def);
          }

          search_pos = obj_end + 1;
        }
      }

      // Sort by creation time
      std::sort(defs.begin(), defs.end(),
                [](const ViewDefinition &a, const ViewDefinition &b) {
                  return a.created_at < b.created_at;
                });

      // Recreate views (unlock for create_view which takes struct_mutex_)
      lock.unlock();
      for (const auto &def : defs) {
        create_view(context, def.name, def.sql);
      }
      lock.lock();

      return true;

    } catch (const std::exception &e) {
      last_error_ = std::string("Exception during file load: ") + e.what();
      return false;
    } catch (...) {
      last_error_ = "Unknown exception during file load";
      return false;
    }
  }

  // ========================================================================
  // Query methods
  // ========================================================================

  const DuckDBZSet *query_view(const std::string &view_name) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::shared_lock<std::shared_mutex> view_lock(view_mutex_);
    auto it = views_.find(view_name);
    if (it == views_.end())
      return nullptr;
    return &it->second->get_result();
  }

  // NOTE: the returned pointer is only lock-protected during this lookup.
  // Callers that read view state (scan, get_result) while writers may be
  // active must use scan_view() instead — it holds the read locks for the
  // whole traversal. get_view() remains for single-threaded use (tests).
  const NativeMaterializedView *get_view(const std::string &view_name) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::shared_lock<std::shared_mutex> view_lock(view_mutex_);
    auto it = views_.find(view_name);
    if (it == views_.end()) {
      return nullptr;
    }
    return it->second.get();
  }

  // Scan a view's rows under shared locks: safe against concurrent
  // propagate_changes/create_view, which take view_mutex_ exclusively.
  // Returns false if the view does not exist.
  bool scan_view(const std::string &view_name,
                 const std::function<void(const DuckDBRow &, Weight)> &cb) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::shared_lock<std::shared_mutex> view_lock(view_mutex_);
    auto it = views_.find(view_name);
    if (it == views_.end()) {
      return false;
    }
    it->second->scan(cb);
    return true;
  }

  const TableSchema *get_view_schema(const std::string &view_name) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::shared_lock<std::shared_mutex> view_lock(view_mutex_);
    auto it = views_.find(view_name);
    if (it == views_.end())
      return nullptr;
    return &it->second->result_schema();
  }

  std::vector<std::string> list_views() {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    std::vector<std::string> names;
    for (const auto &[name, _] : views_) {
      names.push_back(name);
    }
    return names;
  }

  std::vector<std::string> list_tracked_tables() {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    std::vector<std::string> names;
    for (const auto &[name, _] : tracked_tables_) {
      names.push_back(name);
    }
    return names;
  }

  // Helper methods for testing and recovery
  bool is_table_tracked(const std::string &table_name) const {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    return tracked_tables_.find(table_name) != tracked_tables_.end();
  }

  bool is_view_registered(const std::string &view_name) const {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    return views_.find(view_name) != views_.end();
  }

  // Total weight (row multiplicity included) of the tracked baseline —
  // comparable against COUNT(*) of the real table
  int64_t tracked_total_weight(const std::string &table_name) const {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end()) {
      return -1;
    }
    int64_t total = 0;
    for (const auto &[row, w] : it->second->current_state()) {
      total += w;
    }
    return total;
  }

  // Apply a delta captured from a transaction's local storage (G2 fast
  // path): O(delta) — no table scan, no diff. The caller has already
  // validated the delta against the committed table (count guard).
  bool apply_captured_delta(const std::string &table_name,
                            const DuckDBZSet &delta) {
    if (delta.empty()) {
      return true;
    }
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end()) {
      return false;
    }
    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end()) {
      return false;
    }
    {
      std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
      it->second->apply_delta(delta);
    }
    propagate_changes(table_name, delta);
    captured_delta_syncs_++;
    return true;
  }

  // Number of commits served by captured deltas instead of scan-and-diff
  // (observable so tests can prove the fast path actually ran)
  uint64_t captured_delta_syncs() const { return captured_delta_syncs_; }

  // Live shared join arrangements (I1); lets tests prove sharing happened
  size_t shared_arrangement_count() const {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    size_t n = 0;
    for (const auto &[fp, w] : arrangements_) {
      if (!w.expired()) {
        n++;
      }
    }
    return n;
  }

  // Number of scan-and-diff table scans performed (tests use this to prove
  // sync scoping skips untouched tables)
  uint64_t scan_syncs() const { return scan_syncs_; }

  size_t get_tracked_table_count(const std::string &table_name) const {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    auto it = tracked_tables_.find(table_name);
    if (it != tracked_tables_.end()) {
      return it->second->current_state().size();
    }
    return 0;
  }

  void clear_all_state() {
    std::unique_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::unique_lock<std::shared_mutex> view_lock(view_mutex_);
    tracked_tables_.clear();
    table_locks_.clear();
    views_.clear();
    view_definitions_.clear();
    // Dependency graph will be rebuilt when views are recreated
  }

  const TableSchema *get_table_schema(const std::string &table_name) {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    auto it = table_schemas_.find(table_name);
    return it != table_schemas_.end() ? &it->second : nullptr;
  }

  struct ViewInfo {
    std::string name;
    std::string sql;
    size_t row_count;
    uint64_t version;
    std::vector<std::string> source_tables;
    std::vector<std::string> dependent_views;
  };

  ViewInfo get_view_info(const std::string &view_name) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::shared_lock<std::shared_mutex> view_lock(view_mutex_);
    ViewInfo info;
    auto it = views_.find(view_name);
    if (it != views_.end()) {
      info.name = view_name;
      info.sql = it->second->sql();
      info.row_count = it->second->get_result().size();
      info.version = it->second->version();
      info.source_tables = it->second->source_tables();
      info.dependent_views = dep_graph_.get_all_dependents(view_name);
    }
    return info;
  }

  // Get view dependencies
  std::vector<std::string> get_view_dependencies(const std::string &view_name) {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    return dep_graph_.get_dependencies(view_name);
  }

  // Get views that depend on this view/table
  std::vector<std::string> get_dependents(const std::string &name) {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    return dep_graph_.get_all_dependents(name);
  }

  // Alias for get_dependents (used by parser extension)
  std::vector<std::string> get_dependent_views(const std::string &name) {
    return get_dependents(name);
  }

  // Check if view exists
  bool view_exists(const std::string &view_name) {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    return views_.find(view_name) != views_.end();
  }

  // Get drop order for CASCADE (returns views in order to drop)
  std::vector<std::string> get_drop_order(const std::string &view_name) {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    auto dependents = dep_graph_.get_all_dependents(view_name);

    // Return in reverse order (drop leaves first, then parents)
    std::reverse(dependents.begin(), dependents.end());
    return dependents;
  }

  // Returns last error by value for thread safety (was const std::string&)
  std::string last_error() const {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    return last_error_;
  }

  // Initialize persistence table for storing view definitions.
  // Deliberately does NOT hold struct_mutex_ across the query: callers may
  // already hold it (recovery can be triggered mid-sync), and shared_mutex is
  // not recursive. The lock is only taken briefly to record errors.
  bool initialize_persistence_table(duckdb::ClientContext &context) {
    try {
      // Create _dbsp_views table if it doesn't exist. Fresh connection:
      // `context` may be mid-query (recovery runs inside table functions).
      InternalQueryGuard guard;
      duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
      auto result = con.Query(
        "CREATE TABLE IF NOT EXISTS _dbsp_views ("
        "  name VARCHAR PRIMARY KEY,"
        "  sql VARCHAR NOT NULL,"
        "  sources VARCHAR,"
        "  created_at BIGINT NOT NULL"
        ")"
      );

      if (result->HasError()) {
        record_error_best_effort("Failed to create _dbsp_views table: " +
                                 result->GetError());
        return false;
      }

      return true;
    } catch (const std::exception &e) {
      record_error_best_effort(
          std::string("Exception creating _dbsp_views table: ") + e.what());
      return false;
    }
  }

  // Record an error without risking same-thread relock: callers of some paths
  // already hold struct_mutex_ (not recursive), so skip recording rather than
  // deadlock if the lock is contended.
  void record_error_best_effort(const std::string &msg) {
    std::unique_lock<std::shared_mutex> lock(struct_mutex_, std::try_to_lock);
    if (lock.owns_lock()) {
      last_error_ = msg;
    } else {
      std::cerr << "DBSP error (lock busy, not recorded): " << msg << "\n";
    }
  }

  // Create view with explicit sources (for recovery)
  bool create_view(const std::string &view_name,
                   const std::string &sql,
                   const std::vector<std::string> &sources,
                   duckdb::ClientContext &context) {
    // For recovery, we call the normal create_view which will re-parse SQL
    // The sources parameter is used to validate consistency
    return create_view(context, view_name, sql);
  }

  // NOTE: there is deliberately no restore-view-from-checkpoint API.
  // set_result() fills only a view's sink; internal circuit-node state
  // (aggregate groups, join indexes, sort/limit multisets, recursive dedup)
  // cannot be reconstructed from the sink. Recovery rebuilds views by
  // replaying committed DuckDB storage through create_view instead.

private:
  // Scan tracked table from DB and compute/apply delta.
  // Must be called with the table's table_lock held exclusively.
  // struct_mutex_ must also be held (shared or exclusive) by the caller.
  //
  // Returns the consumed delta on success, std::nullopt on error.
  // On error, last_error_ is set.
  std::optional<DuckDBZSet> sync_table_scan_and_consume(
      duckdb::ClientContext &context,
      const std::string &table_name,
      duckdb::MetaTransaction *meta_transaction = nullptr) {

    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end()) {
      last_error_ = "Table not tracked: " + table_name;
      return std::nullopt;
    }

    try {
      // Get table catalog entry using Catalog API (avoid SQL queries)
      auto &db_instance = duckdb::DatabaseInstance::GetDatabase(context);
      auto &db_manager = db_instance.GetDatabaseManager();
      auto dbs = db_manager.GetDatabases();
      duckdb::AttachedDatabase *target_db = nullptr;
      for (auto &db_entry : dbs) {
        if (!db_entry->IsSystem() && !db_entry->IsTemporary()) {
          target_db = db_entry.get();
          break;
        }
      }
      if (!target_db) {
        target_db = &db_manager.GetSystemCatalog().GetAttached();
      }
      auto &catalog = target_db->GetCatalog();

      DuckDBZSet new_state;
      scan_syncs_++;

      {
        // Both paths (commit hook and explicit user call) scan committed state
        // through a fresh Connection. The commit hook fires after Commit() has
        // succeeded, so a new transaction sees the committed rows. Scanning
        // with the just-committed DuckTransaction via the raw storage API
        // stopped seeing its own rows in DuckDB 1.5 (hook now runs before
        // Finalize()), so that path was removed.
        // Guard: OnConnectionOpened must not run first-time recovery here -
        // this thread holds struct_mutex_ (via sync_table) and recovery
        // re-acquires it.
        InternalQueryGuard guard;
        auto &fresh_db = duckdb::DatabaseInstance::GetDatabase(context);
        duckdb::Connection fresh_con(fresh_db);
        // Streaming execution (H5): rows are consumed chunk by chunk below,
        // so materializing the whole table into a QueryResult first was one
        // extra full-table copy per sync
        auto sql_result =
            fresh_con.SendQuery("SELECT * FROM \"" + table_name + "\"");
        if (!sql_result || sql_result->HasError()) {
          last_error_ = "Failed to scan table '" + table_name + "': " +
                        (sql_result ? sql_result->GetError() : "null result");
          return std::nullopt;
        }
        while (true) {
          auto chunk_ptr = sql_result->Fetch();
          if (!chunk_ptr || chunk_ptr->size() == 0) break;
          auto &chunk = *chunk_ptr;
          // Typed column extraction: flatten once, read vector data
          // directly for common types instead of boxing every cell
          // through DataChunk::GetValue (same lesson as the D1 batch
          // evaluator — per-cell Value dispatch dominates)
          chunk.Flatten();
          const idx_t n = chunk.size();
          const idx_t ncols = chunk.ColumnCount();
          std::vector<std::vector<duckdb::Value>> vals(n);
          for (auto &r : vals) {
            r.reserve(ncols);
          }
          for (idx_t c = 0; c < ncols; c++) {
            auto &vec = chunk.data[c];
            const auto &type = vec.GetType();
            auto &validity = duckdb::FlatVector::Validity(vec);
            switch (type.id()) {
            case duckdb::LogicalTypeId::INTEGER: {
              auto data = duckdb::FlatVector::GetData<int32_t>(vec);
              for (idx_t i = 0; i < n; i++) {
                vals[i].push_back(
                    validity.RowIsValid(i) ? duckdb::Value::INTEGER(data[i])
                                           : duckdb::Value(type));
              }
              break;
            }
            case duckdb::LogicalTypeId::BIGINT: {
              auto data = duckdb::FlatVector::GetData<int64_t>(vec);
              for (idx_t i = 0; i < n; i++) {
                vals[i].push_back(
                    validity.RowIsValid(i) ? duckdb::Value::BIGINT(data[i])
                                           : duckdb::Value(type));
              }
              break;
            }
            case duckdb::LogicalTypeId::DOUBLE: {
              auto data = duckdb::FlatVector::GetData<double>(vec);
              for (idx_t i = 0; i < n; i++) {
                vals[i].push_back(
                    validity.RowIsValid(i) ? duckdb::Value::DOUBLE(data[i])
                                           : duckdb::Value(type));
              }
              break;
            }
            case duckdb::LogicalTypeId::VARCHAR: {
              auto data = duckdb::FlatVector::GetData<duckdb::string_t>(vec);
              for (idx_t i = 0; i < n; i++) {
                vals[i].push_back(
                    validity.RowIsValid(i)
                        ? duckdb::Value(data[i].GetString())
                        : duckdb::Value(type));
              }
              break;
            }
            default:
              for (idx_t i = 0; i < n; i++) {
                vals[i].push_back(chunk.GetValue(c, i));
              }
              break;
            }
          }
          for (idx_t i = 0; i < n; i++) {
            DuckDBRow row;
            row.columns.assign(std::move(vals[i]));
            new_state.insert(std::move(row), 1);
          }
        }
      }

      // Compute delta = new_state - old_state
      DuckDBZSet delta;
      const auto &old_state = it->second->current_state();

      for (const auto &[row, weight] : new_state) {
        int64_t old_weight = old_state.get(row);
        int64_t diff = weight - old_weight;
        if (diff != 0) {
          delta.insert(row, diff);
        }
      }

      for (const auto &[row, weight] : old_state) {
        if (new_state.get(row) == 0) {
          delta.insert(row, -weight);
        }
      }

      // The freshly scanned state IS the new baseline: swap it in whole
      // instead of replaying the delta row by row (which repeated every
      // hash operation the diff above already paid for)
      it->second->replace_state(std::move(new_state));
      return delta;

    } catch (const std::exception &e) {
      last_error_ = std::string("Exception in sync_table_scan_and_consume: ") + e.what();
      return std::nullopt;
    } catch (...) {
      last_error_ = "Unknown exception in sync_table_scan_and_consume";
      return std::nullopt;
    }
  }

  bool track_table_internal(duckdb::ClientContext &context,
                            const std::string &table_name) {
    // Called with struct_mutex_ exclusively held.
    if (tracked_tables_.count(table_name)) {
      return true;
    }

    TableSchema schema;
    if (!get_table_schema(context, table_name, schema)) {
      return false;
    }

    tracked_tables_[table_name] =
        std::make_unique<TrackedTable>(table_name, schema);
    table_schemas_[table_name] = schema;
    table_locks_[table_name] = std::make_unique<std::shared_mutex>();

    sync_table_internal(context, table_name);
    // CRITICAL: Clear pending changes from initial sync so they don't get
    // propagated as deltas to views that were already initialized with this
    // state in CDCManager::create_view
    tracked_tables_[table_name]->consume_changes();
    return true;
  }

  bool sync_table_internal(duckdb::ClientContext &context,
                           const std::string &table_name) {
    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end())
      return false;

    try {
      // Get table catalog entry using Catalog API (avoid SQL queries)
      auto &catalog = duckdb::Catalog::GetCatalog(context, INVALID_CATALOG);
      auto &schema_entry = catalog.GetSchema(context, DEFAULT_SCHEMA);
      auto catalog_transaction = catalog.GetCatalogTransaction(context);

      auto table_entry_ptr = schema_entry.GetEntry(
          catalog_transaction, duckdb::CatalogType::TABLE_ENTRY, table_name);
      if (!table_entry_ptr) {
        return false;
      }

      auto &table_entry = table_entry_ptr->Cast<duckdb::TableCatalogEntry>();
      auto &data_table = table_entry.GetStorage();

      // Get transaction and set up table scan
      auto &transaction = duckdb::DuckTransaction::Get(context, catalog);
      duckdb::TableScanState scan_state;

      // Get all column indices
      duckdb::vector<duckdb::StorageIndex> column_ids;
      auto &columns = table_entry.GetColumns();
      for (idx_t i = 0; i < columns.PhysicalColumnCount(); i++) {
        column_ids.push_back(duckdb::StorageIndex(i));
      }

      // Initialize scan
      data_table.InitializeScan(context, transaction, scan_state, column_ids);

      // Scan data chunks
      duckdb::DataChunk chunk;
      chunk.Initialize(context, data_table.GetTypes());

      while (true) {
        chunk.Reset();
        data_table.Scan(transaction, chunk, scan_state);

        if (chunk.size() == 0) {
          break; // No more data
        }

        // Extract rows from chunk
        for (idx_t row_idx = 0; row_idx < chunk.size(); row_idx++) {
          DuckDBRow dbsp_row;
          for (idx_t col_idx = 0; col_idx < chunk.ColumnCount(); col_idx++) {
            dbsp_row.columns.push_back(chunk.GetValue(col_idx, row_idx));
          }
          it->second->insert(dbsp_row);
        }
      }

      return true;

    } catch (const std::exception &e) {
      return false;
    } catch (...) {
      return false;
    }
  }

  bool get_table_schema(duckdb::ClientContext &context,
                        const std::string &table_name, TableSchema &schema) {
    try {
      // Use DuckDB's Catalog API instead of SQL queries to avoid deadlock
      auto &catalog = duckdb::Catalog::GetCatalog(context, INVALID_CATALOG);
      auto &schema_entry = catalog.GetSchema(context, DEFAULT_SCHEMA);
      auto catalog_transaction = catalog.GetCatalogTransaction(context);

      auto table_entry_ptr = schema_entry.GetEntry(
          catalog_transaction, duckdb::CatalogType::TABLE_ENTRY, table_name);
      if (!table_entry_ptr) {
        last_error_ = "Table not found: " + table_name;
        return false;
      }

      auto &table_entry = table_entry_ptr->Cast<duckdb::TableCatalogEntry>();

      schema.table_name = table_name;
      schema.columns.clear();

      for (auto &col : table_entry.GetColumns().Logical()) {
        ColumnInfo col_info;
        col_info.name = col.Name();
        col_info.type = col.Type();
        schema.columns.push_back(col_info);
      }

      if (schema.columns.empty()) {
        last_error_ = "Table has no columns: " + table_name;
        return false;
      }

      return true;

    } catch (const std::exception &e) {
      last_error_ = std::string("Exception getting table schema: ") + e.what();
      return false;
    } catch (...) {
      last_error_ = "Unknown exception getting table schema";
      return false;
    }
  }

  duckdb::LogicalType parse_type(const std::string &type_str) {
    std::string upper = duckdb::StringUtil::Upper(type_str);

    if (upper == "INTEGER" || upper == "INT" || upper == "INT4") {
      return duckdb::LogicalType::INTEGER;
    }
    if (upper == "BIGINT" || upper == "INT8") {
      return duckdb::LogicalType::BIGINT;
    }
    if (upper == "SMALLINT" || upper == "INT2") {
      return duckdb::LogicalType::SMALLINT;
    }
    if (upper == "TINYINT" || upper == "INT1") {
      return duckdb::LogicalType::TINYINT;
    }
    if (upper == "DOUBLE" || upper == "FLOAT8" || upper == "REAL") {
      return duckdb::LogicalType::DOUBLE;
    }
    if (upper == "FLOAT" || upper == "FLOAT4") {
      return duckdb::LogicalType::FLOAT;
    }
    if (upper == "BOOLEAN" || upper == "BOOL") {
      return duckdb::LogicalType::BOOLEAN;
    }
    if (upper.find("VARCHAR") != std::string::npos || upper == "TEXT" ||
        upper == "STRING") {
      return duckdb::LogicalType::VARCHAR;
    }
    if (upper == "DATE") {
      return duckdb::LogicalType::DATE;
    }
    if (upper == "TIMESTAMP" || upper == "DATETIME") {
      return duckdb::LogicalType::TIMESTAMP;
    }

    return duckdb::LogicalType::VARCHAR;
  }

  // Propagate changes through dependency graph.
  // IMPORTANT: Must be called with struct_mutex_ already held (shared or
  // exclusive) by the caller. Do NOT re-acquire struct_mutex_ here.
  // Acquires view_mutex_ exclusively for writing view Z-set state.
  // Incremental cascade (E1): one topological pass over every transitive
  // dependent. Each view applies the pending delta of each updated source;
  // its own resulting delta joins the pending map for its dependents.
  // O(Δ) end to end — no view is ever reset or recomputed from full state.
  void propagate_changes(const std::string &source_name,
                         const DuckDBZSet &delta) {
    if (delta.empty())
      return;

    // Acquire view_mutex_ exclusively for writing view content.
    // Lock ordering: struct_mutex_ (held by caller) → view_mutex_ (acquired here).
    std::unique_lock<std::shared_mutex> view_lock(view_mutex_);

    // Pending deltas by source name. Values either borrow a view's
    // get_delta() (valid until that view's next apply — topological order
    // guarantees the read happens first) or point into the arena below
    // when a view consumed deltas from multiple sources (each apply clears
    // the previous delta, so dependents need the accumulated union).
    std::unordered_map<std::string, const DuckDBZSet *> pending;
    std::deque<DuckDBZSet> arena;
    pending[source_name] = &delta;

    // Shared arrangements are updated BEFORE any consuming view steps —
    // join nodes rely on the arrangement being post-delta and drop their
    // Δl⋈Δr term to compensate (Δl⋈R_new = Δl⋈R_old + Δl⋈Δr)
    apply_to_arrangements(source_name, delta);

    // Group the topological order into levels: views in the same level
    // share no dependency path, so their circuits may step concurrently.
    // Everything they read while stepping is frozen for the level —
    // pending deltas from earlier levels, shared arrangements (updated
    // before views step / between levels), and their own private state.
    const std::vector<std::string> topo =
        dep_graph_.topological_order(source_name);
    std::unordered_map<std::string, size_t> level_of;
    level_of[source_name] = 0;
    std::vector<std::vector<std::string>> levels;
    for (const auto &view_name : topo) {
      auto it = views_.find(view_name);
      if (it == views_.end())
        continue;
      size_t lvl = 1;
      for (const auto &src : it->second->source_tables()) {
        auto l = level_of.find(src);
        if (l != level_of.end()) {
          lvl = std::max(lvl, l->second + 1);
        }
      }
      level_of[view_name] = lvl;
      if (levels.size() < lvl) {
        levels.resize(lvl);
      }
      levels[lvl - 1].push_back(view_name);
    }

    struct StepResult {
      std::string view_name;
      const DuckDBZSet *single_delta = nullptr; // borrows view state
      DuckDBZSet accumulated;                   // owned multi-source union
      size_t applied = 0;
    };
    auto step_view = [&](const std::string &view_name) -> StepResult {
      StepResult r;
      r.view_name = view_name;
      NativeMaterializedView &view = *views_.at(view_name);
      for (const auto &src : view.source_tables()) {
        auto p = pending.find(src);
        if (p == pending.end() || p->second->empty())
          continue;
        view.apply_changes(src, *p->second);
        r.applied++;
        if (r.applied == 1) {
          r.single_delta = &view.get_delta();
        } else {
          if (r.applied == 2 && r.single_delta) {
            r.accumulated = *r.single_delta; // upgrade borrow to owned
            r.single_delta = nullptr;
          }
          for (const auto &[row, w] : view.get_delta()) {
            r.accumulated.insert(row, w);
          }
        }
      }
      return r;
    };

    for (const auto &level : levels) {
      std::vector<StepResult> results;
      results.reserve(level.size());
      // Thread spawn costs ~10µs each — only worth it when the level has
      // real work (tiny deltas stay on the sequential path)
      size_t level_input_rows = 0;
      if (use_parallel_sync_ && level.size() > 1) {
        for (const auto &view_name : level) {
          for (const auto &src : views_.at(view_name)->source_tables()) {
            auto p = pending.find(src);
            if (p != pending.end()) {
              level_input_rows += p->second->size();
            }
          }
        }
      }
      if (use_parallel_sync_ && level.size() > 1 &&
          level_input_rows >= 256) {
        results.resize(level.size());
        std::vector<std::thread> threads;
        threads.reserve(level.size());
        std::exception_ptr first_error;
        std::mutex error_mutex;
        for (size_t i = 0; i < level.size(); i++) {
          threads.emplace_back([&, i]() {
            try {
              results[i] = step_view(level[i]);
            } catch (...) {
              std::lock_guard<std::mutex> g(error_mutex);
              if (!first_error) {
                first_error = std::current_exception();
              }
            }
          });
        }
        for (auto &t : threads) {
          t.join();
        }
        if (first_error) {
          std::rethrow_exception(first_error);
        }
      } else {
        for (const auto &view_name : level) {
          results.push_back(step_view(view_name));
        }
      }

      // Publish results sequentially (stable order): pending map for the
      // next level, arrangement updates for MV-sourced joins
      for (auto &r : results) {
        if (r.applied == 0)
          continue;
        if (r.single_delta) {
          if (!r.single_delta->empty()) {
            pending[r.view_name] = r.single_delta;
            apply_to_arrangements(r.view_name, *r.single_delta);
          }
        } else if (!r.accumulated.empty()) {
          arena.push_back(std::move(r.accumulated));
          pending[r.view_name] = &arena.back();
          apply_to_arrangements(r.view_name, arena.back());
        }
      }
    }
  }

  // Update every live shared arrangement over `name` with `delta`; prune
  // dead entries (their consuming views were dropped). Caller holds
  // view_mutex_ exclusively.
  void apply_to_arrangements(const std::string &name,
                             const DuckDBZSet &delta) {
    auto it = arrangements_by_table_.find(name);
    if (it == arrangements_by_table_.end()) {
      return;
    }
    auto &vec = it->second;
    for (auto w = vec.begin(); w != vec.end();) {
      if (auto arr = w->lock()) {
        arr->apply(delta);
        ++w;
      } else {
        w = vec.erase(w);
      }
    }
    if (vec.empty()) {
      arrangements_by_table_.erase(it);
    }
  }

  // String escaping utilities
  std::string escape_string(const std::string &s) {
    std::string result;
    for (char c : s) {
      if (c == '\'')
        result += "''";
      else
        result += c;
    }
    return result;
  }

  // Format runtime error with context
  std::string format_runtime_error(ErrorCode code, const std::string &view_name,
                                   const std::string &details,
                                   const std::string &source_name) {
    ErrorInfo info;
    info.code = code;
    info.message = details;
    info.context = "View: " + view_name + ", Source: " + source_name;
    info.workaround = get_workaround(code);
    info.doc_link = get_doc_link(code);

    return format_error(info);
  }

  std::string escape_json(const std::string &s) {
    std::string result;
    for (char c : s) {
      switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        result += c;
      }
    }
    return result;
  }

  std::string unescape_json(const std::string &s) {
    std::string result;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] == '\\' && i + 1 < s.size()) {
        switch (s[i + 1]) {
        case '"':
          result += '"';
          i++;
          break;
        case '\\':
          result += '\\';
          i++;
          break;
        case 'n':
          result += '\n';
          i++;
          break;
        case 'r':
          result += '\r';
          i++;
          break;
        case 't':
          result += '\t';
          i++;
          break;
        default:
          result += s[i];
        }
      } else {
        result += s[i];
      }
    }
    return result;
  }

  size_t find_string_end(const std::string &s, size_t start) {
    for (size_t i = start; i < s.size(); i++) {
      if (s[i] == '"' && (i == 0 || s[i - 1] != '\\')) {
        return i;
      }
    }
    return s.size();
  }

  // =========================================================================
  // Private data members
  //
  // Three-tier locking (always acquire in this order):
  //   1. struct_mutex_  — guards structural maps (tracked_tables_ keys,
  //      views_ keys, dep_graph_, table_schemas_, view_definitions_,
  //      table_locks_ map itself, auto_sync_enabled_, use_parallel_sync_)
  //   2. table_locks_[name]  — per-table lock, guards TrackedTable state
  //   3. view_mutex_  — guards view Z-set content (NativeMaterializedView data)
  // =========================================================================

  // Tier 1: structural lock
  mutable std::shared_mutex struct_mutex_;

  // Tier 2: per-table locks (keyed by table name)
  mutable std::unordered_map<std::string, std::unique_ptr<std::shared_mutex>> table_locks_;

  // Tier 3: view content lock
  mutable std::shared_mutex view_mutex_;

  std::unordered_map<std::string, std::unique_ptr<TrackedTable>>
      tracked_tables_;
  // I1: fingerprint → arrangement (weak; consuming join nodes own it),
  // plus per-table dispatch list for delta updates. Map shape mutated
  // under view_mutex_ exclusive (create_view / propagate_changes).
  std::unordered_map<std::string, std::weak_ptr<SharedArrangement>>
      arrangements_;
  std::unordered_map<std::string,
                     std::vector<std::weak_ptr<SharedArrangement>>>
      arrangements_by_table_;
  std::unordered_map<std::string, TableSchema> table_schemas_;
  std::unordered_map<std::string, std::unique_ptr<NativeMaterializedView>>
      views_;
  std::unordered_map<std::string, ViewDefinition> view_definitions_;
  DependencyGraph dep_graph_;
  std::string last_error_;
  std::atomic<bool> auto_sync_enabled_{false};
  std::atomic<uint64_t> captured_delta_syncs_{0};
  std::atomic<uint64_t> scan_syncs_{0};
  bool use_parallel_sync_ = false;
};

// Global CDC manager instance
// Deliberately leaked heap singleton (never destroyed). Planner-frontend
// views hold an internal Connection that keeps the DatabaseInstance alive
// (they must not outlive it: expression-executor buffers come from the
// instance's allocator). A function-local static CDCManager would destroy
// those views during static teardown at process exit, running DuckDB
// shutdown at static-destruction time — intermittent exit segfaults. With
// the leak, no destructors run at exit and the OS reclaims everything.
// Views are still freed normally via drop_view/reset while running.
inline CDCManager &get_cdc_manager() {
  static CDCManager *manager = new CDCManager();
  return *manager;
}

} // namespace dbsp_native

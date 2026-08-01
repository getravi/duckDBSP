// DBSP Change Data Capture (CDC) Manager
// Automatically tracks changes to DuckDB tables and propagates to views
// Supports cascading views (views on views) and persistence

#pragma once

#include "dbsp_duckdb_types.hpp"
#include "dbsp_qualified_name.hpp"
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
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include <algorithm>
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
#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace dbsp_native {

// D-lazy: number of views realize_pending_view[_locked] has actually
// decoded (a stash found and processed), across every CDCManager in the
// process. Test-observable counter (g_* convention, see
// g_recompute_invocations in dbsp_plan_translator.hpp) for asserting "only
// this view's chain decoded" without parsing DBSP_TIMING stderr output.
inline std::atomic<size_t> g_lazy_view_decodes{0};

// DBSP_TIMING=1: emit per-phase wall-clock lines to stderr
// ("[dbsp-timing] <phase> <detail> ms=..."), for profiling restore cost
// (source sync / arrangement backfill / blob decode) and commit propagation
// path (apply_captured_delta / arrangements / view_step). Off by default.
inline bool dbsp_timing_enabled() {
  static const bool on = std::getenv("DBSP_TIMING") != nullptr;
  return on;
}

struct DbspScopeTimer {
  const char *phase;
  std::string detail;
  std::chrono::steady_clock::time_point t0;
  DbspScopeTimer(const char *phase_p, std::string detail_p)
      : phase(phase_p), detail(std::move(detail_p)),
        t0(std::chrono::steady_clock::now()) {}
  ~DbspScopeTimer() {
    if (!dbsp_timing_enabled()) {
      return;
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    fprintf(stderr, "[dbsp-timing] %s %s ms=%.1f\n", phase, detail.c_str(), ms);
  }
};

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

// Qualify a table name with an optional catalog prefix for cross-catalog SQL.
// qualify("m", "_dbsp_views") -> "\"m\".\"_dbsp_views\""
// qualify("",  "_dbsp_views") -> "\"_dbsp_views\""
inline std::string qualify(const std::string &cat, const std::string &tbl) {
  return cat.empty() ? ("\"" + tbl + "\"") : ("\"" + cat + "\".\"" + tbl + "\"");
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
    auto_sync_enabled_ = true; // matches a fresh manager
    autopersist_ = true;
    autoload_attempted_ = false;
    autopersist_interval_ = 0;
    commits_since_checkpoint_ = 0;
    checkpoint_due_ = false;
    deferred_tables_ = 0;
    rebuild_pending_ = false;
    {
      std::lock_guard<std::mutex> g(seeds_mutex_);
      deferred_seeds_.clear();
    }
    last_deferred_count_ = 0;
    lazy_restore_ = true;
    pending_restore_.clear();
    last_pending_restore_count_ = 0;
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

  // Auto-persist control (Feature 1: views survive a clean connection
  // reopen with no explicit dbsp_save()/dbsp_load()). Default ON, same
  // atomic-flag shape as auto-sync above.
  void enable_autopersist() { autopersist_ = true; }

  void disable_autopersist() { autopersist_ = false; }

  bool autopersist_enabled() const { return autopersist_; }

  void set_autopersist_interval(size_t n) { autopersist_interval_ = n; }

  size_t autopersist_interval() const { return autopersist_interval_; }

  // Lazy per-view checkpoint restore (D-lazy). Default ON: the D3b
  // checkpoint fast path in load_from_duck_table stashes a cold-created
  // view's node/sink blobs undecoded instead of injecting them
  // immediately, so reopen returns without paying every view's blob_decode
  // cost -- each view decodes on first need (realize_pending_view). OFF
  // reproduces the pre-D-lazy eager behavior (every checkpointed view
  // fully restored during load_from_duck_table itself), for bulk
  // benchmarks or debugging a restore issue without the pending indirection
  // in the way. Same atomic-flag shape as autopersist_ above.
  void enable_lazy_restore() { lazy_restore_ = true; }

  void disable_lazy_restore() { lazy_restore_ = false; }

  bool lazy_restore_enabled() const { return lazy_restore_; }

  bool has_views() const {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    return !views_.empty();
  }

  // Auto-load trigger: called at the top of every user-facing entry point
  // that has a ClientContext and touches DBSP state. Fires at most once per
  // manager lifetime, and only when nothing has been created in this
  // process yet — a registry the user has already started populating
  // (directly, or via a prior auto-load) is never loaded over. A missing
  // _dbsp_views table is not an error (load_from_duck_table already treats
  // it as "nothing to load"); any other load failure is best-effort, never
  // fatal to the triggering call, but not silent either: it's logged under
  // DBSP_DEBUG_SYNC (same debug var this file already uses for other
  // best-effort CDC diagnostics) and last_error_ is left as
  // load_from_duck_table set it, so a later last_error()-reading caller
  // still sees it. Must NOT hold struct_mutex_ while calling
  // load_from_duck_table (see its own comment): the has-views check below
  // takes and releases its own lock first.
  //
  // autoload_attempted_ is a compare-and-swap, not a plain read-then-write:
  // two connections to the same DatabaseInstance can race their first DBSP
  // call, and a plain bool read/written from multiple threads without
  // synchronization is a data race (UB) regardless of how "benign" the
  // outcome looks. The CAS makes exactly one caller the winner that may
  // proceed to check has_views()/load; every other racer (including a
  // concurrent create_view()) just returns.
  void maybe_autoload(duckdb::ClientContext &context) {
    if (!autopersist_) {
      return;
    }
    bool expected = false;
    if (!autoload_attempted_.compare_exchange_strong(expected, true)) {
      return; // another call already claimed the auto-load attempt
    }
    if (has_views()) {
      return; // user already populated the registry themselves
    }
    if (!load_from_duck_table(context)) {
      if (std::getenv("DBSP_DEBUG_SYNC")) {
        std::cerr << "[dbsp] auto-load failed: " << last_error_ << "\n";
      }
    }
  }

  // Auto-save: fires a piggybacked circuit-state checkpoint once
  // autopersist_interval_ commits have accumulated since the last one
  // (counted by propagate_changes). Callers must invoke this from a point
  // where struct_mutex_ is NOT held by the current thread — save_checkpoint
  // takes its own shared lock on it, and struct_mutex_ (a std::shared_mutex)
  // is not safe to re-enter even for a second shared lock on the same
  // thread if a writer is queued.
  void maybe_save_checkpoint(duckdb::ClientContext &context) {
    if (checkpoint_due_.exchange(false)) {
      save_checkpoint(context);
    }
  }

  // --- Circuit-state checkpointing (D3b) --------------------------------
  // Persist per-view operator state (aggregate groups, private join
  // indexes) and sink results into _dbsp_ckpt, plus per-source watermarks
  // (COUNT + bit_xor(hash(row))) into _dbsp_ckpt_meta. Baselines and
  // shared arrangements are NOT persisted: with the watermark verified at
  // load, the baseline is by definition the committed table content, so
  // the load fast path defers it entirely (D3c) — TrackedTables start
  // deferred and materialize from a typed storage scan on first need.
  // Views with unsupported node kinds are simply not checkpointed and
  // rebuild normally.
  bool save_checkpoint(duckdb::ClientContext &context,
                       const std::string &catalog = "") {
    if (!catalog.empty() && !is_valid_identifier(catalog)) {
      last_error_ = "Invalid catalog name: " + catalog;
      return false;
    }
    struct ViewCkpt {
      std::string name;
      std::string sql; // fingerprint (Finding 1): definition at save time
      std::vector<std::pair<uint64_t, std::vector<uint8_t>>> nodes;
      std::vector<uint8_t> sink;
    };
    std::vector<ViewCkpt> view_blobs;
    std::vector<std::string> table_names;
    {
      std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
      std::shared_lock<std::shared_mutex> view_lock(view_mutex_);
      for (const auto &[name, view] : views_) {
        if (!view->checkpointable()) {
          continue;
        }
        ViewCkpt ck;
        ck.name = name;
        auto def_it = view_definitions_.find(name);
        if (def_it != view_definitions_.end()) {
          ck.sql = def_it->second.sql;
        }
        // D-lazy: a still-pending view has decoded nothing since its
        // checkpoint stash was read (realize_pending_view[_locked] is the
        // only thing that clears pending_restore_, and it always runs
        // before the first apply_changes_batch -- see propagate_changes's
        // pre-pass), so its stash is definitionally still-current state.
        // Re-save it VERBATIM: no decode, no re-encode, byte-identical to
        // what checkpoint_valid read, and immune to the empty
        // serialize_circuit_state()/get_result() a cold-created-but-never-
        // realized view would otherwise silently (and wrongly) produce
        // below.
        auto pend_it = pending_restore_.find(name);
        if (pend_it != pending_restore_.end()) {
          if (std::getenv("DBSP_DEBUG_SYNC") && !view->get_result().empty()) {
            // Invariant check: pending == no deltas seen == empty live
            // result (still the cold-create default). A non-empty result
            // here means some path applied a delta without realizing
            // first -- log loudly rather than silently trusting the stash.
            std::cerr << "[dbsp] invariant violation: pending view '"
                      << name
                      << "' has a non-empty live result at save time\n";
          }
          ck.nodes.reserve(pend_it->second.nodes.size());
          for (const auto &[node_id, blob] : pend_it->second.nodes) {
            ck.nodes.emplace_back(node_id, blob);
          }
          ck.sink = pend_it->second.sink;
          view_blobs.push_back(std::move(ck));
          continue;
        }
        if (!view->serialize_circuit_state(ck.nodes)) {
          continue;
        }
        BlobWriter w;
        const auto &result = view->get_result();
        w.u64(result.size());
        for (const auto &[row, weight] : result) {
          w.row(row.columns);
          w.i64(weight);
        }
        ck.sink = w.take();
        view_blobs.push_back(std::move(ck));
      }
      for (const auto &[name, _] : tracked_tables_) {
        table_names.push_back(name);
      }
    }

    const std::string ckpt_tbl = qualify(catalog, "_dbsp_ckpt");
    const std::string ckpt_meta_tbl = qualify(catalog, "_dbsp_ckpt_meta");
    const std::string ckpt_ver_tbl = qualify(catalog, "_dbsp_ckpt_version");

    try {
      InternalQueryGuard guard;
      duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
      con.Query("BEGIN");
      con.Query("CREATE OR REPLACE TABLE " + ckpt_tbl + " (kind VARCHAR, name "
                "VARCHAR, node_id BIGINT, data BLOB)");
      con.Query("CREATE OR REPLACE TABLE " + ckpt_meta_tbl + " (table_name "
                "VARCHAR, row_count BIGINT, row_hash VARCHAR)");
      // Format version (see dbsp_checkpoint.hpp kDbspCkptFormatVersion): a
      // pre-bump checkpoint lacks this table entirely, so checkpoint_valid
      // treats a missing/mismatched version as no checkpoint at all rather
      // than misparsing node blobs written under an older layout.
      con.Query("CREATE OR REPLACE TABLE " + ckpt_ver_tbl + " (version BIGINT)");
      auto ins = con.Prepare("INSERT INTO " + ckpt_tbl + " VALUES ($1, $2, $3, $4)");
      auto insm = con.Prepare("INSERT INTO " + ckpt_meta_tbl + " VALUES ($1, $2, $3)");
      auto insv = con.Prepare("INSERT INTO " + ckpt_ver_tbl + " VALUES ($1)");
      if (!ins || ins->HasError() || !insm || insm->HasError() || !insv ||
          insv->HasError()) {
        con.Query("ROLLBACK");
        last_error_ = "checkpoint prepare failed";
        return false;
      }
      {
        auto rv = insv->Execute(
            static_cast<int64_t>(dbsp_native::kDbspCkptFormatVersion));
        if (rv->HasError()) {
          con.Query("ROLLBACK");
          last_error_ = rv->GetError();
          return false;
        }
      }
      for (const auto &ck : view_blobs) {
        for (const auto &[node_id, blob] : ck.nodes) {
          auto r = ins->Execute(
              "node", ck.name, static_cast<int64_t>(node_id),
              duckdb::Value::BLOB(blob.data(), blob.size()));
          if (r->HasError()) {
            con.Query("ROLLBACK");
            last_error_ = r->GetError();
            return false;
          }
        }
        auto r = ins->Execute("sink", ck.name, static_cast<int64_t>(0),
                              duckdb::Value::BLOB(ck.sink.data(),
                                                  ck.sink.size()));
        if (r->HasError()) {
          con.Query("ROLLBACK");
          last_error_ = r->GetError();
          return false;
        }
        // SQL fingerprint (Finding 1): the view's exact definition at save
        // time, so a load whose _dbsp_views SQL has since diverged (e.g. a
        // replace_view whose fresh checkpoint never landed before an
        // unclean close) can detect the mismatch and decline the fast
        // path for this view rather than misapply old operator state to
        // a new query.
        auto rsql = ins->Execute("sql", ck.name, static_cast<int64_t>(0),
                                 duckdb::Value::BLOB(ck.sql));
        if (rsql->HasError()) {
          con.Query("ROLLBACK");
          last_error_ = rsql->GetError();
          return false;
        }
      }
      for (const auto &t : table_names) {
        auto wm = con.Query("SELECT COUNT(*), CAST(bit_xor(hash(t)) AS "
                            "VARCHAR) FROM " +
                            quote_table_key(t) + " t");
        if (wm->HasError() || wm->RowCount() != 1) {
          con.Query("ROLLBACK");
          last_error_ = "checkpoint watermark failed for " + t;
          return false;
        }
        auto r = insm->Execute(t, wm->GetValue(0, 0),
                               wm->GetValue(1, 0).ToString());
        if (r->HasError()) {
          con.Query("ROLLBACK");
          last_error_ = r->GetError();
          return false;
        }
      }
      con.Query("COMMIT");
      last_ckpt_saved_count_ = view_blobs.size();
      return true;
    } catch (const std::exception &e) {
      last_error_ = std::string("checkpoint save failed: ") + e.what();
      return false;
    }
  }

  // Load-side: true when a checkpoint exists and every watermark matches
  // the live tables. Fills `views` with per-view node blobs and sink blob.
  struct CkptData {
    std::unordered_map<std::string,
                       std::unordered_map<uint64_t, std::vector<uint8_t>>>
        nodes;
    std::unordered_map<std::string, std::vector<uint8_t>> sinks;
    // Per-view SQL fingerprint at save time (Finding 1) — compared against
    // the SQL about to be used to cold-create the view; a mismatch means
    // the view's definition changed since this checkpoint was written, so
    // the fast path is declined for that view alone.
    std::unordered_map<std::string, std::string> sql_fingerprints;
    // Verified per-source watermarks (COUNT, bit_xor(hash) as VARCHAR) —
    // seeds for lazy (deferred) baselines on the load fast path (D3c).
    std::unordered_map<std::string, std::pair<int64_t, std::string>>
        watermarks;
  };

  bool checkpoint_valid(duckdb::ClientContext &context, CkptData &out,
                        const std::string &catalog = "") {
    const std::string ckpt_tbl = qualify(catalog, "_dbsp_ckpt");
    const std::string ckpt_meta_tbl = qualify(catalog, "_dbsp_ckpt_meta");
    const std::string ckpt_ver_tbl = qualify(catalog, "_dbsp_ckpt_version");
    try {
      InternalQueryGuard guard;
      duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
      // Scope the existence check to the right catalog when one is given.
      std::string exists_sql;
      if (catalog.empty()) {
        exists_sql = "SELECT 1 FROM information_schema.tables WHERE table_name = '_dbsp_ckpt'";
      } else {
        exists_sql = "SELECT 1 FROM information_schema.tables WHERE table_catalog = '" +
                     catalog + "' AND table_name = '_dbsp_ckpt'";
      }
      auto exists = con.Query(exists_sql);
      if (exists->HasError() || exists->RowCount() == 0) {
        return false;
      }
      // Format-version gate: a checkpoint saved before the version table
      // existed (or under a different blob layout) must be treated as
      // absent, never fed to node restore_state — see
      // dbsp_checkpoint.hpp kDbspCkptFormatVersion.
      std::string ver_exists_sql;
      if (catalog.empty()) {
        ver_exists_sql = "SELECT 1 FROM information_schema.tables WHERE "
                         "table_name = '_dbsp_ckpt_version'";
      } else {
        ver_exists_sql = "SELECT 1 FROM information_schema.tables WHERE "
                         "table_catalog = '" +
                         catalog + "' AND table_name = '_dbsp_ckpt_version'";
      }
      auto ver_exists = con.Query(ver_exists_sql);
      if (ver_exists->HasError() || ver_exists->RowCount() == 0) {
        return false; // pre-bump checkpoint: no version table at all
      }
      auto ver = con.Query("SELECT version FROM " + ckpt_ver_tbl);
      if (ver->HasError() || ver->RowCount() != 1 ||
          ver->GetValue(0, 0).GetValue<int64_t>() !=
              dbsp_native::kDbspCkptFormatVersion) {
        return false; // version mismatch: discard, rebuild by replay
      }
      auto meta = con.Query(
          "SELECT table_name, row_count, row_hash FROM " + ckpt_meta_tbl);
      if (meta->HasError()) {
        return false;
      }
      for (duckdb::idx_t i = 0; i < meta->RowCount(); i++) {
        const std::string t = meta->GetValue(0, i).ToString();
        auto live = con.Query("SELECT COUNT(*), CAST(bit_xor(hash(t)) AS "
                              "VARCHAR) FROM " +
                              quote_table_key(t) + " t");
        if (live->HasError() || live->RowCount() != 1 ||
            live->GetValue(0, 0) != meta->GetValue(1, i) ||
            live->GetValue(1, 0).ToString() !=
                meta->GetValue(2, i).ToString()) {
          return false; // table gone or changed since save
        }
        out.watermarks[t] = {meta->GetValue(1, i).GetValue<int64_t>(),
                             meta->GetValue(2, i).ToString()};
      }
      auto rows = con.Query("SELECT kind, name, node_id, data FROM " + ckpt_tbl);
      if (rows->HasError()) {
        return false;
      }
      for (duckdb::idx_t i = 0; i < rows->RowCount(); i++) {
        const std::string kind = rows->GetValue(0, i).ToString();
        const std::string name = rows->GetValue(1, i).ToString();
        const auto node_id =
            static_cast<uint64_t>(rows->GetValue(2, i).GetValue<int64_t>());
        const auto blob_str =
            duckdb::StringValue::Get(rows->GetValue(3, i));
        std::vector<uint8_t> blob(blob_str.begin(), blob_str.end());
        if (kind == "node") {
          out.nodes[name][node_id] = std::move(blob);
        } else if (kind == "sink") {
          out.sinks[name] = std::move(blob);
        } else if (kind == "sql") {
          out.sql_fingerprints[name] =
              std::string(blob.begin(), blob.end());
        }
      }
      return !out.sinks.empty();
    } catch (...) {
      return false;
    }
  }

  // Inject checkpointed state into a cold-created view. Must be called
  // right after create_view(..., skip_init_replay=true).
  bool restore_view_state(const std::string &view_name,
                          const CkptData &ckpt) {
    DbspScopeTimer timer("blob_decode", view_name);
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::unique_lock<std::shared_mutex> view_lock(view_mutex_);
    auto it = views_.find(view_name);
    auto nodes_it = ckpt.nodes.find(view_name);
    auto sink_it = ckpt.sinks.find(view_name);
    if (it == views_.end() || sink_it == ckpt.sinks.end()) {
      return false;
    }
    static const std::unordered_map<uint64_t, std::vector<uint8_t>> kEmpty;
    const auto &blobs =
        nodes_it != ckpt.nodes.end() ? nodes_it->second : kEmpty;
    if (!it->second->restore_circuit_state(blobs)) {
      return false;
    }
    try {
      BlobReader r(sink_it->second.data(), sink_it->second.size());
      DuckDBZSet result;
      const uint64_t n = r.u64();
      for (uint64_t i = 0; i < n; i++) {
        DuckDBRow row = r.hashed_row();
        const int64_t w = r.i64();
        result.insert(row, w);
      }
      it->second->set_result(result);
      return r.done();
    } catch (...) {
      return false;
    }
  }

  // --- Lazy per-view checkpoint restore (D-lazy) --------------------------
  // Precedent: D3c's TrackedTable::is_deferred() + materialize_deferred_
  // locked (baseline materialization). This is the same lazy-materialize-
  // on-first-need shape applied to a VIEW's checkpoint blobs instead of a
  // table's storage scan: load_from_duck_table's checkpoint fast path
  // stashes the already-read (but undecoded) node/sink blobs here instead
  // of calling restore_view_state eagerly, and marks the view pending
  // (NativeMaterializedView::mark_pending_restore()). Every surface that
  // needs a pending view's live state (get_result()/get_delta()) must
  // realize it first -- see the call-site audit in the Task 1 report.
  //
  // Guarded by view_mutex_ (tier 3) -- the same lock that already guards
  // NativeMaterializedView content -- not a new lock level: presence in
  // this map is a property of a view's CONTENT (decoded or not), exactly
  // like TrackedTable::deferred_ is a property of a table's baseline
  // content and lives on the table's own per-table lock, not struct_mutex_.
  struct PendingViewCkpt {
    std::unordered_map<uint64_t, std::vector<uint8_t>> nodes;
    std::vector<uint8_t> sink;
  };

  // Stash a checkpointed view's raw node + sink blobs without decoding.
  // Caller: load_from_duck_table's checkpoint fast path, immediately after
  // create_view(..., skip_init_replay=true) returns true for a
  // fingerprint-matched cold create. No-op if the view is missing (should
  // be unreachable: called right after its own successful create_view).
  // Takes struct_lock shared + view_lock exclusive, the same locks
  // restore_view_state takes for the eager equivalent -- caller holds
  // neither (create_view released both on return, matching
  // load_from_duck_table's own "must NOT hold struct_mutex_" discipline).
  void stash_pending_view(const std::string &view_name, const CkptData &ckpt) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::unique_lock<std::shared_mutex> view_lock(view_mutex_);
    auto it = views_.find(view_name);
    if (it == views_.end()) {
      return;
    }
    PendingViewCkpt pv;
    auto nodes_it = ckpt.nodes.find(view_name);
    if (nodes_it != ckpt.nodes.end()) {
      pv.nodes = nodes_it->second;
    }
    auto sink_it = ckpt.sinks.find(view_name);
    if (sink_it != ckpt.sinks.end()) {
      pv.sink = sink_it->second;
    }
    pending_restore_[view_name] = std::move(pv);
    it->second->mark_pending_restore();
  }

  // Realize (decode + inject) `view_name`'s stashed checkpoint blobs.
  // No-op (returns true) when the view isn't pending -- safe to call
  // unconditionally at every realization call site, mirroring
  // materialize_deferred_locked's own no-op-when-not-deferred shape.
  //
  // Locking: caller already holds view_mutex_ exclusively (create_view,
  // register_arrangements, and propagate_changes's pre-pass all do, per
  // their own doc comments) -- this function does NOT lock it itself
  // (std::shared_mutex is not reentrant; a second exclusive lock on the
  // same thread is undefined behavior). Use realize_pending_view() (below)
  // from a context that holds neither struct_mutex_ nor view_mutex_.
  //
  // On a corrupt/mismatched stash (should not happen given the same bytes
  // just round-tripped through checkpoint_valid's read): unlike the eager
  // restore_view_state failure path in load_from_duck_table -- which can
  // drop_view()/create_view() a per-view replay rebuild right there -- this
  // function cannot: its callers already hold view_mutex_ exclusively, and
  // both drop_view and create_view re-acquire it (deadlock). It instead
  // escalates to the existing D3c global rebuild_pending_ escape hatch
  // (checked at every QueryBegin, see dbsp_context_state.hpp), which
  // drop_view()s and create_view()s every view fresh from committed
  // storage -- same "rare correctness escape hatch" the codebase already
  // accepts for a deferred-baseline watermark mismatch.
  bool realize_pending_view_locked(const std::string &view_name) {
    auto pend_it = pending_restore_.find(view_name);
    if (pend_it == pending_restore_.end()) {
      return true; // not pending: already live, or was never checkpointed
    }
    auto view_it = views_.find(view_name);
    if (view_it == views_.end()) {
      pending_restore_.erase(pend_it);
      return true;
    }
    DbspScopeTimer timer("blob_decode", view_name);
    g_lazy_view_decodes++;
    bool ok;
    try {
      ok = view_it->second->restore_circuit_state(pend_it->second.nodes);
      if (ok) {
        BlobReader r(pend_it->second.sink.data(), pend_it->second.sink.size());
        DuckDBZSet result;
        const uint64_t n = r.u64();
        for (uint64_t i = 0; i < n; i++) {
          DuckDBRow row = r.hashed_row();
          const int64_t w = r.i64();
          result.insert(row, w);
        }
        view_it->second->set_result(result);
        ok = ok && r.done();
      }
    } catch (...) {
      ok = false;
    }
    pending_restore_.erase(pend_it);
    view_it->second->clear_pending_restore();
    if (!ok) {
      rebuild_pending_ = true;
      record_error_best_effort(
          "DBSP: lazy-restore stash for view '" + view_name +
          "' failed to decode; scheduling full rebuild");
    }
    return ok;
  }

  // Self-locking entry point for callers that hold neither struct_mutex_
  // nor view_mutex_ (dbsp_query's scan_view, query_view). No-op when the
  // view is not pending. Acquires view_mutex_ shared for a cheap peek
  // first (avoiding an exclusive lock on the hot already-realized path)
  // and only re-acquires it exclusively -- released and re-taken, never
  // upgraded (std::shared_mutex has no upgrade primitive) -- when the peek
  // finds the view pending; realize_pending_view_locked re-checks under
  // that exclusive lock, so a racing second caller's peek-then-lock is
  // harmless (idempotent).
  void realize_pending_view(const std::string &view_name) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    {
      std::shared_lock<std::shared_mutex> peek(view_mutex_);
      auto it = views_.find(view_name);
      if (it == views_.end() || !it->second->is_pending_restore()) {
        return;
      }
    }
    std::unique_lock<std::shared_mutex> view_lock(view_mutex_);
    realize_pending_view_locked(view_name);
  }

  // Count of views whose checkpoint blobs are still stashed (pending) --
  // observable so tests can assert lazy restore actually deferred work,
  // and for the dbsp_load() report string.
  size_t pending_restore_count() const {
    std::shared_lock<std::shared_mutex> lock(view_mutex_);
    return pending_restore_.size();
  }

  // Zero-arg dbsp_save(): snapshot all view definitions into a catalog's
  // _dbsp_views table, so they travel with the database file (and backups).
  // When `catalog` is non-empty the table is created/read in that attached
  // database (e.g. "m"."_dbsp_views"). Full rewrite for a consistent snapshot (D3).
  bool save_to_duck_table(duckdb::ClientContext &context,
                          const std::string &storage_table = "_dbsp_views",
                          const std::string &only_view = "",
                          const std::string &catalog = "") {
    if (!is_valid_identifier(storage_table)) {
      last_error_ = "Invalid storage table name: " + storage_table;
      return false;
    }
    if (!only_view.empty() && !is_valid_identifier(only_view)) {
      last_error_ = "Invalid view name: " + only_view;
      return false;
    }
    if (!catalog.empty() && !is_valid_identifier(catalog)) {
      last_error_ = "Invalid catalog name: " + catalog;
      return false;
    }
    const std::string qtable = qualify(catalog, storage_table);
    std::vector<ViewDefinition> defs;
    {
      std::shared_lock<std::shared_mutex> lock(struct_mutex_);
      for (const auto &[_, def] : view_definitions_) {
        if (only_view.empty() || def.name == only_view) {
          defs.push_back(def);
        }
      }
    }
    if (!only_view.empty() && defs.empty()) {
      last_error_ = "View not found: " + only_view;
      return false;
    }
    std::sort(defs.begin(), defs.end(),
              [](const ViewDefinition &a, const ViewDefinition &b) {
                return a.created_at < b.created_at;
              });
    try {
      // Fresh connection: `context` is mid-query inside a table function.
      InternalQueryGuard guard;
      duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
      con.Query("BEGIN");
      auto res = con.Query(
          "CREATE TABLE IF NOT EXISTS " + qtable +
          " (name VARCHAR PRIMARY KEY, sql VARCHAR, sources VARCHAR, "
          "created_at BIGINT)");
      if (res->HasError()) {
        con.Query("ROLLBACK");
        last_error_ = res->GetError();
        return false;
      }
      if (only_view.empty()) {
        con.Query("DELETE FROM " + qtable);
      } else {
        con.Query("DELETE FROM " + qtable + " WHERE name = '" +
                  only_view + "'");
      }
      auto prep = con.Prepare("INSERT INTO " + qtable +
                              " VALUES ($1, $2, $3, $4)");
      if (!prep || prep->HasError()) {
        con.Query("ROLLBACK");
        last_error_ = prep ? prep->GetError() : "prepare failed";
        return false;
      }
      for (const auto &def : defs) {
        std::string sources_str;
        for (size_t i = 0; i < def.source_tables.size(); i++) {
          if (i > 0) {
            sources_str += ",";
          }
          sources_str += def.source_tables[i];
        }
        // Use Value(string) constructor (VARCHAR) not CreateValue<string>
        // (BLOB). When BLOB is stored into a VARCHAR column DuckDB hex-encodes
        // the bytes (e.g. " → \x22), breaking SQL round-trips on reload.
        duckdb::vector<duckdb::Value> args;
        args.push_back(duckdb::Value(def.name));
        args.push_back(duckdb::Value(def.sql));
        args.push_back(duckdb::Value(sources_str));
        args.push_back(duckdb::Value::BIGINT(static_cast<int64_t>(def.created_at)));
        auto r = prep->Execute(args);
        if (r->HasError()) {
          con.Query("ROLLBACK");
          last_error_ = r->GetError();
          return false;
        }
      }
      con.Query("COMMIT");
      return true;
    } catch (const std::exception &e) {
      last_error_ = std::string("Save failed: ") + e.what();
      return false;
    }
  }

  // Zero-arg dbsp_load(): recreate views from a catalog's _dbsp_views table
  // (created by save_to_duck_table / create_view). When `catalog` is non-empty,
  // reads from that attached database (e.g. "m"."_dbsp_views"). Missing default
  // table = nothing persisted = success. Must NOT hold struct_mutex_: create_view
  // acquires it.
  bool load_from_duck_table(duckdb::ClientContext &context,
                            const std::string &storage_table = "_dbsp_views",
                            const std::string &catalog = "") {
    if (!is_valid_identifier(storage_table)) {
      last_error_ = "Invalid storage table name: " + storage_table;
      return false;
    }
    if (!catalog.empty() && !is_valid_identifier(catalog)) {
      last_error_ = "Invalid catalog name: " + catalog;
      return false;
    }
    const std::string qtable = qualify(catalog, storage_table);
    std::vector<std::pair<std::string, std::string>> rows;
    try {
      InternalQueryGuard guard;
      duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
      // Scope the existence check to the right catalog when one is given.
      std::string exists_sql;
      if (catalog.empty()) {
        exists_sql = "SELECT 1 FROM information_schema.tables WHERE table_name = '" +
                     storage_table + "'";
      } else {
        exists_sql = "SELECT 1 FROM information_schema.tables WHERE table_catalog = '" +
                     catalog + "' AND table_name = '" + storage_table + "'";
      }
      auto exists = con.Query(exists_sql);
      if (exists->HasError() || exists->RowCount() == 0) {
        // Missing default table = nothing persisted = success; an
        // explicitly named missing table is an error.
        if (storage_table == "_dbsp_views") {
          return true;
        }
        last_error_ = "Storage table not found: " + qtable;
        return false;
      }
      auto res = con.Query("SELECT name, sql FROM " + qtable +
                           " ORDER BY created_at");
      if (res->HasError()) {
        last_error_ = res->GetError();
        return false;
      }
      for (duckdb::idx_t i = 0; i < res->RowCount(); i++) {
        // Use StringValue::Get() for raw VARCHAR content — Value::ToString()
        // escapes characters like " as \x22, which breaks the stored SQL.
        auto name_val = res->GetValue(0, i);
        auto sql_val = res->GetValue(1, i);
        rows.emplace_back(duckdb::StringValue::Get(name_val),
                          duckdb::StringValue::Get(sql_val));
      }
    } catch (const std::exception &e) {
      last_error_ = std::string("Load failed: ") + e.what();
      return false;
    }
    // Fast path (D3b): when a circuit-state checkpoint exists and every
    // source watermark matches the live tables, cold-create the covered
    // views (sources tracked + synced + arrangements backfilled, but no
    // circuit replay) and inject their operator state + sink result.
    CkptData ckpt;
    const bool have_ckpt = checkpoint_valid(context, ckpt, catalog);

    // D3c: hand the verified watermarks to track_table_internal so sources
    // auto-tracked during the cold creates below start DEFERRED (no
    // baseline scan at load). Cleared after the loop — normal DDL paths
    // never defer.
    if (have_ckpt) {
      std::lock_guard<std::mutex> g(seeds_mutex_);
      deferred_seeds_ = ckpt.watermarks;
    }

    size_t loaded = 0;
    size_t ckpt_restored = 0;
    size_t skipped = 0;
    size_t pending_now_count = 0;
    const bool lazy = lazy_restore_.load();
    for (const auto &[name, view_sql] : rows) {
      {
        std::shared_lock<std::shared_mutex> lock(struct_mutex_);
        if (views_.count(name)) {
          skipped++;
          continue; // already live in this session
        }
      }
      // Finding 1: a per-view checkpoint is only trusted when its saved
      // SQL fingerprint matches the definition about to be used to
      // cold-create it. A mismatch (view replaced/redefined since the
      // checkpoint was written, e.g. via dbsp_replace_view, without a
      // fresh save landing before this load) declines the fast path for
      // this view alone — every other view's checkpoint is unaffected.
      const bool fingerprint_ok = have_ckpt &&
                                  ckpt.sql_fingerprints.count(name) > 0 &&
                                  ckpt.sql_fingerprints.at(name) == view_sql;
      const bool cold =
          have_ckpt && ckpt.sinks.count(name) > 0 && fingerprint_ok;
      if (have_ckpt && ckpt.sinks.count(name) > 0 && !fingerprint_ok &&
          std::getenv("DBSP_DEBUG_SYNC")) {
        std::cerr << "[dbsp] checkpoint declined for '" << name
                  << "': SQL fingerprint mismatch (view redefined since "
                     "save) -- rebuilding by replay\n";
      }
      if (create_view(context, name, view_sql, /*skip_init_replay=*/cold)) {
        if (cold && lazy) {
          // D-lazy: stash the already-read blobs undecoded instead of
          // calling restore_view_state eagerly -- realize_pending_view[_
          // locked] decodes them on first need. stash_pending_view cannot
          // itself fail the way the eager decode below can (it copies
          // bytes, it doesn't parse them), so there is no per-view decline
          // branch here: a corrupt stash surfaces later, at realize time.
          stash_pending_view(name, ckpt);
          ckpt_restored++;
          pending_now_count++;
          loaded++;
        } else if (cold && !restore_view_state(name, ckpt)) {
          // Corrupt/mismatched blob: rebuild this view the normal way
          drop_view(name);
          if (create_view(context, name, view_sql)) {
            loaded++;
          }
        } else {
          if (cold) {
            ckpt_restored++;
          }
          loaded++;
        }
      }
      // Continue on individual failures (e.g. a source table was dropped)
    }
    {
      std::lock_guard<std::mutex> g(seeds_mutex_);
      deferred_seeds_.clear();
    }
    {
      // Count sources that ended up deferred (observable in the
      // dbsp_load() message; the regression test asserts on it)
      size_t deferred_now = 0;
      std::shared_lock<std::shared_mutex> lock(struct_mutex_);
      for (const auto &[_, tt] : tracked_tables_) {
        if (tt->is_deferred()) {
          deferred_now++;
        }
      }
      last_deferred_count_ = deferred_now;
    }
    last_loaded_count_ = loaded;
    last_ckpt_restored_count_ = ckpt_restored;
    last_skipped_count_ = skipped;
    last_pending_restore_count_ = pending_now_count;
    return true;
  }

  size_t last_loaded_count() const { return last_loaded_count_; }
  // Views restored from the circuit-state checkpoint (vs full replay) in
  // the last load_from_duck_table call.
  size_t last_ckpt_restored_count() const { return last_ckpt_restored_count_; }
  // Views whose circuit state made it into the last saved checkpoint.
  size_t last_ckpt_saved_count() const { return last_ckpt_saved_count_; }
  // Sources whose baselines were left deferred (lazy, D3c) by the last
  // load_from_duck_table call — a watermark-matched restore reads zero
  // source rows.
  size_t last_deferred_sources_count() const { return last_deferred_count_; }
  // Rows the last load_from_duck_table call skipped because the view was
  // already live in this session (see field comment).
  size_t last_skipped_count() const { return last_skipped_count_; }
  // Views left pending (checkpoint blobs stashed, not decoded) by the last
  // load_from_duck_table call (D-lazy; 0 when lazy_restore_ is off).
  size_t last_pending_restore_count() const {
    return last_pending_restore_count_;
  }

  // --- D3c lazy baselines: cross-cutting hooks ---------------------------

  // True while any tracked table's baseline is deferred (checkpoint fast
  // path, not yet materialized). Atomic: hot paths check it lock-free.
  bool has_deferred() const { return deferred_tables_.load() > 0; }

  // True when an out-of-band change was detected against a deferred
  // baseline: the restore-time baseline is unrecoverable, so dependent
  // view state cannot be reconciled incrementally. QueryBegin calls
  // rebuild_all_views at the next statement boundary.
  bool rebuild_pending() const { return rebuild_pending_.load(); }

  // Materialize every deferred baseline NOW, from pre-write storage.
  // Called by QueryBegin before a write statement executes: once the
  // write commits, the restore-time table content is gone and a later
  // scan-and-diff sync could no longer compute the reconciliation delta.
  void materialize_all_deferred(duckdb::ClientContext &context) {
    if (!has_deferred()) {
      return;
    }
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    for (const auto &[name, tt] : tracked_tables_) {
      auto lock_it = table_locks_.find(name);
      if (lock_it == table_locks_.end()) {
        continue;
      }
      std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
      if (!tt->is_deferred()) {
        continue;
      }
      try {
        materialize_deferred_locked(context, name, nullptr, false);
      } catch (const std::exception &e) {
        rebuild_pending_ = true;
        std::cerr << "DBSP: deferred baseline materialization failed for '"
                  << name << "': " << e.what() << "\n";
      }
    }
  }

  // Drop and recreate every view from its stored definition. Rare
  // correctness escape hatch: only runs after an out-of-band table change
  // invalidated a deferred (lazy-restored) baseline. Views are replayed
  // from committed storage, so the result is exact. Caller must hold no
  // CDC locks (QueryBegin).
  void rebuild_all_views(duckdb::ClientContext &context) {
    if (!rebuild_pending_.exchange(false)) {
      return;
    }
    commit_seq_++; // baselines change wholesale: invalidate in-flight captures
    // Refresh every tracked baseline from committed storage first: after
    // an out-of-band divergence the in-memory baseline is not trustworthy
    // (e.g. a trailing notify double-applied against a scan that already
    // contained it), and the recreated views replay from these baselines.
    {
      std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
      for (const auto &[name, tt] : tracked_tables_) {
        auto lock_it = table_locks_.find(name);
        if (lock_it == table_locks_.end()) {
          continue;
        }
        std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
        TrackedTable &table = *tt; // lambda capture (structured bindings
                                   // are not capturable pre-C++20)
        const bool was_deferred = table.is_deferred();
        try {
          table.begin_rebuild();
          stream_table_rows(context, name, [&](DuckDBRow &&row) {
            table.add_scanned_row(std::move(row));
          });
          table.install_rebuild();
          if (was_deferred) {
            deferred_tables_--;
          }
        } catch (const std::exception &e) {
          std::cerr << "DBSP: baseline refresh failed for '" << name
                    << "': " << e.what() << "\n";
        }
      }
    }
    std::vector<ViewDefinition> defs;
    {
      std::shared_lock<std::shared_mutex> lock(struct_mutex_);
      for (const auto &[_, def] : view_definitions_) {
        defs.push_back(def);
      }
    }
    std::sort(defs.begin(), defs.end(),
              [](const ViewDefinition &a, const ViewDefinition &b) {
                return a.created_at < b.created_at;
              });
    // Drop dependents first: retry passes until no progress (drop_view
    // refuses while dependents exist).
    bool progress = true;
    while (progress) {
      progress = false;
      for (auto it = defs.rbegin(); it != defs.rend(); ++it) {
        if (is_view_registered(it->name) && drop_view(it->name)) {
          progress = true;
        }
      }
    }
    for (const auto &def : defs) {
      if (!create_view(context, def.name, def.sql)) {
        std::cerr << "DBSP: rebuild of view '" << def.name
                  << "' failed: " << last_error_ << "\n";
      }
    }
  }

  // Canonical key for a bare/qualified ref. Prefers catalog resolution;
  // when that is unavailable (autocommit callers have no transaction, and
  // starting one here can deadlock inside executing queries), falls back to
  // textual matching against the tracked-key map. Unresolvable refs pass
  // through so lookups fail with the normal "not tracked" error.
  // Caller must hold struct_mutex_ (shared or exclusive) for the fallback.
  std::string canonicalize_or_passthrough(duckdb::ClientContext &context,
                                          const std::string &ref) {
    if (context.transaction.HasActiveTransaction()) {
      auto entry = resolve_table_entry(context, ref);
      if (entry) {
        return canonical_table_key(*entry);
      }
    }
    if (tracked_tables_.count(ref)) {
      return ref; // already a canonical key
    }
    try {
      const auto &def = duckdb::DatabaseManager::GetDefaultDatabase(context);
      const auto dots = std::count(ref.begin(), ref.end(), '.');
      if (dots == 0) {
        std::string guess = def + ".main." + ref;
        if (tracked_tables_.count(guess)) {
          return guess;
        }
      } else if (dots == 1) {
        const auto dot = ref.find('.');
        // "a.t": a as catalog (attached db, default schema) …
        std::string guess = ref.substr(0, dot) + ".main." + ref.substr(dot + 1);
        if (tracked_tables_.count(guess)) {
          return guess;
        }
        // … or a as schema of the default catalog
        guess = def + "." + ref;
        if (tracked_tables_.count(guess)) {
          return guess;
        }
      }
    } catch (...) {
    }
    return ref;
  }

  // Track a DuckDB table for CDC
  bool track_table(duckdb::ClientContext &context,
                   const std::string &table_ref) {
    std::unique_lock<std::shared_mutex> lock(struct_mutex_);

    // Validate the (possibly dotted) reference to prevent SQL injection,
    // then key by canonical catalog.schema.table (D2): 'li_1' and
    // 'm.li_1' resolve to the same tracked table.
    if (!is_valid_table_reference(table_ref)) {
      last_error_ =
          "Invalid table name (identifier parts, optionally dotted): " +
          table_ref;
      return false;
    }
    auto entry = resolve_table_entry(context, table_ref);
    if (!entry) {
      last_error_ = "Table not found: " + table_ref;
      return false;
    }
    const std::string table_name = canonical_table_key(*entry);

    if (tracked_tables_.count(table_name)) {
      return true; // Already tracked
    }

    TableSchema schema;
    if (!get_table_schema(context, table_name, schema)) {
      return false;
    }

    tracked_tables_[table_name] =
        std::make_unique<TrackedTable>(table_name, schema);
    if (spill_enabled_) {
      tracked_tables_[table_name]->enable_spill(spill_path(table_name));
    }
    table_schemas_[table_name] = schema;
    table_locks_[table_name] = std::make_unique<std::shared_mutex>();

    // Note: Initial table sync deferred to avoid SQL query deadlock
    // User should call dbsp_sync() after dbsp_track() to populate initial data

    return true;
  }

  // Untrack a table
  void untrack_table(const std::string &table_name) {
    std::unique_lock<std::shared_mutex> lock(struct_mutex_);
    {
      auto it = tracked_tables_.find(table_name);
      if (it != tracked_tables_.end() && it->second->is_deferred()) {
        deferred_tables_--;
      }
    }
    tracked_tables_.erase(table_name);
    table_schemas_.erase(table_name);
    table_locks_.erase(table_name);
    dep_graph_.remove_node(table_name);
  }

  // Create a materialized view from SQL
  // Supports referencing other views (cascading views)
  bool create_view(duckdb::ClientContext &context, const std::string &view_name,
                   const std::string &sql, bool skip_init_replay = false) {
    // Never auto-load over a registry the user (or a prior auto-load) has
    // started populating — belt-and-suspenders alongside maybe_autoload's
    // own check, so any create_view caller trips this regardless of
    // whether it went through one of the wrapped entry points first.
    autoload_attempted_ = true;
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

    // Save to _dbsp_views table (ignore errors for now). Gated on
    // autopersist_ (Feature 1): this incremental best-effort write is what
    // makes a later auto-load see the view at all, so autopersist_ is the
    // single master switch for every implicit persistence side effect —
    // this one, and the on-close save/checkpoint. Turning it off (bulk
    // loads) means exactly that: nothing about this session is persisted
    // automatically, matching dbsp_auto_sync's own bulk-load contract.
    if (autopersist_) {
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
        con.Query("CREATE TABLE IF NOT EXISTS _dbsp_views (name VARCHAR "
                  "PRIMARY KEY, sql VARCHAR, sources VARCHAR, created_at "
                  "BIGINT)");
        auto res = con.Query(insert_sql);
        // Ignore errors - persistence is best-effort
      } catch (const std::exception &e) {
        // Ignore persistence errors - log for debugging
        last_error_ = std::string("Persistence error: ") + e.what();
      } catch (...) {
        last_error_ = "Unknown persistence error";
      }
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
        register_arrangements(context, *pview, skip_init_replay);
      }

      // Initialize view with current data from sources
      // struct_mutex_ held exclusively so no concurrent mutations to tracked
      // tables or other views can occur; reads are safe without table_locks_
      // Checkpoint restore (D3b) skips this replay: node state and the sink
      // result are injected from the checkpoint instead.
      for (const auto &source :
           skip_init_replay ? std::vector<std::string>{} : resolved_sources) {
        if (pview && pview->shared_init_skip().count(source)) {
          continue;
        }
        if (tracked_tables_.count(source)) {
          // D3c: replay needs real table state — materialize a deferred
          // baseline first (struct_mutex_ exclusive: no table lock needed;
          // view_mutex_ already held)
          if (tracked_tables_.at(source)->is_deferred()) {
            materialize_deferred_locked(context, source, nullptr,
                                        /*view_lock_held=*/true);
          }
          // Stream the baseline in bounded chunks: deltas are additive,
          // so N smaller applies equal one big one — and spill mode never
          // materializes the whole table in RAM
          const auto &table = tracked_tables_.at(source);
          DuckDBZSet chunk;
          table->scan_state([&](const DuckDBRow &row, int64_t w) {
            chunk.insert(row, w);
            if (chunk.size() >= 65536) {
              view->apply_changes(source, chunk);
              chunk = DuckDBZSet();
            }
          });
          // Final chunk applies even when empty: global aggregates emit
          // their one row only when the circuit steps
          view->apply_changes(source, chunk);
        } else if (views_.count(source)) {
          // D-lazy: a warm create replaying a view source needs that
          // source's real result, not the cold-create empty default a
          // still-pending source would otherwise hand back silently.
          // Caller (create_view) already holds view_mutex_ exclusively.
          realize_pending_view_locked(source);
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
  //
  // `cold` = checkpoint fast-path create (skip_init_replay): the view's
  // node state comes from the checkpoint, so arrangements over deferred
  // tables may stay empty (needs_backfill) until the baseline
  // materializes. A warm create replays sources through the circuit and
  // join nodes read arrangements during that replay — deferred sources
  // must materialize first (D3c).
  void register_arrangements(duckdb::ClientContext &context,
                             PlannedCircuitView &pview, bool cold) {
    for (const auto &req : pview.arrangement_requests()) {
      const bool source_is_table = tracked_tables_.count(req.table) > 0;
      const bool source_is_view = views_.count(req.table) > 0;
      if (!source_is_table && !source_is_view) {
        continue; // untracked — node keeps its local index
      }
      if (source_is_table && !cold &&
          tracked_tables_.at(req.table)->is_deferred()) {
        materialize_deferred_locked(context, req.table, nullptr,
                                    /*view_lock_held=*/true);
      }
      // D-lazy: an arrangement over a VIEW source is seeded below from
      // that view's full get_result() (no table-side "defer_fill" analog
      // exists for views — the backfill is either done now or never,
      // there is no needs_backfill sweep for view-named arrangement
      // sources), so a still-pending source view must be realized first,
      // regardless of `cold` (this governs the CURRENT view being
      // created, not req.table's own pending state). Caller
      // (create_view) already holds view_mutex_ exclusively.
      if (source_is_view) {
        realize_pending_view_locked(req.table);
      }
      const bool defer_fill =
          source_is_table && tracked_tables_.at(req.table)->is_deferred();
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
        if (spill_enabled_) {
          arr->enable_spill(spill_dir_ + "/arr_" +
                            std::to_string(arrangement_file_seq_++) +
                            ".dbspill");
        }
        // Backfill full current state so init replay can skip this table.
        // D3c: over a still-deferred table (cold create) the backfill is
        // deferred with the baseline — flagged so materialization fills it.
        if (defer_fill) {
          arr->needs_backfill = true;
        } else {
          DbspScopeTimer timer("arr_backfill", req.table);
          if (source_is_table) {
            DuckDBZSet chunk;
            tracked_tables_.at(req.table)
                ->scan_state([&](const DuckDBRow &row, int64_t w) {
                  chunk.insert(row, w);
                  if (chunk.size() >= 65536) {
                    arr->apply(chunk);
                    chunk = DuckDBZSet();
                  }
                });
            if (!chunk.empty()) {
              arr->apply(chunk);
            }
          } else {
            arr->apply(views_.at(req.table)->get_result());
          }
        }
        arrangements_[req.fingerprint] = arr;
        arrangements_by_table_[req.table].push_back(arr);
      }
      req.node->set_shared_arrangement(req.left_side, arr,
                                       req.consumer_projection);
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
    // D-lazy: realize-or-discard -- dropping the view makes any stashed
    // checkpoint blobs moot, so just discard them (cheaper than decoding
    // first only to throw the result away).
    pending_restore_.erase(view_name);
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
      pending_restore_.erase(dep); // D-lazy: discard, see drop_view
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
    pending_restore_.erase(view_name); // D-lazy: discard, see drop_view
    return views_.erase(view_name) > 0;
  }

  // Feature 2 (dbsp_replace_view / CREATE OR REPLACE MATERIALIZED VIEW):
  // change one view's definition; only it and its transitive dependents
  // repopulate. Upstream state (its own sources, and anything outside the
  // subtree) is untouched, and every dependent keeps its original DDL —
  // only `view_name` gets `new_sql`.
  //
  // Lock discipline mirrors load_from_duck_table (see its comment above):
  // snapshot the dependent DDL under a shared lock, then drop/create
  // unlocked — create_view and drop_view_cascade each take struct_mutex_
  // themselves, and it is not reentrant.
  //
  // Failure semantics (spec §2): no transactional rollback in v1. If a
  // create fails mid-sequence, the remaining dependents are still attempted
  // (with their saved old DDL) and last_error_ accumulates a full report of
  // what failed and which views are still queryable afterward. Returns
  // false on any failure; the registry itself is left internally
  // consistent either way (create_view/drop_view_cascade never leave a
  // partially-registered view).
  bool replace_view(duckdb::ClientContext &context, const std::string &view_name,
                    const std::string &new_sql) {
    std::vector<ViewDefinition> dependent_defs;
    // Full registry name snapshot, taken in the same critical section as
    // dependent_defs (so this doesn't widen the race window with a second
    // lock acquisition) — used below to detect views that got swept into
    // the cascade drop without ever being in our saved-DDL list (TOCTOU: a
    // view created onto the subtree after this snapshot but before
    // drop_view_cascade's own later, exclusive recomputation).
    std::vector<std::string> pre_drop_names;
    {
      std::shared_lock<std::shared_mutex> lock(struct_mutex_);
      if (!views_.count(view_name)) {
        ErrorInfo info;
        info.code = ErrorCode::VIEW_NOT_FOUND;
        info.message = get_message(ErrorCode::VIEW_NOT_FOUND) + ": " + view_name;
        info.context = "View: " + view_name;
        info.workaround = get_workaround(ErrorCode::VIEW_NOT_FOUND);
        last_error_ = format_error(info);
        return false;
      }
      // topological_order() filters to the dependent set and orders it so
      // that a dependent's in-set dependencies precede it — exactly the
      // recreation order needed once `view_name` itself exists again.
      for (const auto &dep : dep_graph_.topological_order(view_name)) {
        auto it = view_definitions_.find(dep);
        if (it != view_definitions_.end()) {
          dependent_defs.push_back(it->second);
        }
      }
      pre_drop_names.reserve(views_.size());
      for (const auto &[name, _] : views_) {
        pre_drop_names.push_back(name);
      }
    }

    // Drop the subtree (view_name + all transitive dependents) via the
    // existing cascade path. No `context` here — like every other
    // drop_view_cascade caller in this file, we're running inside a table
    // function on this ClientContext already (mid-query); drop_view_cascade
    // would call context->Query() to delete the _dbsp_views row, which
    // deadlocks against the context's own query lock. Skipping that delete
    // is harmless for the common case: create_view() below re-inserts
    // (INSERT OR REPLACE) the row for every successfully recreated view
    // when autopersist is on. Views that never get recreated (failure path
    // below) get their stale row cleaned up explicitly instead.
    if (!drop_view_cascade(view_name)) {
      return false; // last_error_ already set by drop_view_cascade
    }

    // Belt-and-braces alongside the SQL-fingerprint guard in
    // checkpoint_valid (Finding 1): the whole dropped subtree's
    // checkpoint blobs are stale the moment this cascade runs, regardless
    // of whether the recreates below succeed — proactively erase them so
    // an unclean close before the next save_checkpoint() can't leave a
    // _dbsp_ckpt row for a definition this name no longer has. The
    // fingerprint check is the correctness backstop either way; this just
    // closes the window earlier.
    erase_persisted_checkpoint_rows(context, view_name);
    for (const auto &def : dependent_defs) {
      erase_persisted_checkpoint_rows(context, def.name);
    }

    std::vector<std::string> failed_names;  // for _dbsp_views cleanup
    std::vector<std::string> failure_lines; // for the human-readable report

    if (!create_view(context, view_name, new_sql)) {
      failed_names.push_back(view_name);
      failure_lines.push_back(view_name + ": " + last_error_);
    }
    for (const auto &def : dependent_defs) {
      if (!create_view(context, def.name, def.sql)) {
        failed_names.push_back(def.name);
        failure_lines.push_back(def.name + ": " + last_error_);
      }
    }

    // TOCTOU detection: diff the pre-drop registry snapshot against the
    // current one. Anything that's gone, isn't `view_name`, and wasn't one
    // of our intentionally-dropped dependents was swept up by
    // drop_view_cascade's own (fresh, exclusive-lock) recomputation of the
    // dependent set — i.e. a view created concurrently onto the subtree in
    // the snapshot/drop window. We never saved its DDL, so it can't be
    // recreated here; report it rather than silently losing it.
    {
      std::unordered_set<std::string> known_names;
      known_names.insert(view_name);
      for (const auto &def : dependent_defs) {
        known_names.insert(def.name);
      }
      std::shared_lock<std::shared_mutex> lock(struct_mutex_);
      for (const auto &name : pre_drop_names) {
        if (known_names.count(name)) {
          continue;
        }
        if (!views_.count(name)) {
          failed_names.push_back(name);
          failure_lines.push_back(
              name + ": concurrently created, definition unknown, recreate "
                     "manually");
        }
      }
    }

    if (!failure_lines.empty()) {
      // Keep the registry, the DuckDB catalog, and _dbsp_views persistence
      // in agreement about which views are gone: an old row surviving here
      // would let a crash before the next clean-shutdown save auto-load a
      // stale definition later, in the wrong order relative to its
      // already-recreated (freshly re-timestamped) ancestor.
      for (const auto &name : failed_names) {
        erase_persisted_view_row(context, name);
      }

      std::string still_live;
      for (const auto &def : dependent_defs) {
        if (is_view_registered(def.name)) {
          if (!still_live.empty()) still_live += ", ";
          still_live += def.name;
        }
      }
      if (is_view_registered(view_name)) {
        still_live = still_live.empty() ? view_name : view_name + ", " + still_live;
      }

      ErrorInfo info;
      info.code = ErrorCode::CASCADE_UPDATE_FAILED;
      std::string details = "replace_view('" + view_name + "') lost " +
                            std::to_string(failure_lines.size()) +
                            " view(s):\n";
      for (const auto &line : failure_lines) {
        details += "  - " + line + "\n";
      }
      info.message = details;
      info.context = "View: " + view_name + " — still queryable: " +
                     (still_live.empty() ? "(none)" : still_live);
      info.workaround = get_workaround(ErrorCode::CASCADE_UPDATE_FAILED);
      last_error_ = format_error(info);
      return false;
    }

    return true;
  }

  // Best-effort delete of one view's persisted _dbsp_views row. Used by
  // replace_view's failure path so a permanently-lost view (dropped but
  // never successfully recreated) doesn't leave a stale row behind for a
  // later dbsp_load()/auto-load to resurrect. Same fresh-Connection
  // pattern as create_view's own INSERT (see its comment): must NOT run
  // the query on `context` directly — replace_view executes inside a
  // table function already mid-query on it, and context->Query() would
  // deadlock (the same reason drop_view_cascade is called without a
  // context above).
  void erase_persisted_view_row(duckdb::ClientContext &context,
                                const std::string &name) {
    if (!autopersist_) {
      return; // matches create_view's own persistence gate
    }
    try {
      InternalQueryGuard guard;
      duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
      con.Query("DELETE FROM _dbsp_views WHERE name = '" + name + "'");
      // Ignore errors (including "table does not exist") — persistence is
      // best-effort throughout this file.
    } catch (...) {
    }
  }

  // Best-effort delete of one view's persisted _dbsp_ckpt rows (all kinds:
  // node/sink/sql). Called from replace_view (Finding 1 belt-and-braces):
  // proactively drops now-stale checkpoint blobs for a view whose
  // definition just changed, rather than relying solely on the SQL-
  // fingerprint mismatch in checkpoint_valid to decline the fast path at
  // the next load. Unlike erase_persisted_view_row, this is NOT gated on
  // autopersist_: dbsp_save()/dbsp_close() can write a checkpoint
  // regardless of the autopersist setting, so a checkpoint can exist even
  // with autopersist off — exactly the scenario Finding 1 describes. Same
  // fresh-Connection pattern as erase_persisted_view_row (see its
  // comment): must NOT run on `context` directly.
  void erase_persisted_checkpoint_rows(duckdb::ClientContext &context,
                                       const std::string &name) {
    try {
      InternalQueryGuard guard;
      duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
      con.Query("DELETE FROM _dbsp_ckpt WHERE name = '" + name + "'");
      // Ignore errors (including "table does not exist") — persistence is
      // best-effort throughout this file.
    } catch (...) {
    }
  }

  // Record an INSERT. `context` enables lazy-baseline materialization
  // (D3c): the first notified delta after a checkpoint restore triggers
  // the deferred table scans. Context-less callers on a deferred manager
  // fall back to a scheduled full rebuild (correct, not incremental).
  void on_insert(const std::string &table_name, const DuckDBRow &row,
                 duckdb::ClientContext *context = nullptr) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);

    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end())
      return;

    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end())
      return;

    if (has_deferred()) {
      DuckDBZSet pending;
      pending.insert(row, 1);
      if (!prepare_deferred_for_delta(context, table_name, pending)) {
        return;
      }
    }

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
  void on_delete(const std::string &table_name, const DuckDBRow &row,
                 duckdb::ClientContext *context = nullptr) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);

    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end())
      return;

    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end())
      return;

    if (has_deferred()) {
      DuckDBZSet pending;
      pending.insert(row, -1);
      if (!prepare_deferred_for_delta(context, table_name, pending)) {
        return;
      }
    }

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
                 const DuckDBRow &new_row,
                 duckdb::ClientContext *context = nullptr) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);

    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end())
      return;

    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end())
      return;

    if (has_deferred()) {
      DuckDBZSet pending;
      pending.insert(old_row, -1);
      pending.insert(new_row, 1);
      if (!prepare_deferred_for_delta(context, table_name, pending)) {
        return;
      }
    }

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
                       const std::vector<DuckDBRow> &rows,
                       duckdb::ClientContext *context = nullptr) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);

    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end())
      return;

    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end())
      return;

    if (has_deferred()) {
      DuckDBZSet pending;
      for (const auto &row : rows) {
        pending.insert(row, 1);
      }
      if (!prepare_deferred_for_delta(context, table_name, pending)) {
        return;
      }
    }

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
                  const std::string &table_ref) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    // Accept bare or qualified refs from any caller; keys are canonical (D2)
    const std::string table_name = canonicalize_or_passthrough(context, table_ref);

    if (tracked_tables_.find(table_name) == tracked_tables_.end()) {
      if (std::getenv("DBSP_DEBUG_SYNC")) { std::cerr << "[dbsp] sync: not tracked: " << table_name << "\n"; }
      return false;
    }

    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end()) {
      if (std::getenv("DBSP_DEBUG_SYNC")) { std::cerr << "[dbsp] sync: no lock: " << table_name << "\n"; }
      return false;
    }

    std::optional<DuckDBZSet> delta_opt;
    {
      std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
      delta_opt = sync_table_scan_and_consume(context, table_name);
    }

    if (!delta_opt.has_value()) {
      if (std::getenv("DBSP_DEBUG_SYNC")) { std::cerr << "[dbsp] sync: scan nullopt: " << last_error_ << "\n"; }
      return false;
    }

    if (!delta_opt->empty()) {
      ensure_no_deferred_before_propagate(context, table_name);
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
                   const std::vector<std::string> &table_refs,
                   bool do_parallel,
                   duckdb::MetaTransaction *meta_transaction = nullptr) {
    std::vector<std::string> table_names;
    table_names.reserve(table_refs.size());
    {
      std::shared_lock<std::shared_mutex> lock(struct_mutex_);
      for (const auto &ref : table_refs) {
        table_names.push_back(canonicalize_or_passthrough(context, ref));
      }
    }

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
            ensure_no_deferred_before_propagate(context, table_name);
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
          ensure_no_deferred_before_propagate(context, name);
          propagate_changes(name, *delta_opt);
        }
      }
    }
  }

  // Enable/disable baseline spilling (Phase K1). Existing tables migrate
  // immediately: enabling moves their baselines to disk record logs,
  // disabling reloads them into RAM. New tables follow the current mode.
  bool set_spill(bool enable) {
    std::unique_lock<std::shared_mutex> struct_lock(struct_mutex_);
    if (enable == spill_enabled_) {
      return true;
    }
    if (enable) {
      namespace fs = std::filesystem;
      std::error_code ec;
      const fs::path tmp = fs::temp_directory_path(ec);
      // Self-cleaning: sweep spill directories left by processes that
      // are gone (crashes, kills — normal teardown discards files but
      // the per-pid directory lingers). Keeps the temp dir from
      // accumulating one directory per test run.
      for (const auto &entry : fs::directory_iterator(tmp, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("dbsp_spill_", 0) != 0) {
          continue;
        }
        const pid_t pid =
            static_cast<pid_t>(std::atoll(name.c_str() + 11));
        if (pid > 0 && pid != ::getpid() && ::kill(pid, 0) == -1 &&
            errno == ESRCH) {
          std::error_code rec;
          fs::remove_all(entry.path(), rec);
        }
      }
      spill_dir_ =
          (tmp / ("dbsp_spill_" + std::to_string(::getpid()))).string();
      fs::create_directories(spill_dir_, ec);
      if (ec) {
        last_error_ = "Cannot create spill directory: " + spill_dir_;
        return false;
      }
    }
    for (auto &[name, table] : tracked_tables_) {
      auto lock_it = table_locks_.find(name);
      std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
      if (enable) {
        table->enable_spill(spill_path(name));
      } else {
        table->disable_spill();
      }
    }
    {
      // Arrangement content is guarded by view_mutex_
      // (lock order: struct_mutex_ → view_mutex_)
      std::unique_lock<std::shared_mutex> view_lock(view_mutex_);
      for (auto it = arrangements_.begin(); it != arrangements_.end();) {
        auto arr = it->second.lock();
        if (!arr) {
          it = arrangements_.erase(it);
          continue;
        }
        if (enable) {
          arr->enable_spill(spill_dir_ + "/arr_" +
                            std::to_string(arrangement_file_seq_++) +
                            ".dbspill");
        } else {
          arr->disable_spill();
        }
        ++it;
      }
    }
    spill_enabled_ = enable;
    g_spill_mode.store(enable);
    if (enable) {
      g_spill_dir = spill_dir_;
    }
    return true;
  }

  bool spill_enabled() const {
    std::shared_lock<std::shared_mutex> lock(struct_mutex_);
    return spill_enabled_;
  }

  // Enable/disable parallel sync
  void set_parallel_sync(bool enable) {
    std::unique_lock<std::shared_mutex> lock(struct_mutex_);
    use_parallel_sync_ = enable;
    // One knob: view-level (I2) and intra-operator (L2) parallelism
    g_intraop_shards.store(
        enable ? static_cast<int>(std::min<unsigned>(
                     8, std::max<unsigned>(
                            2, std::thread::hardware_concurrency())))
               : 0);
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
    // D-lazy: get_result() on a still-pending view is the cold-create
    // empty default -- realize first (self-locking, no-op if not pending).
    realize_pending_view(view_name);
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
  // D-lazy: unlike query_view/scan_view, this does NOT realize a pending
  // view -- it has no production callers (test/debug only; grep confirms),
  // and none of its current test call sites exercise the checkpoint fast
  // path, so a get_result()/scan() on the raw pointer it returns would see
  // the cold-create empty default if ever called on a pending view. Known
  // gap: a future test against a lazy-restored view must go through
  // scan_view()/query_view() instead of get_view(), or call
  // realize_pending_view() itself first.
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
    // D-lazy: dbsp_query's read path -- the primary "first touch decodes
    // this view's chain" trigger. No-op if not pending.
    realize_pending_view(view_name);
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

  // Copy the view's last-sync output delta under the read locks
  // (rows added/removed by the most recent propagation, weight ±n).
  // Single-generation: overwritten by the next sync that touches the view.
  // Returns false when the view does not exist.
  // D-lazy: deliberately does NOT realize a pending view. get_delta() is
  // "rows changed by the most recent apply_changes_batch"; restore_view_
  // state/realize_pending_view[_locked] only ever call set_result() (the
  // sink), never touch the delta field, so a still-pending (never
  // stepped) view's delta is legitimately empty either way -- decoding
  // its stash here would pay the blob_decode cost for a value realization
  // cannot change, defeating the point of staying lazy.
  bool scan_view_delta(const std::string &view_name,
                       const std::function<void(const DuckDBRow &, Weight)> &cb) {
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    std::shared_lock<std::shared_mutex> view_lock(view_mutex_);
    auto it = views_.find(view_name);
    if (it == views_.end()) {
      return false;
    }
    for (const auto &[row, weight] : it->second->get_delta()) {
      cb(row, weight);
    }
    return true;
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
    return it->second->state_total_weight();
  }

  // Apply a delta captured from a transaction's local storage (G2 fast
  // path): O(delta) — no table scan, no diff. The caller has already
  // validated the delta against the committed table (count guard).
  // `context` enables lazy-baseline materialization (D3c); without it a
  // deferred manager rejects the fast path (caller falls back to sync).
  bool apply_captured_delta(const std::string &table_name,
                            const DuckDBZSet &delta,
                            duckdb::ClientContext *context = nullptr) {
    if (delta.empty()) {
      return true;
    }
    DbspScopeTimer t_total("apply_captured_delta",
                           table_name + " rows=" +
                               std::to_string(delta.size()));
    std::shared_lock<std::shared_mutex> struct_lock(struct_mutex_);
    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end()) {
      return false;
    }
    auto lock_it = table_locks_.find(table_name);
    if (lock_it == table_locks_.end()) {
      return false;
    }
    if (has_deferred() &&
        !prepare_deferred_for_delta(context, table_name, delta)) {
      // Rebuild scheduled (or no context): the committed rows are already
      // in storage, so the rebuild/scan fallback reconciles them.
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

  // Monotonic count of baseline mutations; UPDATE/DELETE write-capture
  // snapshots it at transaction begin and falls back when it moved by
  // commit time (an interleaved commit may have invalidated the capture's
  // committed-state read). See docs/DESIGN_WRITE_CAPTURE.md.
  uint64_t commit_seq() const { return commit_seq_; }

  // Write-capture commit-guard failures (each one fell back to
  // scan-and-diff — loud, countable, never silent)
  uint64_t capture_guard_fallbacks() const { return capture_guard_fallbacks_; }
  void note_capture_guard_fallback() { capture_guard_fallbacks_++; }

  // Test knob: force the scan-and-diff path for differential comparison
  bool write_capture_enabled() const { return write_capture_enabled_; }
  void set_write_capture_enabled(bool enabled) {
    write_capture_enabled_ = enabled;
  }

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
      return it->second->state_size();
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
      // D-lazy: get_result() on a still-pending view is the cold-create
      // empty default, not its real row count -- dbsp_views() would
      // otherwise report 0 rows for every untouched view right after
      // reopen, an observable divergence from eager mode. The sink blob's
      // own leading u64 length prefix (BlobWriter's sink format: u64
      // count, then `count` (row, weight) pairs -- see save_checkpoint)
      // already IS the exact row count, readable in 8 bytes with no row
      // decode. Deliberately does NOT call realize_pending_view_locked:
      // reporting metadata about a pending view must not itself defeat
      // the laziness dbsp_lazy_restore exists to provide (dbsp_views()
      // enumerates every view -- realizing here would eagerly decode
      // everything on the first `dbsp_views()` call, same as if lazy
      // restore were off).
      auto pend_it = it->second->is_pending_restore()
                         ? pending_restore_.find(view_name)
                         : pending_restore_.end();
      if (pend_it != pending_restore_.end()) {
        try {
          BlobReader r(pend_it->second.sink.data(),
                       pend_it->second.sink.size());
          info.row_count = static_cast<size_t>(r.u64());
        } catch (...) {
          // Corrupt/truncated stash: unknown row count, not a crash --
          // the same corruption surfaces properly (rebuild scheduled) the
          // first time something actually realizes this view.
          info.row_count = 0;
        }
      } else {
        info.row_count = it->second->get_result().size();
      }
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
  // NOTE: set_result() fills only a view's sink; internal circuit-node
  // state (aggregate groups, join indexes, sort/limit multisets, recursive
  // dedup) cannot be reconstructed from the sink alone. Checkpoint-covered
  // views restore circuit state via load_from_duck_table's D3b fast path
  // (watermark-guarded); everything else rebuilds by replaying committed
  // DuckDB storage through create_view. Recovery routes through
  // load_from_duck_table too — same semantics, same fast path.

private:
  // Typed streaming scan of a table's committed rows through a fresh
  // internal connection; emits each row. Returns the number of rows
  // emitted. Throws std::runtime_error on scan failure.
  //
  // Both paths (commit hook and explicit user call) scan committed state
  // through a fresh Connection. The commit hook fires after Commit() has
  // succeeded, so a new transaction sees the committed rows. Scanning
  // with the just-committed DuckTransaction via the raw storage API
  // stopped seeing its own rows in DuckDB 1.5 (hook now runs before
  // Finalize()), so that path was removed.
  // Guard: OnConnectionOpened must not run first-time recovery here -
  // the calling thread may hold struct_mutex_ and recovery re-acquires it.
  static int64_t
  stream_table_rows(duckdb::ClientContext &context,
                    const std::string &table_key,
                    const std::function<void(DuckDBRow &&)> &emit) {
    InternalQueryGuard guard;
    auto &fresh_db = duckdb::DatabaseInstance::GetDatabase(context);
    duckdb::Connection fresh_con(fresh_db);
    // Streaming execution (H5): rows are consumed chunk by chunk below,
    // so materializing the whole table into a QueryResult first was one
    // extra full-table copy per sync
    auto sql_result =
        fresh_con.SendQuery("SELECT * FROM " + quote_table_key(table_key));
    if (!sql_result || sql_result->HasError()) {
      throw std::runtime_error(
          "Failed to scan table '" + table_key + "': " +
          (sql_result ? sql_result->GetError() : "null result"));
    }
    int64_t total = 0;
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
      // Vectorized row hashes (lazy per-Value hashing was ~2/3 of
      // ingestion time); pre-seeds each row's cache with the identical
      // value the lazy path would compute
      std::vector<size_t> row_hashes;
      chunk_row_hashes(chunk, row_hashes);
      for (idx_t i = 0; i < n; i++) {
        DuckDBRow row;
        row.columns.assign(std::move(vals[i]));
        row.columns.set_hash(row_hashes[i]);
        emit(std::move(row));
      }
      total += static_cast<int64_t>(n);
    }
    return total;
  }

  // Live COUNT(*) + bit_xor(hash) of a table, matching the watermark format
  // written by save_checkpoint. Returns false on query failure.
  static bool live_watermark(duckdb::ClientContext &context,
                             const std::string &table_key, int64_t &count,
                             std::string &hash) {
    try {
      InternalQueryGuard guard;
      duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
      auto wm = con.Query("SELECT COUNT(*), CAST(bit_xor(hash(t)) AS "
                          "VARCHAR) FROM " +
                          quote_table_key(table_key) + " t");
      if (wm->HasError() || wm->RowCount() != 1) {
        return false;
      }
      count = wm->GetValue(0, 0).GetValue<int64_t>();
      hash = wm->GetValue(1, 0).ToString();
      return true;
    } catch (...) {
      return false;
    }
  }

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
    DbspScopeTimer timer("source_sync", table_name);

    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end()) {
      last_error_ = "Table not tracked: " + table_name;
      return std::nullopt;
    }

    // D3c deferred baseline: the table was checkpoint-restored and its
    // baseline never materialized. Recheck the restore-time watermark:
    // a match proves the table is untouched since save — the delta is
    // empty and the baseline can STAY deferred (no scan at all). A
    // mismatch means an out-of-band change happened; the restore-time
    // baseline is unrecoverable, so the reconciliation delta for the
    // restored views cannot be computed — install current storage as the
    // baseline and schedule a full view rebuild (next QueryBegin).
    if (it->second->is_deferred()) {
      int64_t live_count = 0;
      std::string live_hash;
      if (live_watermark(context, table_name, live_count, live_hash) &&
          live_count == it->second->deferred_weight() &&
          live_hash == it->second->deferred_hash()) {
        return DuckDBZSet(); // unchanged since restore: stay lazy
      }
      try {
        materialize_deferred_locked(context, table_name, nullptr, false);
      } catch (const std::exception &e) {
        last_error_ = std::string("Deferred baseline scan failed: ") + e.what();
        return std::nullopt;
      }
      rebuild_pending_ = true;
      record_error_best_effort(
          "DBSP: table '" + table_name +
          "' changed out-of-band after a lazy checkpoint restore; "
          "dependent views scheduled for full rebuild");
      return DuckDBZSet(); // views untouched here; rebuild reconciles
    }

    try {
      it->second->begin_rebuild();
      scan_syncs_++;

      stream_table_rows(context, table_name, [&](DuckDBRow &&row) {
        it->second->add_scanned_row(std::move(row));
      });

      // Diff against the previous baseline and swap the new one in
      // (spill mode: digest-index compare + on-disk payloads; RAM mode:
      // whole-map diff + move)
      return it->second->finish_rebuild();

    } catch (const std::exception &e) {
      last_error_ = std::string("Exception in sync_table_scan_and_consume: ") + e.what();
      if (std::getenv("DBSP_DEBUG_SYNC")) { std::cerr << "[dbsp] " << last_error_ << "\n"; }
      return std::nullopt;
    } catch (...) {
      last_error_ = "Unknown exception in sync_table_scan_and_consume";
      return std::nullopt;
    }
  }

  // ---- D3c deferred-baseline materialization ----------------------------
  //
  // Materialize a deferred table's baseline from a typed storage scan and
  // backfill any deferred shared arrangements over it.
  //
  // `pending` (optional) is a delta the caller is ABOUT to apply whose
  // rows are already committed to storage (notify/captured-delta contract:
  // the SQL write lands first). The installed baseline is scan − pending,
  // so the caller's subsequent apply lands the baseline exactly on the
  // committed content — and arrangements are backfilled from the pre-delta
  // state (propagation applies the delta to them afterwards, per the
  // post-delta arrangement convention in propagate_changes).
  //
  // Returns true when the scan matched the expected row weight
  // (restore-time watermark + pending). On mismatch the table changed
  // out-of-band: current storage is installed verbatim (no subtraction, no
  // arrangement backfill) and a full view rebuild is scheduled; the caller
  // must NOT apply/propagate its delta (the rebuild replays storage, which
  // already contains it).
  //
  // Locking: caller holds struct_mutex_ (shared or exclusive) and — unless
  // it holds struct_mutex_ exclusively — the table's lock exclusively.
  // view_lock_held says whether view_mutex_ is already held exclusively
  // (create_view); otherwise it is acquired for the backfill.
  bool materialize_deferred_locked(duckdb::ClientContext &context,
                                   const std::string &table_name,
                                   const DuckDBZSet *pending,
                                   bool view_lock_held) {
    auto it = tracked_tables_.find(table_name);
    if (it == tracked_tables_.end() || !it->second->is_deferred()) {
      return true;
    }
    DbspScopeTimer timer("baseline_materialize", table_name);
    auto &tt = *it->second;

    int64_t expected = tt.deferred_weight();
    if (pending) {
      for (const auto &[row, w] : *pending) {
        expected += w;
      }
    }

    tt.begin_rebuild();
    const int64_t scanned = stream_table_rows(
        context, table_name, [&](DuckDBRow &&row) {
          tt.add_scanned_row(std::move(row));
        }); // throws on scan failure: baseline stays deferred
    const bool clean = (scanned == expected);

    tt.install_rebuild();
    deferred_tables_--;

    if (!clean) {
      rebuild_pending_ = true;
      record_error_best_effort(
          "DBSP: table '" + table_name +
          "' does not match its lazy-restore watermark (expected weight " +
          std::to_string(expected) + ", scanned " + std::to_string(scanned) +
          "); dependent views scheduled for full rebuild");
      return false;
    }

    if (pending) {
      DuckDBZSet neg;
      for (const auto &[row, w] : *pending) {
        neg.insert(row, -w);
      }
      tt.apply_delta(neg);
    }

    backfill_deferred_arrangements_locked(table_name, view_lock_held);
    return true;
  }

  // Backfill shared arrangements flagged needs_backfill from the (now
  // materialized) baseline. Caller holds struct_mutex_; acquires
  // view_mutex_ exclusively unless view_lock_held.
  void backfill_deferred_arrangements_locked(const std::string &table_name,
                                             bool view_lock_held) {
    auto arr_it = arrangements_by_table_.find(table_name);
    if (arr_it == arrangements_by_table_.end()) {
      return;
    }
    std::unique_lock<std::shared_mutex> view_lock(view_mutex_,
                                                  std::defer_lock);
    if (!view_lock_held) {
      view_lock.lock();
    }
    auto tt_it = tracked_tables_.find(table_name);
    if (tt_it == tracked_tables_.end()) {
      return;
    }
    for (auto &weak : arr_it->second) {
      auto arr = weak.lock();
      if (!arr || !arr->needs_backfill) {
        continue;
      }
      DbspScopeTimer timer("arr_backfill", table_name);
      DuckDBZSet chunk;
      tt_it->second->scan_state([&](const DuckDBRow &row, int64_t w) {
        chunk.insert(row, w);
        if (chunk.size() >= 65536) {
          arr->apply(chunk);
          chunk = DuckDBZSet();
        }
      });
      if (!chunk.empty()) {
        arr->apply(chunk);
      }
      arr->needs_backfill = false;
    }
  }

  // Materialize every deferred table before a delta on `edited_table` is
  // applied and propagated: join nodes read arrangements (and dependent
  // circuits read state) of OTHER tables, so all of them must be real
  // before the first propagation. Takes each table's lock one at a time
  // (never nested) so concurrent callers cannot deadlock on sibling table
  // locks. Caller holds struct_mutex_ shared and NO table locks.
  //
  // Returns false when the edited table's watermark guard failed — the
  // caller must skip its delta (a full rebuild is scheduled).
  bool materialize_for_delta(duckdb::ClientContext &context,
                             const std::string &edited_table,
                             const DuckDBZSet *pending) {
    for (const auto &[name, tt] : tracked_tables_) {
      if (name == edited_table) {
        continue;
      }
      auto lock_it = table_locks_.find(name);
      if (lock_it == table_locks_.end()) {
        continue;
      }
      std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
      if (tt->is_deferred()) {
        materialize_deferred_locked(context, name, nullptr, false);
      }
    }
    auto lock_it = table_locks_.find(edited_table);
    if (lock_it == table_locks_.end()) {
      return false;
    }
    std::unique_lock<std::shared_mutex> table_lock(*lock_it->second);
    auto it = tracked_tables_.find(edited_table);
    if (it == tracked_tables_.end()) {
      return false;
    }
    if (!it->second->is_deferred()) {
      return true;
    }
    return materialize_deferred_locked(context, edited_table, pending, false);
  }

  // D3c safety net: a scan-diff delta is about to propagate; join nodes
  // may read arrangements over OTHER tables, so nothing may stay deferred.
  // (`edited` itself is never deferred here — the deferred branch of
  // sync_table_scan_and_consume returns an empty delta.) Caller holds
  // struct_mutex_ shared and no table locks.
  void ensure_no_deferred_before_propagate(duckdb::ClientContext &context,
                                           const std::string &edited) {
    if (!has_deferred()) {
      return;
    }
    try {
      materialize_for_delta(context, edited, nullptr);
    } catch (const std::exception &e) {
      rebuild_pending_ = true;
      std::cerr << "DBSP: deferred baseline materialization failed: "
                << e.what() << "\n";
    }
  }

  // Guarded entry for the notify/captured-delta paths: caller holds
  // struct_mutex_ shared and no table locks. Returns true when the delta
  // may be applied and propagated. On any failure a full view rebuild is
  // scheduled and the delta must be dropped (the rebuild replays committed
  // storage, which already contains it per the notify contract).
  bool prepare_deferred_for_delta(duckdb::ClientContext *context,
                                  const std::string &table_name,
                                  const DuckDBZSet &pending) {
    if (!has_deferred()) {
      return true;
    }
    if (!context) {
      rebuild_pending_ = true;
      std::cerr << "DBSP: delta on '" << table_name
                << "' while baselines are deferred and no context is "
                   "available; scheduling full view rebuild\n";
      return false;
    }
    try {
      return materialize_for_delta(*context, table_name, &pending);
    } catch (const std::exception &e) {
      rebuild_pending_ = true;
      std::cerr << "DBSP: deferred baseline materialization failed: "
                << e.what() << "\n";
      return false;
    }
  }

  bool track_table_internal(duckdb::ClientContext &context,
                            const std::string &table_ref) {
    // Called with struct_mutex_ exclusively held.
    // Sources arrive canonical from the plan translator; legacy persisted
    // definitions may carry bare names — resolve either to the canonical
    // key (D2).
    auto entry = resolve_table_entry(context, table_ref);
    if (!entry) {
      return false;
    }
    const std::string table_name = canonical_table_key(*entry);
    if (tracked_tables_.count(table_name)) {
      return true;
    }

    TableSchema schema;
    if (!get_table_schema(context, table_name, schema)) {
      return false;
    }

    tracked_tables_[table_name] =
        std::make_unique<TrackedTable>(table_name, schema);
    if (spill_enabled_) {
      tracked_tables_[table_name]->enable_spill(spill_path(table_name));
    }
    table_schemas_[table_name] = schema;
    table_locks_[table_name] = std::make_unique<std::shared_mutex>();

    // D3c lazy restore: during a checkpoint fast-path load the source's
    // save-time watermark was just verified against live storage, so the
    // baseline is by definition the committed table content — defer the
    // scan until something actually needs table state.
    {
      std::lock_guard<std::mutex> g(seeds_mutex_);
      auto seed = deferred_seeds_.find(table_name);
      if (seed != deferred_seeds_.end()) {
        tracked_tables_[table_name]->mark_deferred(seed->second.first,
                                                   seed->second.second);
        deferred_tables_++;
        return true;
      }
    }

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
      // Resolve the tracked key through the catalog (attached dbs too; D2)
      auto table_entry_ptr = resolve_table_entry(context, table_name);
      if (!table_entry_ptr) {
        return false;
      }

      auto &table_entry = *table_entry_ptr;
      auto &data_table = table_entry.GetStorage();

      // Transaction of the table's own catalog, not main's
      auto &transaction =
          duckdb::DuckTransaction::Get(context, table_entry.ParentCatalog());
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
      // Resolve through the catalog (works for attached databases and for
      // bare or dotted references; D2). Catalog API, not SQL — avoids
      // deadlock with in-flight queries.
      auto table_entry_ptr = resolve_table_entry(context, table_name);
      if (!table_entry_ptr) {
        last_error_ = "Table not found: " + table_name;
        return false;
      }

      auto &table_entry = *table_entry_ptr;

      schema.table_name = canonical_table_key(table_entry);
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
    // Every baseline mutation lands here; write-capture commit guards
    // compare against this to detect interleaved commits (see
    // docs/DESIGN_WRITE_CAPTURE.md).
    commit_seq_++;

    // Auto-persist checkpoint interval (Feature 1): count commits here
    // (cheap, lock-free) but defer the actual save_checkpoint() call to
    // maybe_save_checkpoint() at a point where struct_mutex_ is not held —
    // this function runs under callers' struct_mutex_ (on_insert/on_delete/
    // sync_tables hold it for their whole body), and save_checkpoint takes
    // its own shared lock on the same mutex.
    size_t interval = autopersist_interval_.load();
    if (interval > 0 &&
        commits_since_checkpoint_.fetch_add(1) + 1 >= interval) {
      commits_since_checkpoint_ = 0;
      checkpoint_due_ = true;
    }

    // Acquire view_mutex_ exclusively for writing view content.
    // Lock ordering: struct_mutex_ (held by caller) → view_mutex_ (acquired here).
    std::unique_lock<std::shared_mutex> view_lock(view_mutex_);

    // Pending deltas by source name. Values borrow a view's
    // get_batch_delta() — valid until that view's next apply, and each
    // view applies at most once per pass (topological order guarantees
    // every dependent reads first).
    std::unordered_map<std::string, const DuckDBZSet *> pending;
    pending[source_name] = &delta;

    // Shared arrangements are updated BEFORE any consuming view steps —
    // join nodes rely on the arrangement being post-delta and drop their
    // Δl⋈Δr term to compensate (Δl⋈R_new = Δl⋈R_old + Δl⋈Δr)
    {
      DbspScopeTimer t_arr("arrangements", source_name);
      apply_to_arrangements(source_name, delta);
    }

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

    // D-lazy (Global Constraint): a delta arriving for a pending view, or
    // any of its pending ancestors, must realize it FIRST -- deltas must
    // never apply to un-restored state. `topo` is exactly source_name's
    // transitive dependent set (the view about to receive this delta and
    // every pending ancestor of it, in dependency order), so a single
    // sequential sweep here covers the whole "or any of its pending
    // ancestors" clause in one pass. Done here, BEFORE the per-level loop
    // below (which may run step_view on separate threads when
    // use_parallel_sync_ is on), rather than inside step_view itself:
    // realize_pending_view_locked mutates pending_restore_ (a single map
    // shared by every view, not a per-view lock like table_locks_), and
    // the parallel-thread section below takes no lock of its own — it
    // relies on apply_changes_batch touching only each view's own
    // disjoint state. Sequencing every realize before any thread spawns
    // keeps that assumption true instead of adding a new lock level.
    for (const auto &view_name : topo) {
      if (views_.count(view_name)) {
        realize_pending_view_locked(view_name);
      }
    }

    struct StepResult {
      std::string view_name;
      const DuckDBZSet *delta = nullptr; // borrows view state
      size_t applied = 0;
    };
    auto step_view = [&](const std::string &view_name) -> StepResult {
      StepResult r;
      r.view_name = view_name;
      NativeMaterializedView &view = *views_.at(view_name);
      // All of this pass's source deltas go to the view in ONE batched
      // apply (one circuit step). Per-source sequential applies are wrong
      // for a join whose both sides updated this pass: its −Δl⋈Δr
      // both-shared correction only fires when the deltas share a step.
      std::vector<std::pair<std::string, const DuckDBZSet *>> inputs;
      for (const auto &src : view.source_tables()) {
        auto p = pending.find(src);
        if (p == pending.end() || p->second->empty())
          continue;
        inputs.emplace_back(src, p->second);
      }
      if (inputs.empty())
        return r;
      DbspScopeTimer t_step("view_step", view_name);
      view.apply_changes_batch(inputs);
      r.applied = inputs.size();
      r.delta = &view.get_batch_delta();
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
        if (r.applied == 0 || !r.delta || r.delta->empty())
          continue;
        pending[r.view_name] = r.delta;
        apply_to_arrangements(r.view_name, *r.delta);
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
  // D-lazy: checkpoint blobs for views the load fast path cold-created but
  // has not yet decoded (NativeMaterializedView::is_pending_restore()).
  // Tier 3 (view_mutex_) -- see stash_pending_view/realize_pending_view.
  std::unordered_map<std::string, PendingViewCkpt> pending_restore_;
  std::unordered_map<std::string, ViewDefinition> view_definitions_;
  DependencyGraph dep_graph_;
  std::string last_error_;
  // ON by default: a materialized view keeps itself current. Turn off
  // for bulk loads (each autocommit write pays a scoped scan-and-diff).
  std::atomic<bool> auto_sync_enabled_{true};
  // Auto-persist (Feature 1). autoload_attempted_ is atomic (not a plain
  // bool): multiple connections to the same DatabaseInstance can race
  // their first DBSP call, and maybe_autoload's compare_exchange on this
  // flag is what makes exactly one of them the auto-load attempt.
  std::atomic<bool> autopersist_{true};
  std::atomic<bool> autoload_attempted_{false};
  std::atomic<size_t> autopersist_interval_{0}; // commits; 0 = off
  std::atomic<size_t> commits_since_checkpoint_{0};
  std::atomic<bool> checkpoint_due_{false};
  size_t last_loaded_count_ = 0;
  size_t last_ckpt_restored_count_ = 0;
  size_t last_ckpt_saved_count_ = 0;
  // Rows in the last load_from_duck_table call whose view name was already
  // live in this session's registry (skipped, not (re)created) -- lets a
  // caller distinguish "this call restored 0 from checkpoint because the
  // checkpoint had nothing" from "this call restored 0 because everything
  // was already loaded by an earlier call".
  size_t last_skipped_count_ = 0;
  std::atomic<uint64_t> captured_delta_syncs_{0};
  std::atomic<uint64_t> scan_syncs_{0};
  // Write-capture (UPDATE/DELETE) observability + conflict detection:
  // commit_seq_ advances on every propagated baseline mutation and on
  // full rebuilds; guard failures fall back to scan-and-diff, loudly.
  std::atomic<uint64_t> commit_seq_{0};
  std::atomic<uint64_t> capture_guard_fallbacks_{0};
  std::atomic<bool> write_capture_enabled_{true};
  // D3c lazy baselines: count of deferred tables (lock-free hot-path
  // check), pending full-rebuild flag (out-of-band change detected against
  // a deferred baseline; consumed by rebuild_all_views), and the
  // watermark seeds handed from load_from_duck_table to
  // track_table_internal during a checkpoint fast-path load.
  std::atomic<size_t> deferred_tables_{0};
  std::atomic<bool> rebuild_pending_{false};
  std::mutex seeds_mutex_;
  std::unordered_map<std::string, std::pair<int64_t, std::string>>
      deferred_seeds_;
  size_t last_deferred_count_ = 0;
  // D-lazy: default ON (see enable_lazy_restore/disable_lazy_restore), and
  // views left pending by the last load_from_duck_table call (mirrors
  // last_deferred_count_ above for tables).
  std::atomic<bool> lazy_restore_{true};
  size_t last_pending_restore_count_ = 0;
  bool use_parallel_sync_ = false;
  bool spill_enabled_ = false;
  std::string spill_dir_;
  uint64_t arrangement_file_seq_ = 0;

  std::string spill_path(const std::string &table_name) const {
    return spill_dir_ + "/" + table_name + ".dbspill";
  }
};

// Per-instance CDC managers (Phase D1).
//
// One CDCManager per DatabaseInstance, created on first use and taken out of
// the registry by the extension's last-user-connection teardown, which
// destroys it on a detached thread (destroying views destroys their internal
// Connections, which re-enter ConnectionManager::RemoveConnection — doing
// that inline in OnConnectionClosed would deadlock on connections_lock).
//
// The registry itself is a deliberately leaked heap singleton (never
// destroyed): a function-local static would destroy surviving managers —
// and with them planner-frontend views holding internal Connections — during
// static teardown at process exit, running DuckDB shutdown at
// static-destruction time (historically: intermittent exit segfaults). With
// the leak, no destructors run at exit and the OS reclaims everything.
class CDCManagerRegistry {
public:
  CDCManager &get_or_create(duckdb::DatabaseInstance &db) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &slot = managers_[&db];
    if (!slot) {
      slot = std::make_unique<CDCManager>();
    }
    return *slot;
  }

  // Manager for db, or nullptr when none exists. The pointer stays valid
  // until take() removes it (managers are only destroyed via take()).
  CDCManager *find(duckdb::DatabaseInstance *db) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = managers_.find(db);
    return it == managers_.end() ? nullptr : it->second.get();
  }

  // Detach db's manager so the caller can destroy it off the
  // RemoveConnection path. Atomic single-flight: a concurrent second call
  // gets nullptr.
  std::unique_ptr<CDCManager> take(duckdb::DatabaseInstance *db) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = managers_.find(db);
    if (it == managers_.end()) {
      return nullptr;
    }
    auto manager = std::move(it->second);
    managers_.erase(it);
    return manager;
  }

private:
  std::mutex mutex_;
  std::unordered_map<duckdb::DatabaseInstance *,
                     std::unique_ptr<CDCManager>>
      managers_;
};

inline CDCManagerRegistry &get_cdc_registry() {
  static CDCManagerRegistry *registry = new CDCManagerRegistry();
  return *registry;
}

inline CDCManager &get_cdc_manager(duckdb::DatabaseInstance &db) {
  return get_cdc_registry().get_or_create(db);
}

inline CDCManager &get_cdc_manager(duckdb::ClientContext &context) {
  return get_cdc_manager(duckdb::DatabaseInstance::GetDatabase(context));
}

} // namespace dbsp_native

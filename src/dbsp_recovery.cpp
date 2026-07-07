#include "dbsp_recovery.hpp"
#include "dbsp_crash_marker.hpp"
#include "dbsp_cdc.hpp"
#include <filesystem>
#include <iostream>
#include <chrono>
#include <sstream>
#include <algorithm>

namespace dbsp_native {

// Global recovery manager instance
static std::unique_ptr<DBSPRecoveryManager> g_recovery_manager = nullptr;

DBSPRecoveryManager& get_recovery_manager() {
  if (!g_recovery_manager) {
    g_recovery_manager = std::make_unique<DBSPRecoveryManager>();
  }
  return *g_recovery_manager;
}

DBSPRecoveryManager::DBSPRecoveryManager(const std::string &recovery_path)
  : recovery_path_(recovery_path.empty() ? ".dbsp_recovery" : recovery_path),
    recovery_enabled_(true),
    session_started_(false) {
}

DBSPRecoveryManager::~DBSPRecoveryManager() {
  if (session_started_) {
    mark_session_end();
  }
}

std::string DBSPRecoveryManager::determine_recovery_path(const std::string &db_path) const {
  if (!recovery_path_.empty() && recovery_path_ != ".dbsp_recovery") {
    return recovery_path_;
  }

  // In-memory instances have no durable state: views die with the process,
  // so there is nothing crash markers could protect. Returning "" disables
  // markers entirely — the old fallback littered ./.dbsp_recovery into the
  // embedding process's CWD (e.g. an API server's repo root).
  if (db_path.empty() || db_path == "memory" || db_path == ":memory:") {
    return "";
  }

  std::filesystem::path db_file_path(db_path);
  if (db_file_path.has_parent_path()) {
    return db_file_path.parent_path().string() + "/.dbsp_recovery";
  }
  // Relative single-component db filename: markers next to it.
  return ".dbsp_recovery";
}

void DBSPRecoveryManager::mark_session_start() {
  if (!recovery_enabled_ || recovery_path_.empty()) return;

  DBSPCrashMarker::mark_session_start(recovery_path_);
  session_started_ = true;
}

void DBSPRecoveryManager::mark_session_end() {
  if (!recovery_enabled_ || recovery_path_.empty()) return;

  DBSPCrashMarker::mark_session_end(recovery_path_);
  session_started_ = false;
}

bool DBSPRecoveryManager::check_crash_markers() const {
  if (!recovery_enabled_ || recovery_path_.empty()) return false;

  return DBSPCrashMarker::detect_crash(recovery_path_);
}

void DBSPRecoveryManager::clear_crash_markers() {
  // Crash markers are already cleared by detect_crash()
  // This method exists for future enhancements
}

bool DBSPRecoveryManager::initialize_persistence(duckdb::ClientContext &context) {
  try {
    // Ensure recovery directory exists
    std::filesystem::path recovery_dir(recovery_path_);
    if (!std::filesystem::exists(recovery_dir)) {
      std::filesystem::create_directories(recovery_dir);
    }

    // Initialize _dbsp_views table via CDC manager
    auto &cdc_manager = get_cdc_manager(context);
    return cdc_manager.initialize_persistence_table(context);

  } catch (const std::exception &e) {
    std::cerr << "Failed to initialize persistence: " << e.what() << std::endl;
    return false;
  }
}

bool DBSPRecoveryManager::load_views(duckdb::ClientContext &context) {
  try {
    auto &cdc_manager = get_cdc_manager(context);

    // Query _dbsp_views table for all view definitions. Fresh connection:
    // `context` may be mid-query (recovery runs inside table functions).
    InternalQueryGuard guard;
    duckdb::Connection con(duckdb::DatabaseInstance::GetDatabase(context));
    auto result = con.Query(
      "SELECT name, sql, sources FROM _dbsp_views ORDER BY created_at ASC");

    if (result->HasError()) {
      std::cerr << "Failed to load views: " << result->GetError() << std::endl;
      return false;
    }

    // Convert to materialized result
    auto &materialized = result->Cast<duckdb::MaterializedQueryResult>();

    // Recreate each view
    size_t view_count = 0;
    for (size_t i = 0; i < materialized.RowCount(); i++) {
      auto name = materialized.GetValue(0, i).ToString();
      auto sql = materialized.GetValue(1, i).ToString();
      auto sources_str = materialized.GetValue(2, i).ToString();

      // Parse sources (comma-separated list)
      std::vector<std::string> sources;
      std::istringstream iss(sources_str);
      std::string source;
      while (std::getline(iss, source, ',')) {
        sources.push_back(source);
      }

      // Recreate view (re-plans the SQL and rebuilds the circuit by
      // replaying committed table state). Views already live in this
      // session (created before recovery's deferred first run) are kept
      // as-is — recreating them would fail and rebuilding is wasted work.
      if (cdc_manager.get_view(name)) {
        view_count++;
        continue;
      }
      try {
        if (cdc_manager.create_view(name, sql, sources, context)) {
          view_count++;
        } else {
          std::cerr << "Failed to recreate view '" << name
                    << "': " << cdc_manager.last_error() << std::endl;
        }
      } catch (const std::exception &e) {
        std::cerr << "Failed to recreate view '" << name << "': " << e.what() << std::endl;
        // Continue with other views
      }
    }

    return true;

  } catch (const std::exception &e) {
    std::cerr << "Failed to load views: " << e.what() << std::endl;
    return false;
  }
}

bool DBSPRecoveryManager::resync_tracked_tables(duckdb::ClientContext &context) {
  try {
    auto &cdc_manager = get_cdc_manager(context);

    // Get list of tracked tables
    auto tracked_tables = cdc_manager.list_tracked_tables();

    // Resync each table (full table scan)
    size_t table_count = 0;
    for (const auto &table_name : tracked_tables) {
      try {
        // Use existing sync mechanism (reads from DuckDB storage)
        cdc_manager.sync_table(context, table_name);
        table_count++;
      } catch (const std::exception &e) {
        std::cerr << "Failed to resync table '" << table_name << "': " << e.what() << std::endl;
        // Continue with other tables
      }
    }

    return true;

  } catch (const std::exception &e) {
    std::cerr << "Failed to resync tables: " << e.what() << std::endl;
    return false;
  }
}

bool DBSPRecoveryManager::rebuild_dependency_graph() {
  try {
    // Dependency graph is automatically rebuilt during view creation
    // in load_views(), so this method is a no-op for now

    // Future: Could verify dependency graph consistency here

    return true;

  } catch (const std::exception &e) {
    std::cerr << "Failed to rebuild dependency graph: " << e.what() << std::endl;
    return false;
  }
}

bool DBSPRecoveryManager::recover_from_crash(duckdb::ClientContext &context,
                                            const std::string &db_path) {
  if (!recovery_enabled_) {
    return true;  // Recovery disabled, nothing to do
  }

  // Determine final recovery path
  recovery_path_ = determine_recovery_path(db_path);
  if (std::getenv("DBSP_DEBUG_RECOVERY")) {
    std::cerr << "[dbsp] recovery db_path='" << db_path << "' recovery_path='"
              << recovery_path_ << "'" << std::endl;
  }

  // Step 1: Check for crash markers
  bool crashed = check_crash_markers();
  if (crashed) {
    std::cout << "DBSP Recovery: Previous session crashed, starting recovery..." << std::endl;
  }

  // Step 2: Initialize persistence infrastructure
  if (!initialize_persistence(context)) {
    std::cerr << "DBSP Recovery: Failed to initialize persistence" << std::endl;
    return false;
  }

  // Step 3: Load view definitions from _dbsp_views table
  if (!load_views(context)) {
    std::cerr << "DBSP Recovery: Failed to load views" << std::endl;
    // Continue anyway - some views may have loaded
  }

  // NOTE: there is deliberately no snapshot/WAL restore step. DuckDB's own
  // committed storage is the single durable source of truth: step 3 rebuilt
  // every view by replaying tracked-table scans of committed data through
  // its circuit, which restores internal node state (aggregate groups, join
  // indexes, sort/limit multisets, recursive dedup) that a sink-only
  // restore never could. The former checkpoint/WAL subsystem applied stale
  // Z-sets on top of that (or double-applied deltas) and was removed — see
  // the [restore_audit] tests for the failure modes it caused.

  // Step 4: Resync tracked tables against DuckDB storage. With baselines
  // fresh from step 3 the deltas are empty; this catches tables tracked in
  // _dbsp_tables that no view references (step 3 only tracks view sources).
  if (!resync_tracked_tables(context)) {
    std::cerr << "DBSP Recovery: Failed to resync tables" << std::endl;
    // Continue anyway - some tables may have synced
  }

  // Step 5: Rebuild dependency graph (mostly automatic)
  if (!rebuild_dependency_graph()) {
    std::cerr << "DBSP Recovery: Failed to rebuild dependency graph" << std::endl;
    return false;
  }

  // Step 6: Clear crash markers and mark new session start
  clear_crash_markers();
  mark_session_start();

  if (crashed) {
      } else {
      }

  return true;
}

} // namespace dbsp_native

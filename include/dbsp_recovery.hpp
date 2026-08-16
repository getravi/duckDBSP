#pragma once

#include "duckdb.hpp"
#include <string>
#include <mutex>
#include <map>
#include <vector>
#include <memory>

namespace dbsp_native {

// Forward declarations
class CDCManager;

/**
 * @brief DBSPRecoveryManager - Orchestrates crash recovery and persistence
 *
 * Responsibilities:
 * - Detect crashes via lock files
 * - Load view definitions from _dbsp_views table
 * - Recreate views from definitions
 * - Resync tracked tables from DuckDB storage
 * - Rebuild dependency graph
 */
class DBSPRecoveryManager {
public:
  /**
   * @brief Constructor
   * @param recovery_path Path to recovery directory (default: .dbsp_recovery)
   */
  explicit DBSPRecoveryManager(const std::string &recovery_path = "");

  /**
   * @brief Destructor - ensures clean shutdown
   */
  ~DBSPRecoveryManager();

  /**
   * @brief Main recovery entry point - call on extension initialization
   * @param context DuckDB client context
   * @param db_path Path to database file
   * @return true if recovery succeeded, false otherwise
   */
  bool recover_from_crash(duckdb::ClientContext &context,
                         const std::string &db_path = "");

  /**
   * @brief Initialize persistence infrastructure
   * @param context DuckDB client context
   * @return true if initialization succeeded
   */
  bool initialize_persistence(duckdb::ClientContext &context);

  /**
   * @brief Mark that session started (called on connection open)
   *
   * REFCOUNTED PER DATABASE. Several sessions can be open on one database
   * file, and the crash marker must survive until the LAST of them closes —
   * otherwise the first close drops the shared lock and every session still
   * running is unprotected: a crash after that point looks like a clean
   * exit, so the next open skips recovery.
   *
   * @param db  the DatabaseInstance this session belongs to, so the close
   *            path can find its recovery directory without re-deriving it
   *            from a half-torn-down context.
   */
  void mark_session_start(const void *db = nullptr);

  /**
   * @brief Mark that one session ended cleanly (called on connection close)
   *
   * Drops the lock only when this was the last session on that database.
   * With no argument, ends EVERY tracked session (process teardown).
   */
  void mark_session_end(const void *db = nullptr);

  /**
   * @brief Get recovery directory path
   * @return Recovery directory path
   */
  std::string get_recovery_path() const { return recovery_path_; }

  /**
   * @brief Check if recovery is enabled
   * @return true if recovery is enabled
   */
  bool is_recovery_enabled() const { return recovery_enabled_; }

  /**
   * @brief Enable/disable recovery
   * @param enabled Whether to enable recovery
   */
  void set_recovery_enabled(bool enabled) { recovery_enabled_ = enabled; }

private:
  /**
   * @brief Check if crash markers exist
   * @return true if crash detected
   */
  bool check_crash_markers() const;

  /**
   * @brief Load view definitions from _dbsp_views table
   * @param context DuckDB client context
   * @return true if load succeeded
   */
  bool load_views(duckdb::ClientContext &context);

  /**
   * @brief Resync all tracked tables from DuckDB storage
   * @param context DuckDB client context
   * @return true if resync succeeded
   */
  bool resync_tracked_tables(duckdb::ClientContext &context);

  /**
   * @brief Rebuild dependency graph from view definitions
   * @return true if rebuild succeeded
   */
  bool rebuild_dependency_graph();

  /**
   * @brief Clear crash markers
   */
  void clear_crash_markers();

  /**
   * @brief Determine recovery path based on database path
   * @param db_path Path to database file
   * @return Recovery directory path
   */
  std::string determine_recovery_path(const std::string &db_path) const;

  std::string recovery_path_;  ///< Path to recovery directory
  /// Live sessions per recovery directory. The lock file is written when a
  /// path's count reaches 1 and removed when it falls back to 0, so a
  /// database with two sessions open stays crash-protected until both go.
  std::map<std::string, int> session_counts_;
  /// DatabaseInstance -> its recovery directory, recorded at session start.
  /// The close path runs during teardown, where re-deriving the path from
  /// the context is unreliable.
  std::map<const void *, std::string> db_paths_;
  mutable std::mutex sessions_mutex_;
  bool recovery_enabled_;      ///< Whether recovery is enabled
  bool session_started_;       ///< Whether session has been marked as started
};

/**
 * @brief Get global recovery manager instance
 * @return Reference to recovery manager
 */
DBSPRecoveryManager& get_recovery_manager();

} // namespace dbsp_native

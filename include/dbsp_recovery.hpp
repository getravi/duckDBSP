#pragma once

#include "duckdb.hpp"
#include <string>
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
   */
  void mark_session_start();

  /**
   * @brief Mark that session ended cleanly (called on connection close)
   */
  void mark_session_end();

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

  /**
   * @brief Save checkpoint of current Z-set state
   * @param context DuckDB client context
   * @return true if checkpoint saved successfully
   */
  bool save_checkpoint(duckdb::ClientContext &context);

  /**
   * @brief Load Z-set state from latest checkpoint
   * @param context DuckDB client context
   * @return true if checkpoint loaded successfully
   */
  bool load_checkpoint(duckdb::ClientContext &context);

  /**
   * @brief Get the path to the latest checkpoint file
   * @return Path to checkpoint file, or empty string if none exists
   */
  std::string get_latest_checkpoint() const;

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

  /**
   * @brief Clean up old checkpoint files, keeping only the most recent N
   * @param keep_count Number of checkpoints to keep
   */
  void cleanup_old_checkpoints(size_t keep_count);

  std::string recovery_path_;  ///< Path to recovery directory
  bool recovery_enabled_;      ///< Whether recovery is enabled
  bool session_started_;       ///< Whether session has been marked as started
};

/**
 * @brief Get global recovery manager instance
 * @return Reference to recovery manager
 */
DBSPRecoveryManager& get_recovery_manager();

} // namespace dbsp_native

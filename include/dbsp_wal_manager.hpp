#pragma once

#include "duckdb.hpp"
#include "dbsp_duckdb_types.hpp"
#include <string>
#include <fstream>
#include <mutex>
#include <cstdint>

namespace dbsp_native {

/**
 * @brief WAL operation types
 */
enum class WALOperationType : uint8_t {
  TABLE_INSERT = 1,
  TABLE_DELETE = 2,
  TABLE_UPDATE = 3,
  VIEW_CREATE = 4,
  VIEW_DROP = 5,
  CHECKPOINT_MARKER = 6,
};

/**
 * @brief WAL entry structure
 */
struct WALEntry {
  uint64_t timestamp;              // Milliseconds since epoch
  WALOperationType operation_type;
  std::string table_name;
  DuckDBRow row_data;              // For INSERT/DELETE
  DuckDBRow old_row_data;          // For UPDATE
  std::string view_sql;            // For VIEW_CREATE
  uint64_t checkpoint_marker;      // For CHECKPOINT_MARKER

  void write(std::ostream &out) const;
  bool read(std::istream &in);
};

/**
 * @brief DBSP Write-Ahead Log Manager
 *
 * Logs all table and view operations for crash recovery.
 * Provides zero-data-loss guarantees when combined with checkpoints.
 */
class DBSPWALManager {
public:
  explicit DBSPWALManager(const std::string &wal_path = "");
  ~DBSPWALManager();

  /**
   * @brief Initialize WAL (create/open file)
   * @return true if successful
   */
  bool initialize();

  /**
   * @brief Log a table insert operation
   */
  bool log_insert(const std::string &table_name, const DuckDBRow &row);

  /**
   * @brief Log a table delete operation
   */
  bool log_delete(const std::string &table_name, const DuckDBRow &row);

  /**
   * @brief Log a table update operation
   */
  bool log_update(const std::string &table_name,
                  const DuckDBRow &old_row,
                  const DuckDBRow &new_row);

  /**
   * @brief Log a view create operation
   */
  bool log_view_create(const std::string &view_name, const std::string &sql);

  /**
   * @brief Log a view drop operation
   */
  bool log_view_drop(const std::string &view_name);

  /**
   * @brief Log a checkpoint marker (entries before this can be discarded)
   */
  bool log_checkpoint_marker(uint64_t checkpoint_timestamp);

  /**
   * @brief Flush WAL to disk
   */
  bool flush();

  /**
   * @brief Replay WAL entries after a given timestamp
   * @param context DuckDB client context
   * @param after_timestamp Only replay entries after this timestamp
   * @return true if replay successful
   */
  bool replay_wal(duckdb::ClientContext &context,
                  uint64_t after_timestamp = 0);

  /**
   * @brief Truncate WAL up to checkpoint marker
   * @param checkpoint_timestamp Remove entries before this timestamp
   */
  bool truncate_to_checkpoint(uint64_t checkpoint_timestamp);

  /**
   * @brief Get WAL file path
   */
  std::string get_wal_path() const { return wal_path_; }

  /**
   * @brief Check if WAL is enabled
   */
  bool is_enabled() const { return enabled_; }

  /**
   * @brief Enable/disable WAL
   */
  void set_enabled(bool enabled) { enabled_ = enabled; }

  /**
   * @brief Get last error message
   */
  const std::string &last_error() const { return last_error_; }

  /**
   * @brief Get current timestamp (for checkpoint coordination)
   */
  uint64_t get_current_timestamp() const;

private:
  bool write_entry(const WALEntry &entry);
  bool read_entry(WALEntry &entry);

  std::string wal_path_;
  std::fstream wal_file_;
  std::mutex wal_mutex_;
  bool enabled_;
  std::string last_error_;
  uint64_t last_checkpoint_pos_;
};

/**
 * @brief Get global WAL manager instance
 */
DBSPWALManager& get_wal_manager();

} // namespace dbsp_native

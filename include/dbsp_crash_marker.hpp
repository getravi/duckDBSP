#pragma once

#include <string>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace dbsp_native {

/**
 * @brief DBSPCrashMarker - Manages crash detection via lock files
 *
 * Creates a lock file on session start and removes it on clean shutdown.
 * If the lock file exists on startup, a crash is detected.
 */
class DBSPCrashMarker {
public:
  /**
   * @brief Create lock file to mark session start
   * @param path Recovery directory path
   */
  static void mark_session_start(const std::string &path);

  /**
   * @brief Remove lock file to mark clean session end
   * @param path Recovery directory path
   */
  static void mark_session_end(const std::string &path);

  /**
   * @brief Check if a crash occurred (lock file exists)
   * @param path Recovery directory path
   * @return true if crash detected, false otherwise
   */
  static bool detect_crash(const std::string &path);

  /**
   * @brief Get the session ID for this instance
   * @return Unique session ID
   */
  static std::string get_session_id();

  /**
   * @brief Write crash log entry
   * @param path Recovery directory path
   * @param message Crash message
   */
  static void log_crash(const std::string &path, const std::string &message);

private:
  /**
   * @brief Get the lock file path
   * @param path Recovery directory path
   * @return Full path to lock file
   */
  static std::string get_lock_path(const std::string &path);

  /**
   * @brief Get the crash log file path
   * @param path Recovery directory path
   * @return Full path to crash log file
   */
  static std::string get_crash_log_path(const std::string &path);

  /**
   * @brief Ensure recovery directory exists
   * @param path Recovery directory path
   */
  static void ensure_recovery_dir(const std::string &path);

  // Static session ID generated once per process
  static std::string session_id_;
};

} // namespace dbsp_native

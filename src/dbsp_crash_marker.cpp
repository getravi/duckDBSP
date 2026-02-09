#include "dbsp_crash_marker.hpp"
#include <sstream>
#include <iomanip>
#include <ctime>
#include <random>
#include <unistd.h>  // for getpid()

namespace dbsp_native {

// Initialize static session ID
std::string DBSPCrashMarker::session_id_ = "";

std::string DBSPCrashMarker::get_lock_path(const std::string &path) {
  return path + "/.dbsp.lock";
}

std::string DBSPCrashMarker::get_crash_log_path(const std::string &path) {
  return path + "/.dbsp.crash";
}

void DBSPCrashMarker::ensure_recovery_dir(const std::string &path) {
  std::filesystem::path dir_path(path);
  if (!std::filesystem::exists(dir_path)) {
    std::filesystem::create_directories(dir_path);
  }
}

std::string DBSPCrashMarker::get_session_id() {
  if (session_id_.empty()) {
    // Generate unique session ID: timestamp + random number
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(10000, 99999);
    int random_num = dis(gen);

    std::ostringstream oss;
    oss << timestamp << "_" << random_num;
    session_id_ = oss.str();
  }
  return session_id_;
}

void DBSPCrashMarker::mark_session_start(const std::string &path) {
  ensure_recovery_dir(path);

  std::string lock_path = get_lock_path(path);
  std::ofstream lock_file(lock_path);
  if (!lock_file) {
    throw std::runtime_error("Failed to create lock file: " + lock_path);
  }

  // Write session ID and timestamp
  auto now = std::chrono::system_clock::now();
  std::time_t now_time = std::chrono::system_clock::to_time_t(now);

  lock_file << "session_id: " << get_session_id() << "\n";
  lock_file << "start_time: " << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S") << "\n";
  lock_file << "pid: " << getpid() << "\n";
  lock_file.close();
}

void DBSPCrashMarker::mark_session_end(const std::string &path) {
  std::string lock_path = get_lock_path(path);

  // Remove lock file to indicate clean shutdown
  if (std::filesystem::exists(lock_path)) {
    std::filesystem::remove(lock_path);
  }
}

bool DBSPCrashMarker::detect_crash(const std::string &path) {
  std::string lock_path = get_lock_path(path);

  // If lock file exists, a previous session didn't exit cleanly
  if (std::filesystem::exists(lock_path)) {
    // Log the crash before cleaning up
    std::ifstream lock_file(lock_path);
    std::ostringstream oss;
    oss << lock_file.rdbuf();
    lock_file.close();

    log_crash(path, "Previous session crashed:\n" + oss.str());

    // Remove the stale lock file
    std::filesystem::remove(lock_path);

    return true;
  }

  return false;
}

void DBSPCrashMarker::log_crash(const std::string &path, const std::string &message) {
  ensure_recovery_dir(path);

  std::string crash_log_path = get_crash_log_path(path);
  std::ofstream crash_log(crash_log_path, std::ios::app);
  if (!crash_log) {
    // If we can't write to crash log, just continue
    return;
  }

  auto now = std::chrono::system_clock::now();
  std::time_t now_time = std::chrono::system_clock::to_time_t(now);

  crash_log << "=== Crash detected at "
            << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S")
            << " ===\n";
  crash_log << message << "\n\n";
  crash_log.close();
}

} // namespace dbsp_native

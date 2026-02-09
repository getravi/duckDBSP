#include "dbsp_wal_manager.hpp"
#include "dbsp_cdc.hpp"
#include <chrono>
#include <iostream>
#include <filesystem>

namespace dbsp_native {

// Global WAL manager instance
static std::unique_ptr<DBSPWALManager> g_wal_manager = nullptr;

DBSPWALManager& get_wal_manager() {
  if (!g_wal_manager) {
    g_wal_manager = std::make_unique<DBSPWALManager>();
  }
  return *g_wal_manager;
}

// WALEntry implementation
void WALEntry::write(std::ostream &out) const {
  // Write timestamp
  out.write(reinterpret_cast<const char *>(&timestamp), sizeof(timestamp));

  // Write operation type
  uint8_t op_type = static_cast<uint8_t>(operation_type);
  out.write(reinterpret_cast<const char *>(&op_type), sizeof(op_type));

  // Write table name (length-prefixed string)
  uint32_t name_len = static_cast<uint32_t>(table_name.size());
  out.write(reinterpret_cast<const char *>(&name_len), sizeof(name_len));
  out.write(table_name.data(), name_len);

  // Write operation-specific data
  switch (operation_type) {
    case WALOperationType::TABLE_INSERT:
    case WALOperationType::TABLE_DELETE: {
      // Write row data
      uint16_t row_size = static_cast<uint16_t>(row_data.columns.size());
      out.write(reinterpret_cast<const char *>(&row_size), sizeof(row_size));
      for (const auto &value : row_data.columns) {
        std::string str_val = value.ToString();
        uint32_t val_len = static_cast<uint32_t>(str_val.size());
        out.write(reinterpret_cast<const char *>(&val_len), sizeof(val_len));
        out.write(str_val.data(), val_len);
      }
      break;
    }

    case WALOperationType::TABLE_UPDATE: {
      // Write old row
      uint16_t old_row_size = static_cast<uint16_t>(old_row_data.columns.size());
      out.write(reinterpret_cast<const char *>(&old_row_size), sizeof(old_row_size));
      for (const auto &value : old_row_data.columns) {
        std::string str_val = value.ToString();
        uint32_t val_len = static_cast<uint32_t>(str_val.size());
        out.write(reinterpret_cast<const char *>(&val_len), sizeof(val_len));
        out.write(str_val.data(), val_len);
      }

      // Write new row
      uint16_t new_row_size = static_cast<uint16_t>(row_data.columns.size());
      out.write(reinterpret_cast<const char *>(&new_row_size), sizeof(new_row_size));
      for (const auto &value : row_data.columns) {
        std::string str_val = value.ToString();
        uint32_t val_len = static_cast<uint32_t>(str_val.size());
        out.write(reinterpret_cast<const char *>(&val_len), sizeof(val_len));
        out.write(str_val.data(), val_len);
      }
      break;
    }

    case WALOperationType::VIEW_CREATE: {
      // Write view SQL
      uint32_t sql_len = static_cast<uint32_t>(view_sql.size());
      out.write(reinterpret_cast<const char *>(&sql_len), sizeof(sql_len));
      out.write(view_sql.data(), sql_len);
      break;
    }

    case WALOperationType::VIEW_DROP:
      // No additional data
      break;

    case WALOperationType::CHECKPOINT_MARKER: {
      // Write checkpoint timestamp
      out.write(reinterpret_cast<const char *>(&checkpoint_marker), sizeof(checkpoint_marker));
      break;
    }
  }
}

bool WALEntry::read(std::istream &in) {
  // Read timestamp
  in.read(reinterpret_cast<char *>(&timestamp), sizeof(timestamp));
  if (!in.good()) return false;

  // Read operation type
  uint8_t op_type;
  in.read(reinterpret_cast<char *>(&op_type), sizeof(op_type));
  if (!in.good()) return false;
  operation_type = static_cast<WALOperationType>(op_type);

  // Read table name
  uint32_t name_len;
  in.read(reinterpret_cast<char *>(&name_len), sizeof(name_len));
  if (!in.good() || name_len > 1024) return false;

  table_name.resize(name_len);
  in.read(&table_name[0], name_len);
  if (!in.good()) return false;

  // Read operation-specific data
  switch (operation_type) {
    case WALOperationType::TABLE_INSERT:
    case WALOperationType::TABLE_DELETE: {
      uint16_t row_size;
      in.read(reinterpret_cast<char *>(&row_size), sizeof(row_size));
      if (!in.good() || row_size > 1000) return false;

      row_data.columns.clear();
      for (uint16_t i = 0; i < row_size; i++) {
        uint32_t val_len;
        in.read(reinterpret_cast<char *>(&val_len), sizeof(val_len));
        if (!in.good() || val_len > 1024 * 1024) return false;

        std::string str_val;
        str_val.resize(val_len);
        in.read(&str_val[0], val_len);
        if (!in.good()) return false;

        row_data.columns.push_back(duckdb::Value(str_val));
      }
      break;
    }

    case WALOperationType::TABLE_UPDATE: {
      // Read old row
      uint16_t old_row_size;
      in.read(reinterpret_cast<char *>(&old_row_size), sizeof(old_row_size));
      if (!in.good() || old_row_size > 1000) return false;

      old_row_data.columns.clear();
      for (uint16_t i = 0; i < old_row_size; i++) {
        uint32_t val_len;
        in.read(reinterpret_cast<char *>(&val_len), sizeof(val_len));
        if (!in.good() || val_len > 1024 * 1024) return false;

        std::string str_val;
        str_val.resize(val_len);
        in.read(&str_val[0], val_len);
        if (!in.good()) return false;

        old_row_data.columns.push_back(duckdb::Value(str_val));
      }

      // Read new row
      uint16_t new_row_size;
      in.read(reinterpret_cast<char *>(&new_row_size), sizeof(new_row_size));
      if (!in.good() || new_row_size > 1000) return false;

      row_data.columns.clear();
      for (uint16_t i = 0; i < new_row_size; i++) {
        uint32_t val_len;
        in.read(reinterpret_cast<char *>(&val_len), sizeof(val_len));
        if (!in.good() || val_len > 1024 * 1024) return false;

        std::string str_val;
        str_val.resize(val_len);
        in.read(&str_val[0], val_len);
        if (!in.good()) return false;

        row_data.columns.push_back(duckdb::Value(str_val));
      }
      break;
    }

    case WALOperationType::VIEW_CREATE: {
      uint32_t sql_len;
      in.read(reinterpret_cast<char *>(&sql_len), sizeof(sql_len));
      if (!in.good() || sql_len > 1024 * 1024) return false;

      view_sql.resize(sql_len);
      in.read(&view_sql[0], sql_len);
      if (!in.good()) return false;
      break;
    }

    case WALOperationType::VIEW_DROP:
      // No additional data
      break;

    case WALOperationType::CHECKPOINT_MARKER: {
      in.read(reinterpret_cast<char *>(&checkpoint_marker), sizeof(checkpoint_marker));
      if (!in.good()) return false;
      break;
    }

    default:
      return false;  // Unknown operation type
  }

  return true;
}

// DBSPWALManager implementation
DBSPWALManager::DBSPWALManager(const std::string &wal_path)
  : wal_path_(wal_path.empty() ? ".dbsp_recovery/wal.dbsp" : wal_path),
    enabled_(true),
    last_checkpoint_pos_(0) {
}

DBSPWALManager::~DBSPWALManager() {
  if (wal_file_.is_open()) {
    flush();
    wal_file_.close();
  }
}

bool DBSPWALManager::initialize() {
  if (!enabled_) return true;

  std::lock_guard<std::mutex> lock(wal_mutex_);

  // Close any existing handle first to get accurate file existence check
  if (wal_file_.is_open()) {
    wal_file_.close();
  }

  // If file already exists, just open it
  if (std::filesystem::exists(wal_path_)) {
    wal_file_.open(wal_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    return wal_file_.is_open();
  }

  // Create parent directory if needed
  std::filesystem::path wal_file_path(wal_path_);
  if (wal_file_path.has_parent_path()) {
    std::filesystem::create_directories(wal_file_path.parent_path());
  }

  // Open WAL file in append mode
  wal_file_.open(wal_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);

  if (!wal_file_) {
    // File doesn't exist, create it
    wal_file_.open(wal_path_, std::ios::out | std::ios::binary);
    if (!wal_file_) {
      last_error_ = "Failed to create WAL file: " + wal_path_;
      return false;
    }
    wal_file_.close();

    // Reopen in read/write mode
    wal_file_.open(wal_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);
    if (!wal_file_) {
      last_error_ = "Failed to reopen WAL file: " + wal_path_;
      return false;
    }
  }

  return true;
}

uint64_t DBSPWALManager::get_current_timestamp() const {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
}

bool DBSPWALManager::write_entry(const WALEntry &entry) {
  if (!enabled_ || !wal_file_.is_open()) return false;

  std::lock_guard<std::mutex> lock(wal_mutex_);

  try {
    entry.write(wal_file_);
    return wal_file_.good();
  } catch (const std::exception &e) {
    last_error_ = std::string("Failed to write WAL entry: ") + e.what();
    return false;
  }
}

bool DBSPWALManager::log_insert(const std::string &table_name, const DuckDBRow &row) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::TABLE_INSERT;
  entry.table_name = table_name;
  entry.row_data = row;

  return write_entry(entry);
}

bool DBSPWALManager::log_delete(const std::string &table_name, const DuckDBRow &row) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::TABLE_DELETE;
  entry.table_name = table_name;
  entry.row_data = row;

  return write_entry(entry);
}

bool DBSPWALManager::log_update(const std::string &table_name,
                                const DuckDBRow &old_row,
                                const DuckDBRow &new_row) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::TABLE_UPDATE;
  entry.table_name = table_name;
  entry.old_row_data = old_row;
  entry.row_data = new_row;

  return write_entry(entry);
}

bool DBSPWALManager::log_view_create(const std::string &view_name, const std::string &sql) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::VIEW_CREATE;
  entry.table_name = view_name;
  entry.view_sql = sql;

  return write_entry(entry);
}

bool DBSPWALManager::log_view_drop(const std::string &view_name) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::VIEW_DROP;
  entry.table_name = view_name;

  return write_entry(entry);
}

bool DBSPWALManager::log_checkpoint_marker(uint64_t checkpoint_timestamp) {
  WALEntry entry;
  entry.timestamp = get_current_timestamp();
  entry.operation_type = WALOperationType::CHECKPOINT_MARKER;
  entry.checkpoint_marker = checkpoint_timestamp;

  bool result = write_entry(entry);
  if (result) {
    last_checkpoint_pos_ = wal_file_.tellp();
  }

  return result;
}

bool DBSPWALManager::flush() {
  if (!enabled_ || !wal_file_.is_open()) return true;

  std::lock_guard<std::mutex> lock(wal_mutex_);

  wal_file_.flush();
  return wal_file_.good();
}

bool DBSPWALManager::replay_wal(duckdb::ClientContext &context, uint64_t after_timestamp) {
  if (!enabled_) return true;

  std::lock_guard<std::mutex> lock(wal_mutex_);

  if (!wal_file_.is_open()) {
    last_error_ = "WAL file not open";
    return false;
  }

  // Save current position
  auto current_pos = wal_file_.tellg();

  // Seek to beginning
  wal_file_.seekg(0, std::ios::beg);

  auto &cdc_manager = get_cdc_manager();
  size_t entries_replayed = 0;

  std::cout << "DBSP WAL: Replaying entries after timestamp " << after_timestamp << std::endl;

  try {
    while (wal_file_.good() && !wal_file_.eof()) {
      WALEntry entry;
      if (!entry.read(wal_file_)) {
        if (wal_file_.eof()) break;
        last_error_ = "Failed to read WAL entry";
        wal_file_.seekg(current_pos);
        return false;
      }

      // Skip entries before threshold
      if (entry.timestamp <= after_timestamp) {
        continue;
      }

      // Replay entry
      switch (entry.operation_type) {
        case WALOperationType::TABLE_INSERT:
          cdc_manager.on_insert(entry.table_name, entry.row_data);
          entries_replayed++;
          break;

        case WALOperationType::TABLE_DELETE:
          cdc_manager.on_delete(entry.table_name, entry.row_data);
          entries_replayed++;
          break;

        case WALOperationType::TABLE_UPDATE:
          cdc_manager.on_update(entry.table_name, entry.old_row_data, entry.row_data);
          entries_replayed++;
          break;

        case WALOperationType::VIEW_CREATE:
          cdc_manager.create_view(context, entry.table_name, entry.view_sql);
          entries_replayed++;
          break;

        case WALOperationType::VIEW_DROP:
          cdc_manager.drop_view(entry.table_name);
          entries_replayed++;
          break;

        case WALOperationType::CHECKPOINT_MARKER:
          // Update threshold for future truncation
          after_timestamp = entry.checkpoint_marker;
          break;
      }
    }

    std::cout << "DBSP WAL: Replayed " << entries_replayed << " entries" << std::endl;

    // Restore position
    wal_file_.clear();  // Clear EOF flag
    wal_file_.seekg(current_pos);

    return true;

  } catch (const std::exception &e) {
    last_error_ = std::string("Exception during WAL replay: ") + e.what();
    wal_file_.seekg(current_pos);
    return false;
  }
}

bool DBSPWALManager::truncate_to_checkpoint(uint64_t checkpoint_timestamp) {
  if (!enabled_ || !wal_file_.is_open()) return true;

  std::lock_guard<std::mutex> lock(wal_mutex_);

  // Create new temp WAL file
  std::string temp_wal_path = wal_path_ + ".tmp";
  std::ofstream new_wal(temp_wal_path, std::ios::binary);

  if (!new_wal) {
    last_error_ = "Failed to create temp WAL file";
    return false;
  }

  // Seek to beginning
  wal_file_.seekg(0, std::ios::beg);

  // Copy entries after checkpoint timestamp
  try {
    while (wal_file_.good() && !wal_file_.eof()) {
      WALEntry entry;
      if (!entry.read(wal_file_)) {
        if (wal_file_.eof()) break;
        last_error_ = "Failed to read WAL entry during truncation";
        return false;
      }

      // Keep entries after checkpoint
      if (entry.timestamp > checkpoint_timestamp) {
        entry.write(new_wal);
      }
    }

    new_wal.close();
    wal_file_.close();

    // Replace old WAL with new one
    std::filesystem::remove(wal_path_);
    std::filesystem::rename(temp_wal_path, wal_path_);

    // Reopen WAL
    wal_file_.open(wal_path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::app);

    std::cout << "DBSP WAL: Truncated entries before timestamp " << checkpoint_timestamp << std::endl;

    return wal_file_.is_open();

  } catch (const std::exception &e) {
    last_error_ = std::string("Exception during WAL truncation: ") + e.what();
    return false;
  }
}

} // namespace dbsp_native

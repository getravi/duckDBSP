#include "dbsp_checkpoint_format.hpp"
#include <chrono>
#include <cstring>

namespace dbsp_native {

// CRC64 polynomial (ECMA-182)
constexpr uint64_t CRC64_POLY = 0xC96C5795D7870F42ULL;

// CRC64 lookup table
static uint64_t crc64_table[256];
static bool crc64_table_initialized = false;

static void init_crc64_table() {
  if (crc64_table_initialized) return;

  for (int i = 0; i < 256; i++) {
    uint64_t crc = i;
    for (int j = 0; j < 8; j++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ CRC64_POLY;
      } else {
        crc >>= 1;
      }
    }
    crc64_table[i] = crc;
  }
  crc64_table_initialized = true;
}

uint64_t compute_crc64(const void *data, size_t size, uint64_t prev) {
  init_crc64_table();

  uint64_t crc = prev;
  const uint8_t *ptr = static_cast<const uint8_t *>(data);

  for (size_t i = 0; i < size; i++) {
    crc = crc64_table[(crc ^ ptr[i]) & 0xFF] ^ (crc >> 8);
  }

  return crc;
}

// CheckpointHeader implementation
void CheckpointHeader::write(std::ostream &out) const {
  out.write(reinterpret_cast<const char *>(&version), sizeof(version));
  out.write(reinterpret_cast<const char *>(&timestamp), sizeof(timestamp));
  out.write(reinterpret_cast<const char *>(&num_views), sizeof(num_views));
  out.write(reinterpret_cast<const char *>(&num_tables), sizeof(num_tables));
  out.write(reinterpret_cast<const char *>(&flags), sizeof(flags));
  out.write(reinterpret_cast<const char *>(reserved), sizeof(reserved));
}

bool CheckpointHeader::read(std::istream &in) {
  in.read(reinterpret_cast<char *>(&version), sizeof(version));
  in.read(reinterpret_cast<char *>(&timestamp), sizeof(timestamp));
  in.read(reinterpret_cast<char *>(&num_views), sizeof(num_views));
  in.read(reinterpret_cast<char *>(&num_tables), sizeof(num_tables));
  in.read(reinterpret_cast<char *>(&flags), sizeof(flags));
  in.read(reinterpret_cast<char *>(reserved), sizeof(reserved));

  return in.good() && version == CHECKPOINT_VERSION;
}

// CheckpointWriter implementation
CheckpointWriter::CheckpointWriter(const std::string &path)
  : path_(path) {
  file_.open(path, std::ios::binary | std::ios::trunc);
  if (!file_) {
    last_error_ = "Failed to open checkpoint file for writing: " + path;
  }
}

CheckpointWriter::~CheckpointWriter() {
  if (file_.is_open()) {
    file_.close();
  }
}

bool CheckpointWriter::write_header(const CheckpointHeader &header) {
  if (!file_) {
    last_error_ = "File not open";
    return false;
  }

  header.write(file_);
  update_checksum(&header, sizeof(header));
  return file_.good();
}

bool CheckpointWriter::write_view(const std::string &name, const DuckDBZSet &zset) {
  if (!file_) return false;

  if (!write_string(name)) return false;
  if (!write_zset(zset)) return false;

  return true;
}

bool CheckpointWriter::write_table(const std::string &name, uint64_t sequence,
                                   const DuckDBZSet &zset) {
  if (!file_) return false;

  if (!write_string(name)) return false;
  if (!write_uint64(sequence)) return false;
  if (!write_zset(zset)) return false;

  return true;
}

bool CheckpointWriter::finalize() {
  if (!file_) return false;

  // Write checksum
  file_.write(reinterpret_cast<const char *>(&checksum_), sizeof(checksum_));
  file_.flush();

  return file_.good();
}

bool CheckpointWriter::write_string(const std::string &str) {
  uint32_t len = static_cast<uint32_t>(str.size());
  if (!write_uint32(len)) return false;

  file_.write(str.data(), len);
  update_checksum(str.data(), len);

  return file_.good();
}

bool CheckpointWriter::write_uint64(uint64_t value) {
  file_.write(reinterpret_cast<const char *>(&value), sizeof(value));
  update_checksum(&value, sizeof(value));
  return file_.good();
}

bool CheckpointWriter::write_uint32(uint32_t value) {
  file_.write(reinterpret_cast<const char *>(&value), sizeof(value));
  update_checksum(&value, sizeof(value));
  return file_.good();
}

bool CheckpointWriter::write_uint16(uint16_t value) {
  file_.write(reinterpret_cast<const char *>(&value), sizeof(value));
  update_checksum(&value, sizeof(value));
  return file_.good();
}

bool CheckpointWriter::write_uint8(uint8_t value) {
  file_.write(reinterpret_cast<const char *>(&value), sizeof(value));
  update_checksum(&value, sizeof(value));
  return file_.good();
}

bool CheckpointWriter::write_int64(int64_t value) {
  file_.write(reinterpret_cast<const char *>(&value), sizeof(value));
  update_checksum(&value, sizeof(value));
  return file_.good();
}

bool CheckpointWriter::write_zset(const DuckDBZSet &zset) {
  // Write number of rows
  uint64_t num_rows = zset.size();
  if (!write_uint64(num_rows)) return false;

  // Write each row
  for (const auto &[row, weight] : zset) {
    if (!write_row(row)) return false;
    if (!write_int64(weight)) return false;
  }

  return true;
}

bool CheckpointWriter::write_row(const DuckDBRow &row) {
  // Write number of columns
  uint16_t num_cols = static_cast<uint16_t>(row.columns.size());
  if (!write_uint16(num_cols)) return false;

  // Write each value
  for (const auto &value : row.columns) {
    if (!write_value(value)) return false;
  }

  return true;
}

bool CheckpointWriter::write_value(const duckdb::Value &value) {
  // Write type
  uint8_t type_id = static_cast<uint8_t>(value.type().id());
  if (!write_uint8(type_id)) return false;

  // Write value based on type
  if (value.IsNull()) {
    uint8_t null_flag = 1;
    return write_uint8(null_flag);
  }

  uint8_t null_flag = 0;
  if (!write_uint8(null_flag)) return false;

  // Serialize value as string (simple approach for Phase 2)
  std::string str_value = value.ToString();
  return write_string(str_value);
}

void CheckpointWriter::update_checksum(const void *data, size_t size) {
  checksum_ = compute_crc64(data, size, checksum_);
}

// CheckpointReader implementation
CheckpointReader::CheckpointReader(const std::string &path)
  : path_(path) {
  file_.open(path, std::ios::binary);
  if (!file_) {
    last_error_ = "Failed to open checkpoint file for reading: " + path;
  }
}

CheckpointReader::~CheckpointReader() {
  if (file_.is_open()) {
    file_.close();
  }
}

bool CheckpointReader::read_header(CheckpointHeader &header) {
  if (!file_) {
    last_error_ = "File not open";
    return false;
  }

  if (!header.read(file_)) {
    last_error_ = "Failed to read header or invalid version";
    return false;
  }

  update_checksum(&header, sizeof(header));
  return true;
}

bool CheckpointReader::read_view(std::string &name, DuckDBZSet &zset) {
  if (!file_) return false;

  if (!read_string(name)) return false;
  if (!read_zset(zset)) return false;

  return true;
}

bool CheckpointReader::read_table(std::string &name, uint64_t &sequence,
                                  DuckDBZSet &zset) {
  if (!file_) return false;

  if (!read_string(name)) return false;
  if (!read_uint64(sequence)) return false;
  if (!read_zset(zset)) return false;

  return true;
}

bool CheckpointReader::verify_checksum() {
  if (!file_) return false;

  // Read expected checksum
  file_.read(reinterpret_cast<char *>(&expected_checksum_),
             sizeof(expected_checksum_));

  if (!file_.good()) {
    last_error_ = "Failed to read checksum";
    return false;
  }

  if (checksum_ != expected_checksum_) {
    last_error_ = "Checksum mismatch - checkpoint may be corrupted";
    return false;
  }

  return true;
}

bool CheckpointReader::read_string(std::string &str) {
  uint32_t len;
  if (!read_uint32(len)) return false;

  if (len > 1024 * 1024) {  // Sanity check: max 1MB string
    last_error_ = "String too long";
    return false;
  }

  str.resize(len);
  file_.read(&str[0], len);
  update_checksum(str.data(), len);

  return file_.good();
}

bool CheckpointReader::read_uint64(uint64_t &value) {
  file_.read(reinterpret_cast<char *>(&value), sizeof(value));
  update_checksum(&value, sizeof(value));
  return file_.good();
}

bool CheckpointReader::read_uint32(uint32_t &value) {
  file_.read(reinterpret_cast<char *>(&value), sizeof(value));
  update_checksum(&value, sizeof(value));
  return file_.good();
}

bool CheckpointReader::read_uint16(uint16_t &value) {
  file_.read(reinterpret_cast<char *>(&value), sizeof(value));
  update_checksum(&value, sizeof(value));
  return file_.good();
}

bool CheckpointReader::read_uint8(uint8_t &value) {
  file_.read(reinterpret_cast<char *>(&value), sizeof(value));
  update_checksum(&value, sizeof(value));
  return file_.good();
}

bool CheckpointReader::read_int64(int64_t &value) {
  file_.read(reinterpret_cast<char *>(&value), sizeof(value));
  update_checksum(&value, sizeof(value));
  return file_.good();
}

bool CheckpointReader::read_zset(DuckDBZSet &zset) {
  // Read number of rows
  uint64_t num_rows;
  if (!read_uint64(num_rows)) return false;

  if (num_rows > 10000000) {  // Sanity check: max 10M rows
    last_error_ = "Too many rows in Z-set";
    return false;
  }

  zset.clear();

  // Read each row
  for (uint64_t i = 0; i < num_rows; i++) {
    DuckDBRow row;
    int64_t weight;

    if (!read_row(row)) return false;
    if (!read_int64(weight)) return false;

    zset.insert(row, weight);
  }

  return true;
}

bool CheckpointReader::read_row(DuckDBRow &row) {
  // Read number of columns
  uint16_t num_cols;
  if (!read_uint16(num_cols)) return false;

  if (num_cols > 1000) {  // Sanity check: max 1000 columns
    last_error_ = "Too many columns";
    return false;
  }

  row.columns.clear();
  row.columns.reserve(num_cols);

  // Read each value
  for (uint16_t i = 0; i < num_cols; i++) {
    duckdb::Value value;
    if (!read_value(value)) return false;
    row.columns.push_back(value);
  }

  return true;
}

bool CheckpointReader::read_value(duckdb::Value &value) {
  // Read type
  uint8_t type_id;
  if (!read_uint8(type_id)) return false;

  // Read null flag
  uint8_t null_flag;
  if (!read_uint8(null_flag)) return false;

  if (null_flag) {
    value = duckdb::Value(duckdb::LogicalType(static_cast<duckdb::LogicalTypeId>(type_id)));
    return true;
  }

  // Read string representation
  std::string str_value;
  if (!read_string(str_value)) return false;

  // Parse value based on type (simple approach for Phase 2)
  // For now, keep values as strings and let DuckDB handle conversion when used
  // TODO: Add ClientContext parameter to properly cast values during deserialization
  value = duckdb::Value(str_value);

  return true;
}

void CheckpointReader::update_checksum(const void *data, size_t size) {
  checksum_ = compute_crc64(data, size, checksum_);
}

} // namespace dbsp_native

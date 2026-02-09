#pragma once

#include "duckdb.hpp"
#include "dbsp_duckdb_types.hpp"
#include <vector>
#include <string>
#include <cstdint>
#include <fstream>

namespace dbsp_native {

/**
 * @brief Binary format for DBSP checkpoints
 *
 * Format:
 * - HEADER: Version, timestamp, counts, flags
 * - VIEWS: For each view, name + Z-set data
 * - TABLES: For each tracked table, name + sequence + Z-set data
 * - CHECKSUM: CRC64 of entire checkpoint
 */

// Checkpoint format version
constexpr uint32_t CHECKPOINT_VERSION = 1;

// Checkpoint flags
enum class CheckpointFlags : uint8_t {
  NONE = 0,
  COMPRESSED = 1 << 0,
  ENCRYPTED = 1 << 1,
};

/**
 * @brief Checkpoint header structure
 */
struct CheckpointHeader {
  uint32_t version = CHECKPOINT_VERSION;
  uint64_t timestamp = 0;  // Milliseconds since epoch
  uint32_t num_views = 0;
  uint32_t num_tables = 0;
  uint8_t flags = static_cast<uint8_t>(CheckpointFlags::NONE);
  uint8_t reserved[7] = {0};  // Padding to 32 bytes

  void write(std::ostream &out) const;
  bool read(std::istream &in);
};

/**
 * @brief Checkpoint writer - saves Z-sets to binary format
 */
class CheckpointWriter {
public:
  explicit CheckpointWriter(const std::string &path);
  ~CheckpointWriter();

  // Write header
  bool write_header(const CheckpointHeader &header);

  // Write a view's Z-set
  bool write_view(const std::string &name, const DuckDBZSet &zset);

  // Write a tracked table's Z-set
  bool write_table(const std::string &name, uint64_t sequence, const DuckDBZSet &zset);

  // Finalize checkpoint (write checksum)
  bool finalize();

  // Get error message
  const std::string &last_error() const { return last_error_; }

private:
  std::ofstream file_;
  std::string path_;
  std::string last_error_;
  uint64_t checksum_ = 0;

  bool write_string(const std::string &str);
  bool write_uint64(uint64_t value);
  bool write_uint32(uint32_t value);
  bool write_uint16(uint16_t value);
  bool write_uint8(uint8_t value);
  bool write_int64(int64_t value);
  bool write_zset(const DuckDBZSet &zset);
  bool write_row(const DuckDBRow &row);
  bool write_value(const duckdb::Value &value);

  void update_checksum(const void *data, size_t size);
};

/**
 * @brief Checkpoint reader - loads Z-sets from binary format
 */
class CheckpointReader {
public:
  explicit CheckpointReader(const std::string &path);
  ~CheckpointReader();

  // Read header
  bool read_header(CheckpointHeader &header);

  // Read a view's Z-set
  bool read_view(std::string &name, DuckDBZSet &zset);

  // Read a tracked table's Z-set
  bool read_table(std::string &name, uint64_t &sequence, DuckDBZSet &zset);

  // Verify checksum
  bool verify_checksum();

  // Get error message
  const std::string &last_error() const { return last_error_; }

  // Check if file is valid
  bool is_valid() const { return file_.is_open(); }

private:
  std::ifstream file_;
  std::string path_;
  std::string last_error_;
  uint64_t checksum_ = 0;
  uint64_t expected_checksum_ = 0;

  bool read_string(std::string &str);
  bool read_uint64(uint64_t &value);
  bool read_uint32(uint32_t &value);
  bool read_uint16(uint16_t &value);
  bool read_uint8(uint8_t &value);
  bool read_int64(int64_t &value);
  bool read_zset(DuckDBZSet &zset);
  bool read_row(DuckDBRow &row);
  bool read_value(duckdb::Value &value);

  void update_checksum(const void *data, size_t size);
};

/**
 * @brief Compute CRC64 checksum
 */
uint64_t compute_crc64(const void *data, size_t size, uint64_t prev = 0);

} // namespace dbsp_native

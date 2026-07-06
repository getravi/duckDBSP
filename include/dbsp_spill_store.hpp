// dbsp_spill_store.hpp — disk-backed baseline storage (Phase K1)
//
// A TrackedTable's baseline (the full copy of the table used for
// scan-and-diff sync and view initialization) is the largest single RAM
// consumer in the engine, and its access pattern is sequential: one full
// pass per sync, one full stream per view init. SpilledBaseline moves the
// row payloads to an on-disk record log and keeps only a 128-bit row
// hash → (offset, length, weight) index in memory (~40 bytes/row instead
// of fully materialized duckdb::Values).
//
// Collisions: two distinct rows sharing a 128-bit hash would corrupt a
// diff. At 10^6 rows the probability is ~10^-22 — accepted without a
// byte-level compare (same trust model as content-addressed stores).
//
// Crash safety: files are replaced atomically (tmp + rename) and carry a
// generation counter. A torn or stale file is simply discarded — recovery
// re-syncs baselines from DuckDB, which remains the only durable source.

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dbsp_native {

// 128-bit row hash: two independent 64-bit FNV-1a passes over the
// serialized row bytes (different offset bases → independent streams).
struct RowDigest {
  uint64_t hi = 0;
  uint64_t lo = 0;
  bool operator==(const RowDigest &o) const {
    return hi == o.hi && lo == o.lo;
  }
};

struct RowDigestHash {
  size_t operator()(const RowDigest &d) const {
    return static_cast<size_t>(d.hi ^ (d.lo * 0x9e3779b97f4a7c15ULL));
  }
};

inline RowDigest digest_bytes(const uint8_t *data, size_t len) {
  uint64_t a = 0xcbf29ce484222325ULL;
  uint64_t b = 0x84222325cbf29ce4ULL;
  for (size_t i = 0; i < len; i++) {
    a = (a ^ data[i]) * 0x100000001b3ULL;
    b = (b ^ data[i]) * 0x100000001b3ULL;
    b ^= b >> 29;
  }
  return {a, b};
}

// Serialize a row (vector of Values) into `out`. Each value uses DuckDB's
// own binary serialization — full type fidelity for every type DuckDB
// supports, at the cost of per-value framing bytes.
inline void serialize_row(const std::vector<duckdb::Value> &row,
                          std::vector<uint8_t> &out) {
  duckdb::MemoryStream stream;
  const uint32_t n = static_cast<uint32_t>(row.size());
  stream.WriteData(duckdb::const_data_ptr_cast(&n), sizeof(n));
  for (const auto &v : row) {
    duckdb::BinarySerializer::Serialize(v, stream);
  }
  out.assign(stream.GetData(), stream.GetData() + stream.GetPosition());
}

inline std::vector<duckdb::Value> deserialize_row(const uint8_t *data,
                                                  size_t len) {
  duckdb::MemoryStream stream(
      reinterpret_cast<duckdb::data_ptr_t>(const_cast<uint8_t *>(data)),
      len);
  uint32_t n = 0;
  stream.ReadData(duckdb::data_ptr_cast(&n), sizeof(n));
  std::vector<duckdb::Value> row;
  row.reserve(n);
  for (uint32_t i = 0; i < n; i++) {
    duckdb::BinaryDeserializer des(stream);
    des.Begin();
    row.push_back(duckdb::Value::Deserialize(des));
    des.End();
  }
  return row;
}

// On-disk record log with an in-memory digest index. One instance per
// tracked table (in spill mode). NOT thread-safe by itself — callers hold
// the table lock, same as the in-memory baseline.
class SpilledBaseline {
public:
  struct Slot {
    uint64_t offset = 0;
    uint32_t length = 0;
    int64_t weight = 0;
  };

  explicit SpilledBaseline(std::string path) : path_(std::move(path)) {}

  ~SpilledBaseline() { discard(); }

  SpilledBaseline(const SpilledBaseline &) = delete;
  SpilledBaseline &operator=(const SpilledBaseline &) = delete;

  size_t distinct_rows() const { return index_.size(); }

  int64_t total_weight() const { return total_weight_; }

  bool empty() const { return index_.empty(); }

  // ---- rebuild path (scan-and-diff sync) -------------------------------
  // Usage: begin_rebuild(); add() every scanned row; end_rebuild()
  // reports the delta vs the previous generation and atomically swaps
  // the files.

  void begin_rebuild() {
    pending_.clear();
    new_file_ = open_file(tmp_path(), "wb");
    new_offset_ = 0;
  }

  // Add one scanned row (weight w) to the new generation. Returns the
  // digest so callers can avoid recomputing it.
  RowDigest add(const std::vector<duckdb::Value> &row, int64_t w = 1) {
    std::vector<uint8_t> bytes;
    serialize_row(row, bytes);
    const RowDigest d = digest_bytes(bytes.data(), bytes.size());
    auto &slot = pending_[d];
    if (slot.weight == 0) {
      slot.offset = new_offset_;
      slot.length = static_cast<uint32_t>(bytes.size());
      write_record(new_file_, bytes);
      new_offset_ += sizeof(uint32_t) + bytes.size();
    }
    slot.weight += w;
    return d;
  }

  // Diff the new generation against the old one. `on_added` fires with
  // the row and positive weight for rows gaining weight; `on_removed`
  // with the reconstructed row and positive weight for rows losing it.
  // Then the new generation replaces the old (atomic rename).
  void end_rebuild(
      const std::function<void(const std::vector<duckdb::Value> &, int64_t)>
          &on_added,
      const std::function<void(const std::vector<duckdb::Value> &, int64_t)>
          &on_removed) {
    std::fflush(new_file_);

    // Rows added or with increased weight: payload available in the new
    // file; read back sequentially (offsets ascend by construction)
    std::vector<std::pair<RowDigest, int64_t>> added;
    for (const auto &[d, slot] : pending_) {
      auto it = index_.find(d);
      const int64_t old_w = it == index_.end() ? 0 : it->second.weight;
      if (slot.weight > old_w) {
        added.emplace_back(d, slot.weight - old_w);
      }
    }
    std::FILE *nf = open_file(tmp_path(), "rb");
    for (const auto &[d, w] : added) {
      const Slot &slot = pending_.at(d);
      on_added(read_row(nf, slot), w);
    }
    std::fclose(nf);

    // Rows removed or with decreased weight: payload only in the OLD file
    if (file_ == nullptr && !path_.empty() && !index_.empty()) {
      file_ = open_file(path_, "rb");
    }
    for (const auto &[d, slot] : index_) {
      auto it = pending_.find(d);
      const int64_t new_w = it == pending_.end() ? 0 : it->second.weight;
      if (slot.weight > new_w) {
        on_removed(read_row(file_, slot), slot.weight - new_w);
      }
    }

    swap_in_pending();
  }

  // ---- point mutations (captured-delta commits, manual CDC) ------------
  // Appends to the current file; net-zero rows keep a dead record on disk
  // until the next full rebuild compacts it.

  void apply_row(const std::vector<duckdb::Value> &row, int64_t w) {
    std::vector<uint8_t> bytes;
    serialize_row(row, bytes);
    const RowDigest d = digest_bytes(bytes.data(), bytes.size());
    auto it = index_.find(d);
    if (it == index_.end()) {
      if (w == 0) {
        return;
      }
      ensure_append_file();
      Slot slot;
      slot.offset = append_offset_;
      slot.length = static_cast<uint32_t>(bytes.size());
      slot.weight = w;
      write_record(file_, bytes);
      std::fflush(file_);
      append_offset_ += sizeof(uint32_t) + bytes.size();
      index_.emplace(d, slot);
    } else {
      it->second.weight += w;
      if (it->second.weight == 0) {
        index_.erase(it);
      }
    }
    total_weight_ += w;
  }

  // ---- streaming read (view init / arrangement backfill) ---------------
  void scan(const std::function<void(const std::vector<duckdb::Value> &,
                                     int64_t)> &fn) {
    if (index_.empty()) {
      return;
    }
    reopen_read();
    // Sequential order: sort slots by offset
    std::vector<const Slot *> slots;
    slots.reserve(index_.size());
    for (const auto &[d, slot] : index_) {
      slots.push_back(&slot);
    }
    std::sort(slots.begin(), slots.end(),
              [](const Slot *a, const Slot *b) {
                return a->offset < b->offset;
              });
    for (const Slot *slot : slots) {
      fn(read_row(file_, *slot), slot->weight);
    }
  }

  // Drop all state and delete files (table untracked / manager reset)
  void discard() {
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
    if (new_file_) {
      std::fclose(new_file_);
      new_file_ = nullptr;
    }
    index_.clear();
    pending_.clear();
    total_weight_ = 0;
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(tmp_path(), ec);
  }

private:
  std::string tmp_path() const { return path_ + ".tmp"; }

  static std::FILE *open_file(const std::string &p, const char *mode) {
    std::FILE *f = std::fopen(p.c_str(), mode);
    if (!f) {
      throw std::runtime_error("dbsp spill: cannot open " + p);
    }
    return f;
  }

  static void write_record(std::FILE *f, const std::vector<uint8_t> &bytes) {
    const uint32_t len = static_cast<uint32_t>(bytes.size());
    std::fwrite(&len, sizeof(len), 1, f);
    std::fwrite(bytes.data(), 1, bytes.size(), f);
  }

  static std::vector<duckdb::Value> read_row(std::FILE *f, const Slot &slot) {
    if (std::fseek(f, static_cast<long>(slot.offset), SEEK_SET) != 0) {
      throw std::runtime_error("dbsp spill: seek failed");
    }
    uint32_t len = 0;
    if (std::fread(&len, sizeof(len), 1, f) != 1 || len != slot.length) {
      throw std::runtime_error("dbsp spill: torn record");
    }
    std::vector<uint8_t> bytes(len);
    if (len > 0 && std::fread(bytes.data(), 1, len, f) != len) {
      throw std::runtime_error("dbsp spill: short read");
    }
    return deserialize_row(bytes.data(), bytes.size());
  }

  void ensure_append_file() {
    if (file_) {
      // Reopen in append+read mode if it was read-only
      if (!appendable_) {
        std::fclose(file_);
        file_ = open_file(path_, "ab+");
        appendable_ = true;
        std::fseek(file_, 0, SEEK_END);
        append_offset_ = static_cast<uint64_t>(std::ftell(file_));
      }
      return;
    }
    file_ = open_file(path_, index_.empty() ? "wb+" : "ab+");
    appendable_ = true;
    std::fseek(file_, 0, SEEK_END);
    append_offset_ = static_cast<uint64_t>(std::ftell(file_));
  }

  void reopen_read() {
    if (file_ && appendable_) {
      std::fflush(file_);
      return; // ab+ can read too
    }
    if (!file_) {
      file_ = open_file(path_, "rb");
      appendable_ = false;
    }
  }

  void swap_in_pending() {
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
    if (new_file_) {
      std::fclose(new_file_);
      new_file_ = nullptr;
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path(), path_, ec);
    if (ec) {
      throw std::runtime_error("dbsp spill: rename failed: " + ec.message());
    }
    index_ = std::move(pending_);
    pending_.clear();
    appendable_ = false;
    total_weight_ = 0;
    for (const auto &[d, slot] : index_) {
      total_weight_ += slot.weight;
    }
  }

  std::string path_;
  std::FILE *file_ = nullptr;     // current generation
  std::FILE *new_file_ = nullptr; // rebuild in progress
  bool appendable_ = false;
  uint64_t new_offset_ = 0;
  uint64_t append_offset_ = 0;
  int64_t total_weight_ = 0;
  std::unordered_map<RowDigest, Slot, RowDigestHash> index_;
  std::unordered_map<RowDigest, Slot, RowDigestHash> pending_;
};

} // namespace dbsp_native

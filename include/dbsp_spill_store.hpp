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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace dbsp_native {

// Spill mode globals (set by CDCManager::set_spill under struct_mutex_;
// read at view construction under the same lock)
inline std::atomic<bool> g_spill_mode{false};
inline std::string g_spill_dir;
inline std::atomic<uint64_t> g_spill_file_seq{0};

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
// Row codec: typed fast paths (tag byte + raw payload) for the common
// scalar types, duckdb's BinarySerializer as the fallback tag. Spill
// files are process-disposable (runtime dirs are swept, never reloaded
// across code versions), so this format carries no compatibility burden
// — both codec halves always change together. The duckdb serializer
// costs ~650ns/row on a 3-column row (object framing per Value); the
// fast paths are ~10x cheaper, and generational rebuilds pay the codec
// for EVERY row of the table on EVERY spilled sync.
namespace rowcodec {
enum : uint8_t {
  kFallback = 0, // u32 length + BinarySerializer blob
  kNull = 1,     // u8 LogicalTypeId (primitive ids only)
  kInt32 = 2,
  kInt64 = 3,
  kDouble = 4,
  kVarchar = 5, // u32 length + bytes
  kBool = 6,
};

template <typename T> inline void put_raw(std::vector<uint8_t> &out, T v) {
  const auto *p = reinterpret_cast<const uint8_t *>(&v);
  out.insert(out.end(), p, p + sizeof(T));
}
template <typename T> inline T get_raw(const uint8_t *&p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  p += sizeof(T);
  return v;
}
} // namespace rowcodec

inline void serialize_row(const std::vector<duckdb::Value> &row,
                          std::vector<uint8_t> &out) {
  using namespace rowcodec;
  out.clear();
  const uint32_t n = static_cast<uint32_t>(row.size());
  put_raw(out, n);
  for (const auto &v : row) {
    const auto id = v.type().id();
    if (v.IsNull()) {
      switch (id) {
      case duckdb::LogicalTypeId::INTEGER:
      case duckdb::LogicalTypeId::BIGINT:
      case duckdb::LogicalTypeId::DOUBLE:
      case duckdb::LogicalTypeId::VARCHAR:
      case duckdb::LogicalTypeId::BOOLEAN:
      case duckdb::LogicalTypeId::SQLNULL: {
        out.push_back(kNull);
        out.push_back(static_cast<uint8_t>(id));
        continue;
      }
      default:
        break; // typed NULLs of complex types take the fallback
      }
    } else {
      switch (id) {
      case duckdb::LogicalTypeId::INTEGER:
        out.push_back(kInt32);
        put_raw(out, duckdb::IntegerValue::Get(v));
        continue;
      case duckdb::LogicalTypeId::BIGINT:
        out.push_back(kInt64);
        put_raw(out, duckdb::BigIntValue::Get(v));
        continue;
      case duckdb::LogicalTypeId::DOUBLE:
        out.push_back(kDouble);
        put_raw(out, duckdb::DoubleValue::Get(v));
        continue;
      case duckdb::LogicalTypeId::VARCHAR: {
        out.push_back(kVarchar);
        const auto &str = duckdb::StringValue::Get(v);
        put_raw(out, static_cast<uint32_t>(str.size()));
        out.insert(out.end(), str.begin(), str.end());
        continue;
      }
      case duckdb::LogicalTypeId::BOOLEAN:
        out.push_back(kBool);
        out.push_back(duckdb::BooleanValue::Get(v) ? 1 : 0);
        continue;
      default:
        break;
      }
    }
    // fallback: duckdb's own serializer, length-prefixed
    out.push_back(kFallback);
    duckdb::MemoryStream stream;
    duckdb::BinarySerializer::Serialize(v, stream);
    put_raw(out, static_cast<uint32_t>(stream.GetPosition()));
    out.insert(out.end(), stream.GetData(),
               stream.GetData() + stream.GetPosition());
  }
}

inline std::vector<duckdb::Value> deserialize_row(const uint8_t *data,
                                                  size_t len) {
  using namespace rowcodec;
  const uint8_t *p = data;
  const uint32_t n = get_raw<uint32_t>(p);
  std::vector<duckdb::Value> row;
  row.reserve(n);
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t tag = *p++;
    switch (tag) {
    case kNull:
      row.push_back(duckdb::Value(
          duckdb::LogicalType(static_cast<duckdb::LogicalTypeId>(*p++))));
      break;
    case kInt32:
      row.push_back(duckdb::Value::INTEGER(get_raw<int32_t>(p)));
      break;
    case kInt64:
      row.push_back(duckdb::Value::BIGINT(get_raw<int64_t>(p)));
      break;
    case kDouble:
      row.push_back(duckdb::Value::DOUBLE(get_raw<double>(p)));
      break;
    case kVarchar: {
      const uint32_t sz = get_raw<uint32_t>(p);
      row.push_back(duckdb::Value(
          std::string(reinterpret_cast<const char *>(p), sz)));
      p += sz;
      break;
    }
    case kBool:
      row.push_back(duckdb::Value::BOOLEAN(*p++ != 0));
      break;
    default: { // kFallback
      const uint32_t sz = get_raw<uint32_t>(p);
      duckdb::MemoryStream stream(
          reinterpret_cast<duckdb::data_ptr_t>(const_cast<uint8_t *>(p)),
          sz);
      duckdb::BinaryDeserializer des(stream);
      des.Begin();
      row.push_back(duckdb::Value::Deserialize(des));
      des.End();
      p += sz;
      break;
    }
    }
  }
  (void)len;
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
    std::vector<uint8_t> &bytes = scratch_bytes_;
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
  std::vector<uint8_t> scratch_bytes_;
  std::unordered_map<RowDigest, Slot, RowDigestHash> pending_;
};

// Disk-backed key → bucket index for join arrangements (Phase K2).
// Buckets (row → weight maps) live in an append-only log; RAM keeps a
// key-digest → (offset, length) slot map plus an LRU cache of hot
// deserialized buckets. Probes hit the cache or cost one disk read;
// updates rewrite the bucket at the log tail. When the log grows past
// 2× the live payload (and a floor), live buckets are rewritten to a
// fresh file (atomic rename).
//
// Same collision stance as SpilledBaseline: 128-bit key digests, no
// byte compare. NOT thread-safe — callers serialize (view_mutex_).
class SpilledBucketIndex {
public:
  using Bucket = std::vector<std::pair<std::vector<duckdb::Value>, int64_t>>;

  explicit SpilledBucketIndex(std::string path, size_t cache_capacity = 1024)
      : path_(std::move(path)), cache_capacity_(cache_capacity) {}

  ~SpilledBucketIndex() { discard(); }

  SpilledBucketIndex(const SpilledBucketIndex &) = delete;
  SpilledBucketIndex &operator=(const SpilledBucketIndex &) = delete;

  size_t key_count() const { return slots_.size(); }

  // Bucket for `key_digest`, or nullptr when absent. The pointer is
  // owned by the cache and stays valid until the next probe/update on
  // this index (sequential probe-then-consume usage only).
  const Bucket *probe(const RowDigest &key_digest) {
    auto it = slots_.find(key_digest);
    if (it == slots_.end()) {
      return nullptr;
    }
    return &load(key_digest, it->second);
  }

  // Merge `delta` (row, weight pairs) into the bucket. Rows at net-zero
  // weight leave the bucket; empty buckets leave the index.
  void update(const RowDigest &key_digest, const Bucket &delta) {
    Bucket merged;
    auto it = slots_.find(key_digest);
    if (it != slots_.end()) {
      merged = load(key_digest, it->second); // copy out of cache
      live_bytes_ -= it->second.length;
    }
    for (const auto &[row, w] : delta) {
      bool found = false;
      for (auto &[mrow, mw] : merged) {
        if (rows_equal(mrow, row)) {
          mw += w;
          found = true;
          break;
        }
      }
      if (!found && w != 0) {
        merged.emplace_back(row, w);
      }
    }
    merged.erase(std::remove_if(merged.begin(), merged.end(),
                                [](const auto &e) { return e.second == 0; }),
                 merged.end());
    cache_erase(key_digest);
    if (merged.empty()) {
      if (it != slots_.end()) {
        slots_.erase(it);
      }
      maybe_compact();
      return;
    }
    const Slot slot = append_bucket(merged);
    slots_[key_digest] = slot;
    live_bytes_ += slot.length;
    cache_put(key_digest, std::move(merged));
    maybe_compact();
  }

  // Full walk (unspill migration)
  void scan(const std::function<void(const Bucket &)> &fn) {
    for (const auto &[d, slot] : slots_) {
      fn(read_bucket(slot));
    }
  }

  void discard() {
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
    slots_.clear();
    cache_.clear();
    lru_.clear();
    live_bytes_ = file_bytes_ = 0;
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_ + ".tmp", ec);
  }

private:
  struct Slot {
    uint64_t offset = 0;
    uint32_t length = 0;
  };

  static bool rows_equal(const std::vector<duckdb::Value> &a,
                         const std::vector<duckdb::Value> &b) {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
      const bool an = a[i].IsNull(), bn = b[i].IsNull();
      if (an != bn) {
        return false;
      }
      if (!an && !(a[i] == b[i])) {
        return false;
      }
    }
    return true;
  }

  static void serialize_bucket(const Bucket &bucket,
                               std::vector<uint8_t> &out) {
    duckdb::MemoryStream stream;
    const uint32_t n = static_cast<uint32_t>(bucket.size());
    stream.WriteData(duckdb::const_data_ptr_cast(&n), sizeof(n));
    for (const auto &[row, w] : bucket) {
      stream.WriteData(duckdb::const_data_ptr_cast(&w), sizeof(w));
      std::vector<uint8_t> rb;
      serialize_row(row, rb);
      const uint32_t rl = static_cast<uint32_t>(rb.size());
      stream.WriteData(duckdb::const_data_ptr_cast(&rl), sizeof(rl));
      stream.WriteData(rb.data(), rb.size());
    }
    out.assign(stream.GetData(), stream.GetData() + stream.GetPosition());
  }

  static Bucket deserialize_bucket(const uint8_t *data, size_t len) {
    Bucket bucket;
    size_t pos = 0;
    auto read = [&](void *dst, size_t n) {
      std::memcpy(dst, data + pos, n);
      pos += n;
    };
    uint32_t n = 0;
    read(&n, sizeof(n));
    bucket.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
      int64_t w = 0;
      read(&w, sizeof(w));
      uint32_t rl = 0;
      read(&rl, sizeof(rl));
      bucket.emplace_back(deserialize_row(data + pos, rl), w);
      pos += rl;
    }
    return bucket;
  }

  void ensure_file() {
    if (!file_) {
      file_ = std::fopen(path_.c_str(), "ab+");
      if (!file_) {
        throw std::runtime_error("dbsp spill: cannot open " + path_);
      }
      std::fseek(file_, 0, SEEK_END);
      file_bytes_ = static_cast<uint64_t>(std::ftell(file_));
    }
  }

  Slot append_bucket(const Bucket &bucket) {
    ensure_file();
    std::vector<uint8_t> bytes;
    serialize_bucket(bucket, bytes);
    std::fseek(file_, 0, SEEK_END);
    Slot slot;
    slot.offset = static_cast<uint64_t>(std::ftell(file_));
    slot.length = static_cast<uint32_t>(bytes.size());
    std::fwrite(bytes.data(), 1, bytes.size(), file_);
    std::fflush(file_);
    file_bytes_ = slot.offset + bytes.size();
    return slot;
  }

  Bucket read_bucket(const Slot &slot) {
    ensure_file();
    if (std::fseek(file_, static_cast<long>(slot.offset), SEEK_SET) != 0) {
      throw std::runtime_error("dbsp spill: seek failed");
    }
    std::vector<uint8_t> bytes(slot.length);
    if (slot.length > 0 &&
        std::fread(bytes.data(), 1, slot.length, file_) != slot.length) {
      throw std::runtime_error("dbsp spill: short bucket read");
    }
    return deserialize_bucket(bytes.data(), bytes.size());
  }

  const Bucket &load(const RowDigest &d, const Slot &slot) {
    auto cit = cache_.find(d);
    if (cit != cache_.end()) {
      lru_.splice(lru_.begin(), lru_, cit->second.second);
      return cit->second.first;
    }
    Bucket bucket = read_bucket(slot);
    return cache_put(d, std::move(bucket));
  }

  const Bucket &cache_put(const RowDigest &d, Bucket &&bucket) {
    cache_erase(d);
    lru_.push_front(d);
    auto [it, ok] =
        cache_.emplace(d, std::make_pair(std::move(bucket), lru_.begin()));
    while (cache_.size() > cache_capacity_) {
      cache_.erase(lru_.back());
      lru_.pop_back();
    }
    return it->second.first;
  }

  void cache_erase(const RowDigest &d) {
    auto it = cache_.find(d);
    if (it != cache_.end()) {
      lru_.erase(it->second.second);
      cache_.erase(it);
    }
  }

  void maybe_compact() {
    constexpr uint64_t kFloor = 4 * 1024 * 1024;
    if (file_bytes_ < kFloor || file_bytes_ < 2 * live_bytes_) {
      return;
    }
    const std::string tmp = path_ + ".tmp";
    std::FILE *nf = std::fopen(tmp.c_str(), "wb");
    if (!nf) {
      throw std::runtime_error("dbsp spill: cannot open " + tmp);
    }
    uint64_t off = 0;
    std::unordered_map<RowDigest, Slot, RowDigestHash> new_slots;
    new_slots.reserve(slots_.size());
    for (const auto &[d, slot] : slots_) {
      Bucket bucket = read_bucket(slot);
      std::vector<uint8_t> bytes;
      serialize_bucket(bucket, bytes);
      std::fwrite(bytes.data(), 1, bytes.size(), nf);
      new_slots[d] = Slot{off, static_cast<uint32_t>(bytes.size())};
      off += bytes.size();
    }
    std::fclose(nf);
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    if (ec) {
      throw std::runtime_error("dbsp spill: rename failed: " + ec.message());
    }
    slots_ = std::move(new_slots);
    file_bytes_ = off;
    live_bytes_ = off;
  }

  std::string path_;
  std::FILE *file_ = nullptr;
  uint64_t live_bytes_ = 0;
  uint64_t file_bytes_ = 0;
  size_t cache_capacity_;
  std::unordered_map<RowDigest, Slot, RowDigestHash> slots_;
  std::unordered_map<RowDigest,
                     std::pair<Bucket, std::list<RowDigest>::iterator>,
                     RowDigestHash>
      cache_;
  std::list<RowDigest> lru_;
};

} // namespace dbsp_native

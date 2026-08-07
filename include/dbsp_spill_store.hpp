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
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <list>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
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

// Vectorized serialization (bounded-RAM speed levers): true when every
// column of the chunk has a typed fast path below. The whole table takes
// the boxed row path otherwise — coverage is an optimization knob.
inline bool chunk_types_fast(const duckdb::DataChunk &chunk) {
  for (duckdb::idx_t c = 0; c < chunk.ColumnCount(); c++) {
    switch (chunk.data[c].GetType().id()) {
    case duckdb::LogicalTypeId::INTEGER:
    case duckdb::LogicalTypeId::BIGINT:
    case duckdb::LogicalTypeId::DOUBLE:
    case duckdb::LogicalTypeId::VARCHAR:
    case duckdb::LogicalTypeId::BOOLEAN:
      continue;
    default:
      return false;
    }
  }
  return true;
}

// Serialize every row of a FLATTENED chunk straight from vector data —
// byte-identical to serialize_row, no per-cell duckdb::Value. Caller
// checked chunk_types_fast.
inline void
serialize_chunk(duckdb::DataChunk &chunk,
                const std::function<void(const std::vector<uint8_t> &)> &emit) {
  using namespace rowcodec;
  const duckdb::idx_t n = chunk.size();
  const duckdb::idx_t ncols = chunk.ColumnCount();
  struct Col {
    uint8_t tag;
    uint8_t type_id;
    const void *data;
    duckdb::ValidityMask *validity;
  };
  std::vector<Col> cols(ncols);
  for (duckdb::idx_t c = 0; c < ncols; c++) {
    auto &vec = chunk.data[c];
    Col &col = cols[c];
    col.type_id = static_cast<uint8_t>(vec.GetType().id());
    col.validity = &duckdb::FlatVector::Validity(vec);
    switch (vec.GetType().id()) {
    case duckdb::LogicalTypeId::INTEGER:
      col.tag = kInt32;
      col.data = duckdb::FlatVector::GetData<int32_t>(vec);
      break;
    case duckdb::LogicalTypeId::BIGINT:
      col.tag = kInt64;
      col.data = duckdb::FlatVector::GetData<int64_t>(vec);
      break;
    case duckdb::LogicalTypeId::DOUBLE:
      col.tag = kDouble;
      col.data = duckdb::FlatVector::GetData<double>(vec);
      break;
    case duckdb::LogicalTypeId::VARCHAR:
      col.tag = kVarchar;
      col.data = duckdb::FlatVector::GetData<duckdb::string_t>(vec);
      break;
    default: // BOOLEAN (chunk_types_fast admits nothing else)
      col.tag = kBool;
      col.data = duckdb::FlatVector::GetData<bool>(vec);
      break;
    }
  }
  std::vector<uint8_t> bytes;
  for (duckdb::idx_t i = 0; i < n; i++) {
    bytes.clear();
    put_raw(bytes, static_cast<uint32_t>(ncols));
    for (duckdb::idx_t c = 0; c < ncols; c++) {
      const Col &col = cols[c];
      if (!col.validity->RowIsValid(i)) {
        bytes.push_back(kNull);
        bytes.push_back(col.type_id);
        continue;
      }
      bytes.push_back(col.tag);
      switch (col.tag) {
      case kInt32:
        put_raw(bytes, static_cast<const int32_t *>(col.data)[i]);
        break;
      case kInt64:
        put_raw(bytes, static_cast<const int64_t *>(col.data)[i]);
        break;
      case kDouble:
        put_raw(bytes, static_cast<const double *>(col.data)[i]);
        break;
      case kVarchar: {
        const auto &s = static_cast<const duckdb::string_t *>(col.data)[i];
        put_raw(bytes, static_cast<uint32_t>(s.GetSize()));
        const auto *sp = reinterpret_cast<const uint8_t *>(s.GetData());
        bytes.insert(bytes.end(), sp, sp + s.GetSize());
        break;
      }
      default: // kBool
        bytes.push_back(static_cast<const bool *>(col.data)[i] ? 1 : 0);
        break;
      }
    }
    emit(bytes);
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

  size_t distinct_rows() const {
    if (flat_entries_ != nullptr) {
      return static_cast<size_t>(flat_count_) + overlay_new_ - overlay_dead_;
    }
    return index_.size();
  }

  int64_t total_weight() const { return total_weight_; }

  bool empty() const { return distinct_rows() == 0; }

  // Live weight of one digest across flat + overlay (overlay masks flat).
  int64_t live_weight(const RowDigest &d) const {
    auto it = index_.find(d);
    if (it != index_.end()) {
      return it->second.weight;
    }
    Slot s;
    return flat_find(d, s) ? s.weight : 0;
  }

  // Resident RAM attributable to the index: the mmap'd flat layer is
  // page-cache (evictable, not process-owned) — only the overlay counts.
  size_t resident_index_entries() const {
    return flat_entries_ != nullptr ? index_.size() : index_.size();
  }
  bool flat_mapped() const { return flat_entries_ != nullptr; }

  // ---- durable mode (bounded-RAM speed levers / recovery inc B) ---------
  // Spill files in a per-DATABASE directory survive process exit; with a
  // saved index sidecar, a reopen rebuilds the baseline by one bulk index
  // read instead of rescanning the whole table (the dominant cost of the
  // first edit after attach). The record log + index are only trusted
  // when the caller's watermark (count + row-hash, verified against live
  // storage) matches what the index was saved under AND the log file size
  // is exactly what the save recorded — any append/rebuild since then
  // invalidates the pair and forces the ordinary rescan.

  void set_keep_files(bool keep) { keep_files_ = keep; }

  static constexpr uint64_t kIdxMagic = 0xDB5B1DE0BA5E11FEULL;
  // v2 (tier-2 mmap): entries SORTED by digest; adopt maps the file
  // read-only and binary-searches it — zero build cost, size-independent
  // reopen. index_ becomes a mutation OVERLAY on top (absolute slots;
  // weight 0 = tombstone masking a flat entry). v1 indexes are rejected
  // (one rescan on upgrade).
  static constexpr uint32_t kIdxVersion = 2;
  static constexpr size_t kFlatEntryBytes = 36; // hi8 lo8 off8 len4 w8

  // Delta sidecar (delta-append saves): the whole current OVERLAY written
  // as a small file next to the base index — a dirty save is O(changed
  // rows), not O(baseline). Chained to its base by the base's watermark
  // and entry count; any mismatch (or a log append after the delta save)
  // invalidates the pair and the adopt falls back to the ordinary rescan.
  static constexpr uint64_t kIdxDeltaMagic = 0xDB5B1DE17ADE17A5ULL;
  static constexpr uint32_t kIdxDeltaVersion = 1;
  // Compaction threshold: fold delta into a fresh base once the overlay
  // exceeds this fraction of the base (denominator; 10 = 10%).
  static constexpr uint64_t kIdxDeltaMaxFraction = 10;

  std::string idx_path() const { return path_ + ".idx"; }
  std::string idx_delta_path() const { return path_ + ".idx.d"; }

  // Persist the digest index next to the record log, entries SORTED by
  // digest (v2 mmap format). Caller supplies the watermark the baseline
  // currently corresponds to (save_checkpoint's just-computed values).
  // fsyncs the log first so the recorded file size is durable before the
  // index that references it. Skipped for free when an adopted flat
  // generation is still clean (no overlay, same watermark) — the sidecar
  // on disk is already exactly this content.
  bool save_index(int64_t wm_count, const std::string &wm_hash) {
    if (new_file_ != nullptr) {
      return false; // rebuild in flight: not a consistent generation
    }
    // Clean skip: the sidecars on disk already describe exactly this
    // content — the watermark identifies table content, and every
    // successful save (full OR delta) records the watermark it saved
    // under. (The old check required an untouched adopted base, so the
    // first dirty save permanently disabled skipping for the session.)
    if (wm_count == saved_wm_count_ && wm_hash == saved_wm_hash_) {
      return true;
    }
    if (flat_entries_ != nullptr && index_.empty() &&
        wm_count == flat_wm_count_ && wm_hash == flat_wm_hash_) {
      saved_wm_count_ = wm_count;
      saved_wm_hash_ = wm_hash;
      return true;
    }
    if (file_ != nullptr) {
      std::fflush(file_);
      ::fsync(fileno(file_));
    }
    std::error_code ec;
    const uint64_t fsize = std::filesystem::exists(path_, ec)
                               ? std::filesystem::file_size(path_, ec)
                               : 0;
    // Delta-append save: with an adopted base and a small overlay, write
    // ONLY the overlay (tombstones included — weight 0 masks a base
    // entry) chained to the base. O(changed rows) instead of O(baseline).
    if (std::getenv("DBSP_DEBUG_SYNC")) {
      std::cerr << "[dbsp] save_index " << path_ << ": flat="
                << (flat_entries_ != nullptr ? "yes" : "NO") << " flat_n="
                << flat_count_ << " overlay=" << index_.size() << "\n";
    }
    if (flat_entries_ != nullptr &&
        index_.size() * kIdxDeltaMaxFraction < flat_count_) {
      if (save_index_delta(wm_count, wm_hash, fsize)) {
        saved_wm_count_ = wm_count;
        saved_wm_hash_ = wm_hash;
        return true;
      }
      // fall through to the full fold on any delta-write failure
    }
    // Full fold: gather live entries (flat ∖ tombstones ∪ overlay) as
    // packed bytes, then sort by digest.
    std::vector<std::array<uint8_t, kFlatEntryBytes>> entries;
    entries.reserve(distinct_rows());
    for_each_slot([&](const RowDigest &d, const Slot &s) {
      std::array<uint8_t, kFlatEntryBytes> e;
      uint8_t *p = e.data();
      std::memcpy(p, &d.hi, 8);
      std::memcpy(p + 8, &d.lo, 8);
      std::memcpy(p + 16, &s.offset, 8);
      std::memcpy(p + 24, &s.length, 4);
      std::memcpy(p + 28, &s.weight, 8);
      entries.push_back(e);
    });
    std::sort(entries.begin(), entries.end(),
              [](const auto &a, const auto &b) {
                return std::memcmp(a.data(), b.data(), 16) < 0;
              });
    std::FILE *f = std::fopen((idx_path() + ".tmp").c_str(), "wb");
    if (f == nullptr) {
      return false;
    }
    auto put = [&](const void *p, size_t n) {
      std::fwrite(p, 1, n, f);
    };
    const uint64_t magic = kIdxMagic;
    const uint32_t ver = kIdxVersion;
    const uint32_t hlen = static_cast<uint32_t>(wm_hash.size());
    const uint64_t n = entries.size();
    put(&magic, 8);
    put(&ver, 4);
    put(&wm_count, 8);
    put(&hlen, 4);
    put(wm_hash.data(), hlen);
    put(&fsize, 8);
    put(&n, 8);
    for (const auto &e : entries) {
      put(e.data(), kFlatEntryBytes);
    }
    std::fflush(f);
    ::fsync(fileno(f));
    std::fclose(f);
    std::filesystem::rename(idx_path() + ".tmp", idx_path(), ec);
    if (ec) {
      return false;
    }
    // A full fold supersedes any delta chained to the OLD base.
    std::filesystem::remove(idx_delta_path(), ec);
    // Adopt the just-written base: the overlay folds into the mmap so the
    // next save skips (clean) or writes a small fresh delta, instead of
    // re-folding the whole baseline every save.
    try_load_index(wm_count, wm_hash);
    saved_wm_count_ = wm_count;
    saved_wm_hash_ = wm_hash;
    return true;
  }

  // Write the current overlay as a delta sidecar chained to the adopted
  // base (base watermark + entry count + current log size). tmp+rename.
  bool save_index_delta(int64_t wm_count, const std::string &wm_hash,
                        uint64_t log_fsize) {
    std::vector<std::array<uint8_t, kFlatEntryBytes>> entries;
    entries.reserve(index_.size());
    for (const auto &[d, s] : index_) {
      std::array<uint8_t, kFlatEntryBytes> e;
      uint8_t *p = e.data();
      std::memcpy(p, &d.hi, 8);
      std::memcpy(p + 8, &d.lo, 8);
      std::memcpy(p + 16, &s.offset, 8);
      std::memcpy(p + 24, &s.length, 4);
      std::memcpy(p + 28, &s.weight, 8);
      entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto &a, const auto &b) {
                return std::memcmp(a.data(), b.data(), 16) < 0;
              });
    std::FILE *f = std::fopen((idx_delta_path() + ".tmp").c_str(), "wb");
    if (f == nullptr) {
      return false;
    }
    auto put = [&](const void *p, size_t n) {
      std::fwrite(p, 1, n, f);
    };
    const uint64_t magic = kIdxDeltaMagic;
    const uint32_t ver = kIdxDeltaVersion;
    const uint32_t hlen = static_cast<uint32_t>(wm_hash.size());
    const uint32_t base_hlen = static_cast<uint32_t>(flat_wm_hash_.size());
    const uint64_t n = entries.size();
    put(&magic, 8);
    put(&ver, 4);
    put(&wm_count, 8);
    put(&hlen, 4);
    put(wm_hash.data(), hlen);
    put(&log_fsize, 8);
    put(&flat_wm_count_, 8);
    put(&base_hlen, 4);
    put(flat_wm_hash_.data(), base_hlen);
    put(&flat_count_, 8);
    put(&n, 8);
    for (const auto &e : entries) {
      put(e.data(), kFlatEntryBytes);
    }
    std::fflush(f);
    ::fsync(fileno(f));
    std::fclose(f);
    std::error_code ec;
    std::filesystem::rename(idx_delta_path() + ".tmp", idx_delta_path(), ec);
    return !ec;
  }

  // Adopt a durable log+index pair for the given watermark: mmap the
  // sorted index read-only — O(1) regardless of row count; lookups
  // binary-search the mapping and pages fault in on demand. A delta
  // sidecar chained to the base (delta-append saves) loads into the
  // overlay on top. Any mismatch leaves this object empty and returns
  // false — caller rescans.
  bool try_load_index(int64_t wm_count, const std::string &wm_hash) {
    if (try_load_index_with_delta(wm_count, wm_hash)) {
      return true;
    }
    if (!adopt_base_file(wm_count, wm_hash, /*check_log_fsize=*/true)) {
      return false;
    }
    saved_wm_count_ = wm_count;
    saved_wm_hash_ = wm_hash;
    return true;
  }

  // Delta-chain adopt: the delta's watermark must match the caller's, its
  // recorded log size must match the live log (an append since the delta
  // save invalidates the pair), and its recorded base identity must match
  // the base file — then the base mmaps as usual and the delta entries
  // populate the overlay.
  bool try_load_index_with_delta(int64_t wm_count,
                                 const std::string &wm_hash) {
    std::error_code ec;
    if (!std::filesystem::exists(idx_delta_path(), ec) ||
        !std::filesystem::exists(path_, ec)) {
      return false;
    }
    std::FILE *f = std::fopen(idx_delta_path().c_str(), "rb");
    if (f == nullptr) {
      return false;
    }
    auto get = [&](void *p, size_t n) {
      return std::fread(p, 1, n, f) == n;
    };
    uint64_t magic = 0, fsize = 0, base_n = 0, n = 0;
    uint32_t ver = 0, hlen = 0, base_hlen = 0;
    int64_t count = 0, base_count = 0;
    bool ok = get(&magic, 8) && magic == kIdxDeltaMagic && get(&ver, 4) &&
              ver == kIdxDeltaVersion && get(&count, 8) && get(&hlen, 4);
    std::string hash(hlen, '\0');
    ok = ok && (hlen == 0 || get(hash.data(), hlen)) && get(&fsize, 8) &&
         get(&base_count, 8) && get(&base_hlen, 4);
    std::string base_hash(base_hlen, '\0');
    ok = ok && (base_hlen == 0 || get(base_hash.data(), base_hlen)) &&
         get(&base_n, 8) && get(&n, 8);
    std::vector<std::array<uint8_t, kFlatEntryBytes>> entries(n);
    for (uint64_t i = 0; ok && i < n; i++) {
      ok = get(entries[i].data(), kFlatEntryBytes);
    }
    std::fclose(f);
    if (!ok || count != wm_count || hash != wm_hash ||
        fsize != (std::filesystem::exists(path_, ec)
                      ? std::filesystem::file_size(path_, ec)
                      : 0)) {
      if (std::getenv("DBSP_DEBUG_SYNC")) {
        std::cerr << "[dbsp] delta adopt DECLINED " << path_ << ": ok=" << ok
                  << " count=" << count << "/" << wm_count << " hash="
                  << (hash == wm_hash) << " fsize=" << fsize << "/"
                  << (std::filesystem::exists(path_, ec)
                          ? std::filesystem::file_size(path_, ec)
                          : 0)
                  << "\n";
      }
      return false;
    }
    if (!adopt_base_file(base_count, base_hash,
                         /*check_log_fsize=*/false)) {
      if (std::getenv("DBSP_DEBUG_SYNC")) {
        std::cerr << "[dbsp] delta adopt: base DECLINED " << path_ << "\n";
      }
      return false;
    }
    if (flat_count_ != base_n) {
      flat_unmap(); // base is not the one the delta was chained to
      index_.clear();
      total_weight_ = 0;
      return false;
    }
    for (const auto &e : entries) {
      RowDigest d;
      Slot s;
      const uint8_t *p = e.data();
      std::memcpy(&d.hi, p, 8);
      std::memcpy(&d.lo, p + 8, 8);
      std::memcpy(&s.offset, p + 16, 8);
      std::memcpy(&s.length, p + 24, 4);
      std::memcpy(&s.weight, p + 28, 8);
      index_[d] = s;
      Slot flat;
      const bool in_flat = flat_find(d, flat);
      if (s.weight == 0) {
        overlay_dead_++; // tombstone masking a base entry
      } else if (!in_flat) {
        overlay_new_++; // overlay-born row
      }
    }
    // Content count is the verified watermark; the log grew to the
    // delta-recorded size (its overlay offsets point into that tail).
    total_weight_ = wm_count;
    append_offset_ = fsize;
    appendable_ = false;
    saved_wm_count_ = wm_count;
    saved_wm_hash_ = wm_hash;
    return true;
  }

  // mmap the base index file, verifying its recorded watermark. The log
  // size check applies only for a base-only adopt — under a delta chain
  // the log has legitimately grown past the base's recorded size and the
  // delta's own size check governs.
  bool adopt_base_file(int64_t wm_count, const std::string &wm_hash,
                       bool check_log_fsize) {
    std::error_code ec;
    if (!std::filesystem::exists(idx_path(), ec) ||
        !std::filesystem::exists(path_, ec)) {
      return false;
    }
    std::FILE *f = std::fopen(idx_path().c_str(), "rb");
    if (f == nullptr) {
      return false;
    }
    auto get = [&](void *p, size_t n) {
      return std::fread(p, 1, n, f) == n;
    };
    uint64_t magic = 0, fsize = 0, n = 0;
    uint32_t ver = 0, hlen = 0;
    int64_t count = 0;
    bool ok = get(&magic, 8) && magic == kIdxMagic && get(&ver, 4) &&
              ver == kIdxVersion && get(&count, 8) && get(&hlen, 4);
    std::string hash(hlen, '\0');
    ok = ok && (hlen == 0 || get(hash.data(), hlen)) && get(&fsize, 8) &&
         get(&n, 8);
    const long header_end = ok ? std::ftell(f) : -1;
    std::fclose(f);
    if (!ok || count != wm_count || hash != wm_hash || header_end < 0) {
      if (std::getenv("DBSP_DEBUG_SYNC")) {
        std::cerr << "[dbsp] base adopt DECLINED " << path_ << ": ok=" << ok
                  << " count=" << count << "/" << wm_count << " hash="
                  << (hash == wm_hash) << "\n";
      }
      return false;
    }
    if (check_log_fsize &&
        fsize != (std::filesystem::exists(path_, ec)
                      ? std::filesystem::file_size(path_, ec)
                      : 0)) {
      if (std::getenv("DBSP_DEBUG_SYNC")) {
        std::cerr << "[dbsp] base adopt DECLINED " << path_ << ": fsize="
                  << fsize << "/"
                  << (std::filesystem::exists(path_, ec)
                          ? std::filesystem::file_size(path_, ec)
                          : 0)
                  << "\n";
      }
      return false;
    }
    const uint64_t need =
        static_cast<uint64_t>(header_end) + n * kFlatEntryBytes;
    const uint64_t actual = std::filesystem::file_size(idx_path(), ec);
    if (ec || actual < need) {
      return false; // truncated index: rescan
    }
    const int fd = ::open(idx_path().c_str(), O_RDONLY);
    if (fd < 0) {
      return false;
    }
    void *map = ::mmap(nullptr, static_cast<size_t>(need), PROT_READ,
                       MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
      ::close(fd);
      return false;
    }
    flat_unmap();
    flat_fd_ = fd;
    flat_map_ = map;
    flat_map_len_ = static_cast<size_t>(need);
    flat_entries_ = static_cast<const uint8_t *>(map) + header_end;
    flat_count_ = n;
    flat_wm_count_ = wm_count;
    flat_wm_hash_ = wm_hash;
    index_.clear();
    overlay_new_ = 0;
    overlay_dead_ = 0;
    // Total weight == the verified watermark COUNT(*) by definition (the
    // baseline is exactly the committed table content).
    total_weight_ = wm_count;
    append_offset_ = fsize;
    appendable_ = false;
    return true;
  }

  // ---- flat layer internals ---------------------------------------------

  RowDigest flat_digest(uint64_t i) const {
    RowDigest d;
    const uint8_t *p = flat_entries_ + i * kFlatEntryBytes;
    std::memcpy(&d.hi, p, 8);
    std::memcpy(&d.lo, p + 8, 8);
    return d;
  }

  Slot flat_slot(uint64_t i) const {
    Slot s;
    const uint8_t *p = flat_entries_ + i * kFlatEntryBytes;
    std::memcpy(&s.offset, p + 16, 8);
    std::memcpy(&s.length, p + 24, 4);
    std::memcpy(&s.weight, p + 28, 8);
    return s;
  }

  bool flat_find(const RowDigest &d, Slot &out) const {
    if (flat_entries_ == nullptr) {
      return false;
    }
    uint64_t lo = 0, hi = flat_count_;
    uint8_t key[16];
    std::memcpy(key, &d.hi, 8);
    std::memcpy(key + 8, &d.lo, 8);
    while (lo < hi) {
      const uint64_t mid = lo + (hi - lo) / 2;
      const int c =
          std::memcmp(flat_entries_ + mid * kFlatEntryBytes, key, 16);
      if (c == 0) {
        out = flat_slot(mid);
        return true;
      }
      if (c < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return false;
  }

  // Every LIVE (digest, slot): flat entries not masked by the overlay,
  // plus overlay entries with nonzero weight.
  void for_each_slot(
      const std::function<void(const RowDigest &, const Slot &)> &fn) const {
    for (uint64_t i = 0; i < flat_count_; i++) {
      const RowDigest d = flat_digest(i);
      if (index_.count(d) > 0) {
        continue; // overlay overrides (possibly a tombstone)
      }
      fn(d, flat_slot(i));
    }
    for (const auto &[d, slot] : index_) {
      if (slot.weight != 0) {
        fn(d, slot);
      }
    }
  }

  void flat_unmap() {
    if (flat_map_ != nullptr) {
      ::munmap(flat_map_, flat_map_len_);
      flat_map_ = nullptr;
    }
    if (flat_fd_ >= 0) {
      ::close(flat_fd_);
      flat_fd_ = -1;
    }
    flat_entries_ = nullptr;
    flat_count_ = 0;
    flat_map_len_ = 0;
    flat_wm_hash_.clear();
    flat_wm_count_ = 0;
    overlay_new_ = 0;
    overlay_dead_ = 0;
    // The disk pair no longer matches live state (rebuild swap replaces
    // the log wholesale) — a same-watermark save must NOT skip.
    saved_wm_count_ = -1;
    saved_wm_hash_.clear();
  }

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
    return add_serialized(bytes, w);
  }

  // Same, for a row the caller already serialized (vectorized scan path:
  // bytes come straight from chunk vectors, no per-cell Value boxing).
  RowDigest add_serialized(const std::vector<uint8_t> &bytes, int64_t w = 1) {
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

  // Install the new generation WITHOUT diffing (fresh baselines, deferred
  // installs — anywhere the caller discards the delta). end_rebuild with
  // no-op callbacks still reads every added row's payload back from disk
  // just to hand it to the callback: at 144M rows that is hours of
  // record reads for nothing (bounded-RAM Phase 5).
  void install_rebuild() {
    std::fflush(new_file_);
    swap_in_pending();
  }

  // Diff the new generation against the old one. `on_added` fires with
  // the row and positive weight for rows gaining weight; `on_removed`
  // with the reconstructed row and positive weight for rows losing it.
  // Then the new generation replaces the old (atomic rename) — UNLESS the
  // diff is empty: a no-change sync must be a NO-OP on state. Swapping
  // anyway would rewrite the log and drop the adopted flat layer, the
  // saved-watermark skip, and any delta-sidecar chain — an unattributable
  // read-only commit (fallback sync_all) used to silently destroy all
  // three, forcing full sidecar refolds at the next save.
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
      const int64_t old_w = live_weight(d);
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
    bool any_removed = false;
    if (file_ == nullptr && !path_.empty() && !empty()) {
      file_ = open_file(path_, "rb");
    }
    for_each_slot([&](const RowDigest &d, const Slot &slot) {
      auto it = pending_.find(d);
      const int64_t new_w = it == pending_.end() ? 0 : it->second.weight;
      if (slot.weight > new_w) {
        any_removed = true;
        on_removed(read_row(file_, slot), slot.weight - new_w);
      }
    });

    if (added.empty() && !any_removed) {
      discard_rebuild();
      return;
    }
    swap_in_pending();
  }

  // Throw away an in-flight rebuild, keeping the current generation (log,
  // flat layer, overlay, saved-watermark state) untouched.
  void discard_rebuild() {
    if (new_file_) {
      std::fclose(new_file_);
      new_file_ = nullptr;
    }
    pending_.clear();
    std::error_code ec;
    std::filesystem::remove(tmp_path(), ec);
  }

  // ---- point mutations (captured-delta commits, manual CDC) ------------
  // Appends to the current file; net-zero rows keep a dead record on disk
  // until the next full rebuild compacts it.

  void apply_row(const std::vector<duckdb::Value> &row, int64_t w) {
    std::vector<uint8_t> bytes;
    serialize_row(row, bytes);
    const RowDigest d = digest_bytes(bytes.data(), bytes.size());
    auto it = index_.find(d);
    Slot flat;
    const bool in_overlay = it != index_.end();
    const bool in_flat = !in_overlay && flat_find(d, flat);
    const int64_t cur_w = in_overlay ? it->second.weight
                          : in_flat  ? flat.weight
                                     : 0;
    if (cur_w == 0 && !in_flat && w == 0) {
      return;
    }
    if (cur_w == 0 && !in_flat && !in_overlay) {
      // brand new row: append payload, land it in the overlay/full map.
      // NO per-row fflush: a 5M-row captured delta paid one flush syscall
      // per row (measured 41min for a 5.1M-row UPDATE at 36M). Buffered
      // writes are safe — every read path fseeks first (read_row; C stdio
      // requires it between write and read anyway), reopen_read and
      // save_index flush before observing the file, and durability never
      // depended on the log (DuckDB storage is the only durable source;
      // save_index fsyncs before recording the size it trusts).
      ensure_append_file();
      Slot slot;
      slot.offset = append_offset_;
      slot.length = static_cast<uint32_t>(bytes.size());
      slot.weight = w;
      write_record(file_, bytes);
      append_offset_ += sizeof(uint32_t) + bytes.size();
      index_.emplace(d, slot);
      if (flat_entries_ != nullptr) {
        overlay_new_++;
      }
    } else {
      Slot next = in_overlay ? it->second : flat;
      const int64_t new_w = next.weight + w;
      next.weight = new_w;
      if (flat_entries_ != nullptr) {
        // Overlay is ABSOLUTE and masks the flat entry; weight 0 stays as
        // a tombstone when the row exists in the flat layer. An overlay
        // entry at weight 0 is by construction a flat-origin tombstone
        // (overlay-born rows are erased at 0, never tombstoned).
        const bool tombstone = in_overlay && cur_w == 0;
        const bool flat_origin = in_flat || tombstone;
        const bool was_live = cur_w != 0;
        const bool now_live = new_w != 0;
        if (flat_origin) {
          if (was_live && !now_live) {
            overlay_dead_++;
          } else if (!was_live && now_live) {
            overlay_dead_--; // resurrection of a tombstoned flat row
          }
          index_[d] = next;
        } else {
          // overlay-born row
          if (was_live && !now_live) {
            overlay_new_--;
            index_.erase(it);
          } else {
            index_[d] = next;
          }
        }
      } else {
        if (new_w == 0) {
          index_.erase(it);
        } else {
          it->second = next;
        }
      }
    }
    total_weight_ += w;
  }

  // ---- streaming read (view init / arrangement backfill) ---------------
  void scan(const std::function<void(const std::vector<duckdb::Value> &,
                                     int64_t)> &fn) {
    if (empty()) {
      return;
    }
    reopen_read();
    // Sequential order: sort slots by offset. COPIES, not pointers — a
    // pointer sort's comparator chases scattered hash-map nodes (36M rows
    // = minutes of cache misses under memory pressure, measured); the POD
    // copy sorts contiguously in seconds.
    std::vector<Slot> slots;
    slots.reserve(distinct_rows());
    for_each_slot([&](const RowDigest &, const Slot &slot) {
      slots.push_back(slot);
    });
    std::sort(slots.begin(), slots.end(),
              [](const Slot &a, const Slot &b) {
                return a.offset < b.offset;
              });
    for (const Slot &slot : slots) {
      fn(read_row(file_, slot), slot.weight);
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
    flat_unmap();
    index_.clear();
    pending_.clear();
    total_weight_ = 0;
    std::error_code ec;
    // Durable mode keeps the log (+ index sidecar) across process exit —
    // that is the whole point; a stale pair is rejected by the watermark
    // + file-size check on the next adopt. The in-flight tmp file is
    // always garbage.
    if (!keep_files_) {
      std::filesystem::remove(path_, ec);
      std::filesystem::remove(idx_path(), ec);
      std::filesystem::remove(idx_delta_path(), ec);
    }
    std::filesystem::remove(tmp_path(), ec);
    std::filesystem::remove(idx_delta_path() + ".tmp", ec);
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
    // empty() spans the flat layer too: after a v2 adopt the overlay map
    // is empty while the log holds every row — "wb+" would truncate it.
    file_ = open_file(path_, empty() ? "wb+" : "ab+");
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
    // A fresh generation supersedes any adopted flat layer entirely.
    flat_unmap();
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
  bool keep_files_ = false;       // durable mode: survive process exit
  // Flat mmap layer (v2 adopt): immutable sorted entries; index_ becomes
  // the mutation overlay while mapped.
  int flat_fd_ = -1;
  void *flat_map_ = nullptr;
  size_t flat_map_len_ = 0;
  const uint8_t *flat_entries_ = nullptr;
  uint64_t flat_count_ = 0;
  int64_t flat_wm_count_ = 0;
  std::string flat_wm_hash_;
  // Watermark of the last successful save_index (full or delta): a save
  // asked to persist the same content again skips for free. -1 = never.
  // Reset whenever the on-disk pair goes stale (flat_unmap: rebuild swap,
  // discard) — a rewritten log invalidates saved offsets even when the
  // content watermark is identical.
  int64_t saved_wm_count_ = -1;
  std::string saved_wm_hash_;
  size_t overlay_new_ = 0;  // overlay-born live rows
  size_t overlay_dead_ = 0; // flat rows tombstoned by the overlay
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

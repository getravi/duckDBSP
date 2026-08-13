// dbsp_window_rows.hpp — packed row storage for NativeWindowView partition
// state (packed-window-rows work unit).
//
// A window view's per-partition state (ordered source rows + rendered-output
// cache) was the last deliberately-boxed operator-state class: vectors of
// DuckDBRow at ~300-600B resident per row, whose Value-tree destruction
// dominated circuit teardown and whose bytes dominated the window share of
// RAM accounting. WindowRowStore keeps those rows as packed byte strings
// (dbsp_packed_row.hpp codec) in one arena per store with a positional slot
// directory: ~10x smaller at rest, teardown is two frees, and checkpoint
// save/restore of a packed store is a memcpy instead of a per-row
// Value-tree encode/decode.
//
// The incremental fast path stays O(affected): binary searches decode only
// the sort columns of probed rows (single arena walk per probe), and
// WindowRowsView memoizes full-row decodes so a render pass decodes each
// touched row at most once. Rows whose values the codec cannot represent
// (DECIMAL/HUGEINT/INTERVAL/nested) keep the boxed path: each store decides
// its mode on the first row it stores and flips itself to boxed mid-stream
// if a later row fails to encode, so coverage is an optimization knob, never
// a correctness question (same contract as the packed join indexes).
//
// Decoded rows are hash-seeded via hash_row_fast, which is bit-identical to
// the lazy ColumnVec::hash for every codec-supported type — a decoded row
// therefore hashes and compares equal to the boxed row it was encoded from,
// which is what makes retract-by-equality against result_/delta_ sound.

#pragma once

#include "dbsp_checkpoint.hpp" // BlobWriter/BlobReader, hash_row_fast
#include "dbsp_duckdb_types.hpp" // DuckDBRow, NativeSortView::SortColumn
#include "dbsp_packed_row.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dbsp_native {

namespace windowrows {

// In-blob marker distinguishing the packed window checkpoint layout from the
// legacy per-row layout (whose first u64 is a partition count, far below
// this). New readers accept both; an old reader hitting a packed blob throws
// on the absurd count and reports restore failure -> full rebuild, the
// standard degradation path.
constexpr uint64_t kWindowPackedMagic = 0xDB5BF1A7317D0A15ULL;

// Skip one packed value, returning false on malformed/truncated bytes.
inline bool skip_value(const char *&p, const char *end) {
  if (p >= end) {
    return false;
  }
  const auto t = static_cast<packed::Tag>(static_cast<uint8_t>(*p++));
  size_t n = 0;
  switch (t) {
  case packed::T_NULL:
  case packed::T_I8:
    n = 1;
    break;
  case packed::T_BOOL_F:
  case packed::T_BOOL_T:
    n = 0;
    break;
  case packed::T_I16:
    n = 2;
    break;
  case packed::T_I32:
  case packed::T_U32:
  case packed::T_FLOAT:
  case packed::T_DATE:
    n = 4;
    break;
  case packed::T_I64:
  case packed::T_U64:
  case packed::T_DOUBLE:
  case packed::T_TIMESTAMP:
    n = 8;
    break;
  case packed::T_VARCHAR: {
    if (p + 4 > end) {
      return false;
    }
    n = packed::get_raw<uint32_t>(p); // advances p past the length prefix
    break;
  }
  default:
    return false;
  }
  if (p + n > end) {
    return false;
  }
  p += n;
  return true;
}

// Ordered row container for one window partition (or its rendered-output
// cache): packed arena + positional slot directory, with a boxed fallback.
// Mutations are positional — the OWNER maintains sort order; this class only
// implements the ORDER BY comparator for its binary searches.
class WindowRowStore {
public:
  using SortColumn = NativeSortView::SortColumn;

  WindowRowStore() = default;
  explicit WindowRowStore(std::vector<SortColumn> sort_cols)
      : sort_cols_(std::move(sort_cols)) {
    for (const auto &sc : sort_cols_) {
      max_sort_col_ = std::max(max_sort_col_, sc.column_idx);
    }
  }

  size_t size() const {
    return mode_ == Mode::BOXED ? rows_.size() : slots_.size();
  }
  bool empty() const { return size() == 0; }
  bool packed() const { return mode_ == Mode::PACKED; }
  // Packed layout is checkpointable as raw bytes; an empty store has nothing
  // boxed in it either way.
  bool packed_or_empty() const { return mode_ != Mode::BOXED || rows_.empty(); }

  // BOXED mode only: zero-copy access for the render paths.
  const DuckDBRow &boxed_at(size_t i) const { return rows_[i]; }

  // Any mode. Packed decode seeds the hash cache (hash_row_fast) so the row
  // is immediately usable as a Z-set key for retraction.
  DuckDBRow row_at(size_t i) const {
    if (mode_ == Mode::BOXED) {
      return rows_[i];
    }
    return decode_slot(i);
  }

  void push_back(const DuckDBRow &r) {
    if (!try_store_packed(r, slots_.size())) {
      rows_.push_back(r);
    }
  }

  void insert_at(size_t pos, const DuckDBRow &r) {
    if (!try_store_packed(r, pos)) {
      rows_.insert(rows_.begin() + pos, r);
    }
  }

  void erase_at(size_t pos) {
    if (mode_ == Mode::PACKED) {
      dead_ += slots_[pos].len;
      slots_.erase(slots_.begin() + pos);
      maybe_compact();
    } else {
      rows_.erase(rows_.begin() + pos);
    }
  }

  void overwrite_at(size_t pos, const DuckDBRow &r) {
    if (mode_ == Mode::PACKED) {
      std::string bytes;
      if (packed::encode_row(bytes, r)) {
        dead_ += slots_[pos].len;
        slots_[pos] = append_arena(bytes);
        maybe_compact();
        return;
      }
      flip_to_boxed();
    }
    if (mode_ == Mode::UNDECIDED) { // empty store: decide via append
      push_back(r);
      return;
    }
    rows_[pos] = r;
  }

  void clear() {
    mode_ = Mode::UNDECIDED;
    arena_.clear();
    arena_.shrink_to_fit();
    slots_.clear();
    slots_.shrink_to_fit();
    dead_ = 0;
    rows_.clear();
    rows_.shrink_to_fit();
  }

  // Three-way ORDER BY comparison of stored row i vs a probe row, replicating
  // the boxed RowComparator exactly (missing column -> skip; NULLs peer with
  // NULLs; nulls_first / ascending flags per column). Sort columns whose
  // stored tag matches the probe's type compare straight from the packed
  // bytes; anything else decodes one Value.
  int cmp_sort(size_t i, const DuckDBRow &probe) const {
    if (mode_ == Mode::BOXED) {
      return cmp_sort_rows(rows_[i], probe, sort_cols_);
    }
    const char *p = arena_.data() + slots_[i].off;
    const char *end = p + slots_[i].len;
    const uint32_t ncols = packed::get_raw<uint32_t>(p);
    // One walk collecting column byte offsets, stopping after the last
    // column any sort key needs.
    const uint32_t need =
        std::min<uint32_t>(ncols, static_cast<uint32_t>(max_sort_col_ + 1));
    col_starts_.assign(ncols, nullptr);
    for (uint32_t c = 0; c < need; c++) {
      col_starts_[c] = p;
      if (!skip_value(p, end)) {
        col_starts_.resize(c); // malformed tail: treat as missing columns
        break;
      }
    }
    for (const auto &sc : sort_cols_) {
      if (sc.column_idx >= col_starts_.size() ||
          sc.column_idx >= probe.columns.size() ||
          col_starts_[sc.column_idx] == nullptr) {
        continue;
      }
      const int c = cmp_packed_vs_value(col_starts_[sc.column_idx],
                                        probe.columns[sc.column_idx], sc);
      if (c != 0) {
        return c;
      }
    }
    return 0;
  }

  static int cmp_sort_rows(const DuckDBRow &a, const DuckDBRow &b,
                           const std::vector<SortColumn> &sort_cols) {
    for (const auto &sc : sort_cols) {
      if (sc.column_idx >= a.columns.size() ||
          sc.column_idx >= b.columns.size()) {
        continue;
      }
      const int c =
          cmp_values(a.columns[sc.column_idx], b.columns[sc.column_idx], sc);
      if (c != 0) {
        return c;
      }
    }
    return 0;
  }

  // First index whose row does not sort before `probe`.
  size_t lower_bound(const DuckDBRow &probe) const {
    size_t lo = 0, hi = size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (cmp_sort(mid, probe) < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

  // First index whose row sorts after `probe`.
  size_t upper_bound(const DuckDBRow &probe) const {
    size_t lo = 0, hi = size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (cmp_sort(mid, probe) <= 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

  // Full-row equality against a probe, with DuckDBRow::operator== semantics
  // (decode-and-compare: byte equality would diverge on mixed-type NULLs).
  bool equal_at(size_t i, const DuckDBRow &probe) const {
    if (mode_ == Mode::BOXED) {
      return rows_[i] == probe;
    }
    return decode_slot(i, /*seed_hash=*/false) == probe;
  }

  // Resident bytes for StateBytes::window accounting.
  size_t account(StateAccounting &acct) const {
    if (mode_ != Mode::BOXED) {
      return arena_.capacity() + slots_.capacity() * sizeof(Slot) + 64;
    }
    size_t b = 0;
    for (const auto &r : rows_) {
      b += acct.row_bytes(r);
    }
    return b;
  }

  // --- checkpoint I/O ----------------------------------------------------
  // Packed layout per store: bytes(u32 lens in order) + bytes(concatenated
  // row bytes in order) — save compacts for free, restore is two bulk reads
  // and zero Value construction. REQUIRES packed_or_empty().
  void serialize_packed(BlobWriter &w) const {
    lens_tmp_.clear();
    size_t live = 0;
    for (const auto &s : slots_) {
      lens_tmp_.push_back(s.len);
      live += s.len;
    }
    w.bytes(reinterpret_cast<const uint8_t *>(lens_tmp_.data()),
            lens_tmp_.size() * sizeof(uint32_t));
    bytes_tmp_.clear();
    bytes_tmp_.reserve(live);
    for (const auto &s : slots_) {
      bytes_tmp_.append(arena_, s.off, s.len);
    }
    w.bytes(reinterpret_cast<const uint8_t *>(bytes_tmp_.data()),
            bytes_tmp_.size());
  }

  bool restore_packed(BlobReader &r, uint64_t n_rows) {
    const std::string lens_blob = r.byte_string();
    std::string data = r.byte_string();
    if (lens_blob.size() != n_rows * sizeof(uint32_t)) {
      return false;
    }
    clear();
    slots_.reserve(n_rows);
    uint64_t off = 0;
    const auto *lens = reinterpret_cast<const uint32_t *>(lens_blob.data());
    for (uint64_t i = 0; i < n_rows; i++) {
      slots_.push_back(Slot{off, lens[i]});
      off += lens[i];
    }
    if (off != data.size()) {
      clear();
      return false;
    }
    arena_ = std::move(data);
    mode_ = n_rows > 0 ? Mode::PACKED : Mode::UNDECIDED;
    return true;
  }

private:
  enum class Mode : uint8_t { UNDECIDED, PACKED, BOXED };

  struct Slot {
    uint64_t off;
    uint32_t len;
  };

  // Raw three-way compare of one packed value against a probe Value, for the
  // codec types whose ordering is a plain scalar/byte comparison. NaN doubles
  // follow DuckDB's total order (NaN == NaN, NaN > everything else). Falls
  // back to a one-Value decode for tag/type combinations outside the fast
  // lanes (mixed numeric widths, etc.).
  int cmp_packed_vs_value(const char *vp, const duckdb::Value &b,
                          const SortColumn &sc) const {
    using duckdb::LogicalTypeId;
    const char *p = vp;
    const auto t = static_cast<packed::Tag>(static_cast<uint8_t>(*p++));
    const bool a_null = (t == packed::T_NULL);
    const bool b_null = b.IsNull();
    if (a_null || b_null) {
      if (a_null && b_null) {
        return 0;
      }
      const int a_first = sc.nulls_first ? -1 : 1;
      return a_null ? a_first : -a_first;
    }
    const auto bt = b.type().id();
    int c = 2; // sentinel: no fast lane
    switch (t) {
    case packed::T_I32:
      if (bt == LogicalTypeId::INTEGER) {
        c = three_way(packed::get_raw<int32_t>(p), duckdb::IntegerValue::Get(b));
      }
      break;
    case packed::T_I64:
      if (bt == LogicalTypeId::BIGINT) {
        c = three_way(packed::get_raw<int64_t>(p), duckdb::BigIntValue::Get(b));
      }
      break;
    case packed::T_DOUBLE:
      if (bt == LogicalTypeId::DOUBLE) {
        c = three_way_float(packed::get_raw<double>(p),
                            duckdb::DoubleValue::Get(b));
      }
      break;
    case packed::T_FLOAT:
      if (bt == LogicalTypeId::FLOAT) {
        c = three_way_float(packed::get_raw<float>(p),
                            duckdb::FloatValue::Get(b));
      }
      break;
    case packed::T_DATE:
      if (bt == LogicalTypeId::DATE) {
        c = three_way(packed::get_raw<int32_t>(p),
                      duckdb::DateValue::Get(b).days);
      }
      break;
    case packed::T_TIMESTAMP:
      if (bt == LogicalTypeId::TIMESTAMP) {
        c = three_way(packed::get_raw<int64_t>(p),
                      duckdb::TimestampValue::Get(b).value);
      }
      break;
    case packed::T_VARCHAR:
      if (bt == LogicalTypeId::VARCHAR) {
        const uint32_t n = packed::get_raw<uint32_t>(p);
        const auto &s = duckdb::StringValue::Get(b);
        c = three_way(std::string_view(p, n), std::string_view(s));
      }
      break;
    default:
      break;
    }
    if (c == 2) { // no fast lane: decode and use Value comparison
      const char *dp = vp;
      const duckdb::Value a = packed::decode_value(dp);
      return cmp_values(a, b, sc);
    }
    if (c == 0) {
      return 0;
    }
    if (!sc.ascending) {
      c = -c;
    }
    return c;
  }

  template <typename T> static int three_way(const T &a, const T &b) {
    if (a < b) {
      return -1;
    }
    return b < a ? 1 : 0;
  }

  // DuckDB float total order: NaN peers with NaN and sorts above all values.
  template <typename T> static int three_way_float(T a, T b) {
    const bool an = std::isnan(a), bn = std::isnan(b);
    if (an || bn) {
      if (an && bn) {
        return 0;
      }
      return an ? 1 : -1;
    }
    return three_way(a, b);
  }

  // -1 / 0 / +1 for one sort column with the RowComparator's exact rules.
  static int cmp_values(const duckdb::Value &a, const duckdb::Value &b,
                        const SortColumn &sc) {
    const bool a_null = a.IsNull();
    const bool b_null = b.IsNull();
    if (a_null && b_null) {
      return 0;
    }
    if (a_null) {
      return sc.nulls_first ? -1 : 1;
    }
    if (b_null) {
      return sc.nulls_first ? 1 : -1;
    }
    if (a == b) {
      return 0;
    }
    const bool less = a < b;
    if (sc.ascending) {
      return less ? -1 : 1;
    }
    return less ? 1 : -1;
  }

  DuckDBRow decode_slot(size_t i, bool seed_hash = true) const {
    const char *p = arena_.data() + slots_[i].off;
    const uint32_t n = packed::get_raw<uint32_t>(p);
    std::vector<duckdb::Value> vals;
    vals.reserve(n);
    for (uint32_t c = 0; c < n; c++) {
      vals.push_back(packed::decode_value(p));
    }
    DuckDBRow out;
    const size_t h = seed_hash ? hash_row_fast(vals) : 0;
    out.columns.assign(std::move(vals));
    if (h != 0) {
      out.columns.set_hash(h);
    }
    return out;
  }

  Slot append_arena(const std::string &bytes) {
    const Slot s{arena_.size(), static_cast<uint32_t>(bytes.size())};
    arena_.append(bytes);
    return s;
  }

  // Store `r` packed at slot position `pos` if this store is (or becomes)
  // packed. Returns false when the caller must store boxed instead.
  bool try_store_packed(const DuckDBRow &r, size_t pos) {
    if (mode_ == Mode::BOXED) {
      return false;
    }
    std::string bytes;
    if (packed::encode_row(bytes, r)) {
      mode_ = Mode::PACKED;
      slots_.insert(slots_.begin() + pos, append_arena(bytes));
      return true;
    }
    flip_to_boxed();
    return false;
  }

  // A row the codec can't represent: decode everything stored so far and
  // continue boxed. Rare (type-level), never partial.
  void flip_to_boxed() {
    std::vector<DuckDBRow> boxed;
    boxed.reserve(slots_.size());
    for (size_t i = 0; i < slots_.size(); i++) {
      boxed.push_back(decode_slot(i));
    }
    rows_ = std::move(boxed);
    arena_.clear();
    arena_.shrink_to_fit();
    slots_.clear();
    slots_.shrink_to_fit();
    dead_ = 0;
    mode_ = Mode::BOXED;
  }

  void maybe_compact() {
    if (arena_.size() < 4096 || dead_ * 2 < arena_.size()) {
      return;
    }
    std::string next;
    next.reserve(arena_.size() - dead_);
    for (auto &s : slots_) {
      const uint64_t off = next.size();
      next.append(arena_, s.off, s.len);
      s.off = off;
    }
    arena_ = std::move(next);
    dead_ = 0;
  }

  Mode mode_ = Mode::UNDECIDED;
  std::vector<SortColumn> sort_cols_;
  size_t max_sort_col_ = 0;
  std::string arena_;
  std::vector<Slot> slots_;
  uint64_t dead_ = 0;
  std::vector<DuckDBRow> rows_; // BOXED mode
  // Scratch (per-instance, single-threaded like all view state).
  mutable std::vector<const char *> col_starts_;
  mutable std::vector<uint32_t> lens_tmp_;
  mutable std::string bytes_tmp_;
};

// Read adapter the render paths use: boxed stores pass references through;
// packed stores decode each accessed row at most once per view instance.
// `dense` sizes the memo for whole-partition access (full re-render);
// the sparse default keeps O(affected) fast paths from paying an
// O(partition) allocation just to touch a handful of rows.
class WindowRowsView {
public:
  explicit WindowRowsView(const WindowRowStore &s, bool dense = false)
      : s_(s), dense_(dense) {}
  size_t size() const { return s_.size(); }
  bool empty() const { return s_.size() == 0; }
  const DuckDBRow &operator[](size_t i) const {
    if (!s_.packed()) {
      return s_.boxed_at(i);
    }
    if (dense_) {
      if (cache_.size() != s_.size()) {
        cache_.resize(s_.size());
      }
      DuckDBRow &slot = cache_[i];
      if (slot.columns.payload_id() == nullptr) { // decoded rows have one
        slot = s_.row_at(i);
      }
      return slot;
    }
    auto ins = sparse_.try_emplace(i);
    if (ins.second) {
      ins.first->second = s_.row_at(i);
    }
    return ins.first->second;
  }

private:
  const WindowRowStore &s_;
  bool dense_;
  mutable std::vector<DuckDBRow> cache_;
  mutable std::unordered_map<size_t, DuckDBRow> sparse_;
};

} // namespace windowrows

} // namespace dbsp_native

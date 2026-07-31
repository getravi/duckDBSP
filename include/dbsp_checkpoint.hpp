#pragma once

// Circuit-state checkpointing (Phase D3b).
//
// A checkpoint captures everything a view needs to resume incremental
// maintenance without replaying its sources: per-node operator state
// (aggregate groups, sink materialization), shared join arrangements, and
// tracked-table baselines. Blobs are opaque byte strings written to the
// _dbsp_ckpt table by CDCManager::save_checkpoint and read back by
// load_from_duck_table's fast path.
//
// Any node kind without checkpoint support marks its whole view
// non-checkpointable; such views fall back to the normal rebuild-by-replay
// on load. Correctness never depends on a checkpoint being present or
// fresh: a stale checkpoint (source tables changed since save) is detected
// by row-count watermarks and discarded; a checkpoint whose view
// *definition* has changed since save (e.g. via dbsp_replace_view) is
// detected per-view by a SQL fingerprint (v3, see kDbspCkptFormatVersion)
// and discarded for that view only.

#include "dbsp_spill_store.hpp" // serialize_row / deserialize_row

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dbsp_native {

// Checkpoint blob format version. Bump whenever a node's serialize_state
// layout changes shape (e.g. new fields appended) so a blob written under
// an older layout is detected and discarded rather than fed to the new
// restore_state, which would misparse it. CDCManager::save_checkpoint
// writes this into a dedicated _dbsp_ckpt_version table; checkpoint_valid
// rejects the whole checkpoint (treats it as absent -> rebuild-by-replay)
// when that table is missing (pre-bump blob) or its value differs.
//
// v2 (Task 3, durability-ergonomics): PlanJoinNode::serialize_state gained
// left_pad_/right_pad_ for LEFT/RIGHT outer-join pad bookkeeping.
//
// v3 (durability-ergonomics fix-wave, Finding 1): the checkpoint now also
// carries a per-view SQL fingerprint (a `kind='sql'` row in _dbsp_ckpt,
// alongside each view's `node`/`sink` rows) recording the view's exact
// definition at save time. Without it, a checkpoint saved under one
// definition could be silently replayed onto a *different* one — e.g.
// dbsp_replace_view()/CREATE OR REPLACE swaps a view's SQL, then an
// unclean close (crash, autopersist off, a failed auto-save) skips the
// next save_checkpoint(), leaving stale node/sink blobs on disk next to a
// _dbsp_views row that already carries the new SQL. On load,
// checkpoint_valid compares the stored fingerprint against the SQL about
// to be used to cold-create the view; a mismatch declines the checkpoint
// fast path for *that view only* (rebuild-by-replay), leaving every other
// view's restore unaffected. The version bump means any v2 checkpoint
// (which has no fingerprint rows at all) is treated as wholesale absent,
// same one-time-rebuild cost as prior bumps.
// v4 (cold/restore attack, Task 2): PlanAggregateNode::serialize_state
// gained the per-group `values` multiset for plain (non-DISTINCT) MIN/MAX,
// so those views now report SERIALIZABLE instead of UNSUPPORTED. A v3
// checkpoint has no such field for any group and would misparse under the
// new restore_state layout, so it is treated as absent (one-time
// rebuild-by-replay, same cost as prior bumps).
constexpr int64_t kDbspCkptFormatVersion = 4;

// How a circuit node participates in checkpointing.
enum class CkptSupport {
  STATELESS,    // nothing to save; restore is a no-op
  SERIALIZABLE, // implements serialize_state / restore_state
  UNSUPPORTED,  // view must be rebuilt by replay
};

// Little-endian fixed-width writer/reader for node state blobs. Values go
// through serialize_row (DuckDB BinarySerializer), scalars through these.
class BlobWriter {
public:
  void u64(uint64_t v) { raw(&v, sizeof(v)); }
  void i64(int64_t v) { raw(&v, sizeof(v)); }
  void f64(double v) { raw(&v, sizeof(v)); }

  // Accepts std::vector<Value> or DuckDBRow's ColumnVec (index/size API)
  template <typename Container> void row(const Container &values) {
    std::vector<duckdb::Value> vals;
    vals.reserve(values.size());
    for (size_t i = 0; i < values.size(); i++) {
      vals.push_back(values[i]);
    }
    std::vector<uint8_t> tmp;
    serialize_row(vals, tmp);
    u64(tmp.size());
    raw(tmp.data(), tmp.size());
  }

  std::vector<uint8_t> take() { return std::move(bytes_); }

private:
  void raw(const void *p, size_t n) {
    const auto *b = static_cast<const uint8_t *>(p);
    bytes_.insert(bytes_.end(), b, b + n);
  }
  std::vector<uint8_t> bytes_;
};

class BlobReader {
public:
  BlobReader(const uint8_t *data, size_t len) : data_(data), len_(len) {}

  uint64_t u64() { return fixed<uint64_t>(); }
  int64_t i64() { return fixed<int64_t>(); }
  double f64() { return fixed<double>(); }

  std::vector<duckdb::Value> row() {
    const uint64_t n = u64();
    if (pos_ + n > len_) {
      throw std::runtime_error("checkpoint blob truncated (row)");
    }
    auto vals = deserialize_row(data_ + pos_, n);
    pos_ += n;
    return vals;
  }

  bool done() const { return pos_ == len_; }

private:
  template <typename T> T fixed() {
    if (pos_ + sizeof(T) > len_) {
      throw std::runtime_error("checkpoint blob truncated (scalar)");
    }
    T v;
    std::memcpy(&v, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return v;
  }

  const uint8_t *data_;
  size_t len_;
  size_t pos_ = 0;
};

} // namespace dbsp_native

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

#include "dbsp_duckdb_types.hpp" // DuckDBRow / ColumnVec::kNullHash
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
// v5 (Task 1, restore-tail): PlanRecursiveNode::serialize_state gained
// accumulated_/anchor_total_/base_totals_ plus the nested step_view_
// circuit's own per-node blobs (embedded as a length-prefixed sub-blob
// keyed by inner node id), so a UNION ALL recursive view whose step circuit
// is itself wholly checkpointable now reports SERIALIZABLE instead of
// UNSUPPORTED. A v4 checkpoint carries no node blob at all for a recursive
// node (the node was UNSUPPORTED then, so save_checkpoint never wrote one)
// and would misparse under the new restore_state layout, so it is treated
// as absent (one-time rebuild-by-replay, same cost as prior bumps).
//
// Task 2 (restore-tail) did NOT bump this: EmbeddedViewNode gained
// circuit_state_kind()/serialize_circuit_node_state()/
// restore_circuit_node_state() (dbsp_duckdb_types.hpp), letting a
// NativeWindowView embedded view report SERIALIZABLE instead of
// UNSUPPORTED — but EmbeddedViewNode never wrote a node blob under any
// prior version (it was unconditionally UNSUPPORTED), so an existing v5
// checkpoint simply has no blob keyed by that node's id, not a
// differently-shaped one. restore_circuit_state's per-node loop already
// treats a missing key as a decline for that node (blobs.find(n.id()) ==
// end()), gracefully falling back to rebuild-by-replay for that view
// alone — no whole-checkpoint invalidation needed for a node kind with no
// prior blob to misparse.
constexpr int64_t kDbspCkptFormatVersion = 5;

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

// Row hash matching ColumnVec::hash()/fold_vector_hashes bit-for-bit, computed
// from already-decoded Values without paying Value::Hash()'s cost (a fresh
// 1-element Vector + VectorOperations::Hash dispatch per column — the "lazy
// per-Value hashing" dbsp_engine_hook.hpp's own comment calls out as ~2/3 of
// chunk-ingestion time). Every checkpoint-restored row becomes a hash-map key
// (aggregate states_, join Index/RowWeights) or a DuckDBZSet entry, so a
// restore built entirely from `row()` pays that lazy cost once per row on
// first use — and a checkpointed view with many groups/sink rows spends most
// of "blob_decode" there, not in the byte-level codec (already shared with
// the spill path, see serialize_row/deserialize_row above).
//
// For the codec's typed fast-path columns (INTEGER/BIGINT/DOUBLE/VARCHAR/
// BOOLEAN), duckdb::Hash<T> is the exact primitive VectorOperations::Hash
// calls per element for a flat vector of that physical type (verified
// against duckdb/src/common/vector_operations/vector_hash.cpp — HashOp
// dispatches straight to duckdb::Hash<T>, and duckdb::Hash(string_t) is
// documented+asserted equal to duckdb::Hash(ptr, size) for every string,
// inlined or not), so calling it directly here reproduces Value::Hash()
// exactly without the temporary-Vector detour. Fallback-typed and NULL
// values keep behaving exactly as the lazy path already does (NULL ->
// ColumnVec::kNullHash, ColumnVec::hash()'s own convention rather than
// duckdb's; fallback -> v.Hash(), the one case that still pays full price —
// same tradeoff serialize_row's byte-level fallback already makes). The
// combiner is copy-identical to ColumnVec::hash()/fold_vector_hashes so a
// row restored this way hashes the same as an equal-content row a live
// chunk-based path would build; test_join_checkpoint.cpp's "hashed_row
// matches the lazy path" case is the correctness guard for that invariant.
inline size_t hash_row_fast(const std::vector<duckdb::Value> &vals) {
  size_t h = 0;
  for (const auto &v : vals) {
    size_t col_hash;
    if (v.IsNull()) {
      col_hash = ColumnVec::kNullHash;
    } else {
      switch (v.type().id()) {
      case duckdb::LogicalTypeId::INTEGER:
        col_hash = duckdb::Hash<int32_t>(duckdb::IntegerValue::Get(v));
        break;
      case duckdb::LogicalTypeId::BIGINT:
        col_hash = duckdb::Hash<int64_t>(duckdb::BigIntValue::Get(v));
        break;
      case duckdb::LogicalTypeId::DOUBLE:
        col_hash = duckdb::Hash<double>(duckdb::DoubleValue::Get(v));
        break;
      case duckdb::LogicalTypeId::VARCHAR: {
        const auto &s = duckdb::StringValue::Get(v);
        col_hash = duckdb::Hash(s.data(), s.size());
        break;
      }
      case duckdb::LogicalTypeId::BOOLEAN:
        col_hash = duckdb::Hash<bool>(duckdb::BooleanValue::Get(v));
        break;
      default:
        col_hash = v.Hash(); // fallback types: pay the slow-but-correct path
        break;
      }
    }
    h ^= col_hash + 0x9e3779b9 + (h << 6) + (h >> 2);
  }
  return h;
}

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

  // Same bytes as row(), but returns a DuckDBRow with its hash cache
  // pre-seeded (hash_row_fast) instead of left for the first map/Z-set
  // operation to compute lazily. Use this at every checkpoint restore call
  // site that turns a decoded row into a hash-map key or Z-set entry.
  DuckDBRow hashed_row() {
    auto vals = row();
    DuckDBRow out;
    const size_t h = hash_row_fast(vals);
    out.columns.assign(std::move(vals));
    out.columns.set_hash(h);
    return out;
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

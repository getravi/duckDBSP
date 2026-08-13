// Packed window-row storage (dbsp_window_rows.hpp): the packed stores must
// be behaviorally invisible — identical results to a from-scratch render,
// exact retraction across an update cycle (round-tripped rows must hash and
// compare equal to the rows they were encoded from), checkpoint round-trip
// in the packed layout, legacy-layout blobs still restorable, and a boxed
// fallback for codec-unsupported types that keeps every one of those
// properties.
#include "../test_helpers.hpp"
#include "catch.hpp"
#include "dbsp_window_view.hpp"

#include <cmath>

using namespace dbsp_native;
using duckdb::Value;

namespace {

bool zset_equal(const DuckDBZSet &a, const DuckDBZSet &b) {
  if (a.size() != b.size())
    return false;
  for (const auto &[row, w] : a) {
    if (b.get(row) != w)
      return false;
  }
  return true;
}

DuckDBRow src(int part, int ord, Value arg) {
  DuckDBRow r;
  r.columns = {Value::INTEGER(part), Value::INTEGER(ord), arg};
  return r;
}

std::vector<NativeWindowView::WindowDef> lag1_def() {
  NativeWindowView::WindowDef w;
  w.function = "LAG";
  w.partition_indices = {0};
  w.sort_columns = {{1, true, true}};
  w.arg_column_idx = 2;
  w.offset = 1;
  return {w};
}

NativeWindowView make_view(const char *name) {
  return NativeWindowView(name, "", "t", TableSchema{}, TableSchema{},
                          lag1_def());
}

// Mixed-type seed: ints, VARCHAR, NULLs, NaN and -0.0 doubles — the values
// whose hash/equality round-trip the packed codec must preserve exactly.
DuckDBZSet mixed_seed() {
  DuckDBZSet d;
  d.insert(src(1, 0, Value::INTEGER(10)), 1);
  d.insert(src(1, 1, Value(duckdb::LogicalType::INTEGER)), 1); // typed NULL
  d.insert(src(1, 2, Value("hello")), 1);
  d.insert(src(2, 0, Value::DOUBLE(std::nan(""))), 1);
  d.insert(src(2, 1, Value::DOUBLE(-0.0)), 1);
  d.insert(src(2, 2, Value("")), 1); // empty string
  return d;
}

} // namespace

TEST_CASE("packed window: update cycle retracts exactly (mixed types)",
          "[window][packed]") {
  auto v = make_view("p1");
  v.apply_changes("t", mixed_seed());

  // Value update through the fast path: the retraction of the previously
  // rendered rows must cancel exactly (result stays one row per input).
  DuckDBZSet upd;
  upd.insert(src(1, 1, Value(duckdb::LogicalType::INTEGER)), -1);
  upd.insert(src(1, 1, Value::INTEGER(77)), 1);
  v.apply_changes("t", upd);

  // Differential check against a from-scratch render of the net state.
  DuckDBZSet net = mixed_seed();
  net.insert(src(1, 1, Value(duckdb::LogicalType::INTEGER)), -1);
  net.insert(src(1, 1, Value::INTEGER(77)), 1);
  auto full = make_view("p1f");
  full.apply_changes("t", net);
  REQUIRE(zset_equal(v.get_result(), full.get_result()));
  // Exact cancellation: every weight in the result is +1.
  for (const auto &[row, w] : v.get_result()) {
    (void)row;
    REQUIRE(w == 1);
  }
}

TEST_CASE("packed window: checkpoint round-trip (packed layout)",
          "[window][packed][checkpoint]") {
  auto v = make_view("p2");
  v.apply_changes("t", mixed_seed());
  v.drop_delta();

  std::vector<uint8_t> blob;
  v.serialize_circuit_node_state(blob);
  // Packed layout marker (every store here is codec-supported).
  REQUIRE(blob.size() >= 8);
  uint64_t first = 0;
  std::memcpy(&first, blob.data(), 8);
  REQUIRE(first == windowrows::kWindowPackedMagic);

  auto r = make_view("p2r");
  REQUIRE(r.restore_circuit_node_state(blob.data(), blob.size()));
  REQUIRE(zset_equal(r.get_result(), v.get_result()));

  // Post-restore edit: both views must evolve identically (fast path uses
  // the restored partition rows AND the restored output cache).
  DuckDBZSet upd;
  upd.insert(src(1, 2, Value("hello")), -1);
  upd.insert(src(1, 2, Value("world")), 1);
  v.apply_changes("t", upd);
  r.apply_changes("t", upd);
  REQUIRE(zset_equal(r.get_result(), v.get_result()));
  REQUIRE(zset_equal(r.get_delta(), v.get_delta()));
  for (const auto &[row, w] : r.get_result()) {
    (void)row;
    REQUIRE(w == 1);
  }
}

TEST_CASE("packed window: legacy-layout blob restores",
          "[window][packed][checkpoint]") {
  auto v = make_view("p3");
  v.apply_changes("t", mixed_seed());
  v.drop_delta();

  // Hand-write the pre-packed layout: counts + boxed rows, no magic. The
  // reader must take the legacy branch and re-encode into packed stores.
  // (Content mirrors what the old serializer wrote: partitions_ ordered
  // rows, then partition_outputs_ rendered rows == the view's result rows
  // grouped per partition; easiest faithful source is a boxed twin, so
  // build the blob from the view's own state via the legacy writer path:
  // serialize, then decode-reencode is already covered above. Here we
  // construct the legacy bytes directly from known content.)
  BlobWriter w;
  // partitions_: {1: rows(ord 0..2), 2: rows(ord 0..2)} in key order.
  auto key1 = std::vector<Value>{Value::INTEGER(1)};
  auto key2 = std::vector<Value>{Value::INTEGER(2)};
  std::vector<DuckDBRow> p1 = {src(1, 0, Value::INTEGER(10)),
                               src(1, 1, Value(duckdb::LogicalType::INTEGER)),
                               src(1, 2, Value("hello"))};
  std::vector<DuckDBRow> p2 = {src(2, 0, Value::DOUBLE(std::nan(""))),
                               src(2, 1, Value::DOUBLE(-0.0)),
                               src(2, 2, Value(""))};
  auto lag_out = [](const std::vector<DuckDBRow> &rows) {
    std::vector<DuckDBRow> out;
    for (size_t i = 0; i < rows.size(); i++) {
      DuckDBRow o = rows[i];
      o.columns.push_back(i > 0 ? rows[i - 1].columns[2] : Value());
      out.push_back(o);
    }
    return out;
  };
  w.u64(2);
  w.row(key1);
  w.u64(p1.size());
  for (const auto &r0 : p1)
    w.row(r0.columns);
  w.row(key2);
  w.u64(p2.size());
  for (const auto &r0 : p2)
    w.row(r0.columns);
  w.u64(2);
  w.row(key1);
  auto o1 = lag_out(p1);
  w.u64(o1.size());
  for (const auto &r0 : o1)
    w.row(r0.columns);
  w.row(key2);
  auto o2 = lag_out(p2);
  w.u64(o2.size());
  for (const auto &r0 : o2)
    w.row(r0.columns);
  auto blob = w.take();

  auto r = make_view("p3r");
  REQUIRE(r.restore_circuit_node_state(blob.data(), blob.size()));
  REQUIRE(zset_equal(r.get_result(), v.get_result()));

  // Post-restore update must retract the legacy-restored cache exactly.
  DuckDBZSet upd;
  upd.insert(src(2, 1, Value::DOUBLE(-0.0)), -1);
  upd.insert(src(2, 1, Value::DOUBLE(5.0)), 1);
  v.apply_changes("t", upd);
  r.apply_changes("t", upd);
  REQUIRE(zset_equal(r.get_result(), v.get_result()));
  for (const auto &[row, wt] : r.get_result()) {
    (void)row;
    REQUIRE(wt == 1);
  }
}

TEST_CASE("packed window: boxed fallback for codec-unsupported types",
          "[window][packed]") {
  // DECIMAL args can't be packed: stores must fall back to boxed and stay
  // correct through updates and checkpointing (legacy layout).
  auto dec = [](int64_t unscaled) {
    return Value::DECIMAL(unscaled, 10, 2);
  };
  auto v = make_view("p4");
  DuckDBZSet d0;
  for (int o = 0; o < 4; o++)
    d0.insert(src(1, o, dec(100 + o)), 1);
  v.apply_changes("t", d0);

  DuckDBZSet upd;
  upd.insert(src(1, 2, dec(102)), -1);
  upd.insert(src(1, 2, dec(999)), 1);
  v.apply_changes("t", upd);

  DuckDBZSet net = d0;
  net.insert(src(1, 2, dec(102)), -1);
  net.insert(src(1, 2, dec(999)), 1);
  auto full = make_view("p4f");
  full.apply_changes("t", net);
  REQUIRE(zset_equal(v.get_result(), full.get_result()));

  // Serializes via the legacy layout (no packed magic), restores equal.
  v.drop_delta();
  std::vector<uint8_t> blob;
  v.serialize_circuit_node_state(blob);
  uint64_t first = 0;
  std::memcpy(&first, blob.data(), 8);
  REQUIRE(first != windowrows::kWindowPackedMagic);
  auto r = make_view("p4r");
  REQUIRE(r.restore_circuit_node_state(blob.data(), blob.size()));
  REQUIRE(zset_equal(r.get_result(), v.get_result()));
}

TEST_CASE("packed window: structural churn keeps store and cache aligned",
          "[window][packed]") {
  // Insert/delete cycles (structural path) followed by value updates (fast
  // path) — exercises erase_at/insert_at slot bookkeeping and compaction.
  auto v = make_view("p5");
  DuckDBZSet d0;
  for (int o = 0; o < 32; o++)
    d0.insert(src(1, o, Value::INTEGER(o)), 1);
  v.apply_changes("t", d0);

  DuckDBZSet net = d0;
  int cur20 = 20; // current value at ordinal 20
  for (int round = 0; round < 21; round++) {
    DuckDBZSet d;
    if (round % 3 == 0) { // structural: delete an original ordinal
      d.insert(src(1, round, Value::INTEGER(round)), -1);
    } else if (round % 3 == 1) { // structural: brand-new ordinal
      d.insert(src(1, 100 + round, Value::INTEGER(round)), 1);
    } else { // pure value update (fast path)
      d.insert(src(1, 20, Value::INTEGER(cur20)), -1);
      d.insert(src(1, 20, Value::INTEGER(2000 + round)), 1);
      cur20 = 2000 + round;
    }
    for (const auto &[row, wt] : d)
      net.insert(row, wt);
    v.apply_changes("t", d);
  }
  auto full = make_view("p5f");
  full.apply_changes("t", net);
  REQUIRE(zset_equal(v.get_result(), full.get_result()));
}

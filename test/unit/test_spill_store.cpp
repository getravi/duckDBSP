// Phase K1: SpilledBaseline — disk-backed tracked-table baselines.
// Round-trip fidelity (every value type the engine syncs), rebuild diff
// semantics (added/removed/weight changes), point mutations, and
// streaming scans must match what the in-memory baseline would produce.

#include "dbsp_spill_store.hpp"
#include "catch.hpp"

#include <cstdlib>
#include <map>
#include <random>
#include <unistd.h>

using namespace dbsp_native;
using duckdb::Value;

namespace {
std::string temp_spill_path(const char *tag) {
  return std::string("/tmp/dbsp_spill_test_") + tag + "_" +
         std::to_string(getpid()) + ".dbspill";
}
} // namespace

TEST_CASE("spill: row codec round-trips typed values", "[unit][spill]") {
  std::vector<Value> row;
  row.push_back(Value::INTEGER(42));
  row.push_back(Value::BIGINT(-9000000000LL));
  row.push_back(Value("hello 'quoted' \xE2\x9C\x93 world"));
  row.push_back(Value::DOUBLE(3.25));
  row.push_back(Value(duckdb::LogicalType::VARCHAR)); // NULL varchar
  row.push_back(Value::BOOLEAN(true));
  row.push_back(Value::DECIMAL(int64_t(12345), 10, 2));
  row.push_back(Value::DATE(duckdb::date_t(19000)));

  std::vector<uint8_t> bytes;
  serialize_row(row, bytes);
  auto back = deserialize_row(bytes.data(), bytes.size());
  REQUIRE(back.size() == row.size());
  for (size_t i = 0; i < row.size(); i++) {
    INFO("column " << i);
    REQUIRE(back[i].type() == row[i].type());
    if (row[i].IsNull()) {
      REQUIRE(back[i].IsNull());
    } else {
      REQUIRE(back[i] == row[i]);
    }
  }
}

TEST_CASE("spill: digest distinguishes rows, is stable", "[unit][spill]") {
  std::vector<uint8_t> a, b, a2;
  serialize_row({Value::INTEGER(1), Value("x")}, a);
  serialize_row({Value::INTEGER(2), Value("x")}, b);
  serialize_row({Value::INTEGER(1), Value("x")}, a2);
  REQUIRE(digest_bytes(a.data(), a.size()) ==
          digest_bytes(a2.data(), a2.size()));
  REQUIRE_FALSE(digest_bytes(a.data(), a.size()) ==
                digest_bytes(b.data(), b.size()));
}

TEST_CASE("spill: rebuild diff reports adds, removes, weight changes",
          "[unit][spill]") {
  SpilledBaseline store(temp_spill_path("diff"));

  // Generation 1: {A:1, B:2, C:1}
  store.begin_rebuild();
  store.add({Value::INTEGER(1), Value("A")});
  store.add({Value::INTEGER(2), Value("B")});
  store.add({Value::INTEGER(2), Value("B")});
  store.add({Value::INTEGER(3), Value("C")});
  int adds = 0, removes = 0;
  store.end_rebuild(
      [&](const std::vector<Value> &row, int64_t w) { adds += (int)w; },
      [&](const std::vector<Value> &row, int64_t w) { removes += (int)w; });
  REQUIRE(adds == 4);
  REQUIRE(removes == 0);
  REQUIRE(store.distinct_rows() == 3);
  REQUIRE(store.total_weight() == 4);

  // Generation 2: {B:1, C:1, D:1} — A removed, B loses one copy, D new
  store.begin_rebuild();
  store.add({Value::INTEGER(2), Value("B")});
  store.add({Value::INTEGER(3), Value("C")});
  store.add({Value::INTEGER(4), Value("D")});
  std::vector<std::pair<int, int64_t>> added, removed;
  store.end_rebuild(
      [&](const std::vector<Value> &row, int64_t w) {
        added.emplace_back(row[0].GetValue<int>(), w);
      },
      [&](const std::vector<Value> &row, int64_t w) {
        removed.emplace_back(row[0].GetValue<int>(), w);
      });
  REQUIRE(added.size() == 1);
  REQUIRE(added[0] == std::make_pair(4, int64_t(1)));
  REQUIRE(removed.size() == 2);
  std::sort(removed.begin(), removed.end());
  REQUIRE(removed[0] == std::make_pair(1, int64_t(1))); // A gone
  REQUIRE(removed[1] == std::make_pair(2, int64_t(1))); // B one copy
  REQUIRE(store.total_weight() == 3);
}

TEST_CASE("spill: point mutations then scan", "[unit][spill]") {
  SpilledBaseline store(temp_spill_path("mut"));
  store.apply_row({Value::INTEGER(1)}, 1);
  store.apply_row({Value::INTEGER(2)}, 3);
  store.apply_row({Value::INTEGER(1)}, -1); // net zero → gone
  store.apply_row({Value::INTEGER(3)}, 1);

  std::map<int, int64_t> seen;
  store.scan([&](const std::vector<Value> &row, int64_t w) {
    seen[row[0].GetValue<int>()] = w;
  });
  REQUIRE(seen.size() == 2);
  REQUIRE(seen[2] == 3);
  REQUIRE(seen[3] == 1);
  REQUIRE(store.total_weight() == 4);
}

TEST_CASE("spill: rebuild after point mutations compacts dead records",
          "[unit][spill]") {
  SpilledBaseline store(temp_spill_path("compact"));
  store.apply_row({Value("keep")}, 1);
  store.apply_row({Value("drop")}, 1);
  store.apply_row({Value("drop")}, -1);

  store.begin_rebuild();
  store.add({Value("keep")});
  store.add({Value("fresh")});
  std::vector<std::string> added, removed;
  store.end_rebuild(
      [&](const std::vector<Value> &row, int64_t w) {
        added.push_back(row[0].GetValue<std::string>());
      },
      [&](const std::vector<Value> &row, int64_t w) {
        removed.push_back(row[0].GetValue<std::string>());
      });
  REQUIRE(added == std::vector<std::string>{"fresh"});
  REQUIRE(removed.empty()); // "drop" was already net-zero, never reported
  REQUIRE(store.distinct_rows() == 2);
}

TEST_CASE("spill: property test vs in-memory oracle", "[unit][spill]") {
  SpilledBaseline store(temp_spill_path("prop"));
  std::map<int, int64_t> oracle;
  std::mt19937 rng(4242);

  for (int round = 0; round < 30; round++) {
    // Random new generation
    std::map<int, int64_t> next;
    const int n = 1 + (int)(rng() % 40);
    for (int i = 0; i < n; i++) {
      next[(int)(rng() % 25)] += 1;
    }
    store.begin_rebuild();
    for (const auto &[k, w] : next) {
      for (int64_t c = 0; c < w; c++) {
        store.add({Value::INTEGER(k), Value(std::string(1 + k % 3, 'x'))});
      }
    }
    std::map<int, int64_t> delta;
    store.end_rebuild(
        [&](const std::vector<Value> &row, int64_t w) {
          delta[row[0].GetValue<int>()] += w;
        },
        [&](const std::vector<Value> &row, int64_t w) {
          delta[row[0].GetValue<int>()] -= w;
        });
    // Delta must equal next - oracle exactly
    for (const auto &[k, w] : next) {
      auto it = oracle.find(k);
      const int64_t expect = w - (it == oracle.end() ? 0 : it->second);
      INFO("round " << round << " key " << k);
      REQUIRE(delta[k] == expect);
    }
    for (const auto &[k, w] : oracle) {
      if (!next.count(k)) {
        REQUIRE(delta[k] == -w);
      }
    }
    oracle = next;

    // Scan must reproduce the oracle
    std::map<int, int64_t> scanned;
    store.scan([&](const std::vector<Value> &row, int64_t w) {
      scanned[row[0].GetValue<int>()] += w;
    });
    REQUIRE(scanned == oracle);
  }
}

namespace {
RowDigest key_digest_of(int k) {
  std::vector<uint8_t> bytes;
  serialize_row({Value::INTEGER(k)}, bytes);
  return digest_bytes(bytes.data(), bytes.size());
}
} // namespace

TEST_CASE("spill: bucket index probe/update/erase", "[unit][spill]") {
  SpilledBucketIndex idx(temp_spill_path("bidx"), /*cache_capacity=*/2);

  REQUIRE(idx.probe(key_digest_of(1)) == nullptr);

  idx.update(key_digest_of(1), {{{Value("a")}, 2}, {{Value("b")}, 1}});
  idx.update(key_digest_of(2), {{{Value("c")}, 1}});
  idx.update(key_digest_of(3), {{{Value("d")}, 1}}); // evicts key 1 from cache

  const auto *b1 = idx.probe(key_digest_of(1)); // disk reload
  REQUIRE(b1 != nullptr);
  REQUIRE(b1->size() == 2);

  // Merge: b loses its weight, a gains
  idx.update(key_digest_of(1), {{{Value("b")}, -1}, {{Value("a")}, 3}});
  b1 = idx.probe(key_digest_of(1));
  REQUIRE(b1->size() == 1);
  REQUIRE((*b1)[0].second == 5);

  // Empty bucket leaves the index
  idx.update(key_digest_of(2), {{{Value("c")}, -1}});
  REQUIRE(idx.probe(key_digest_of(2)) == nullptr);
  REQUIRE(idx.key_count() == 2);
}

TEST_CASE("spill: bucket index property test vs oracle incl. compaction",
          "[unit][spill]") {
  SpilledBucketIndex idx(temp_spill_path("bprop"), /*cache_capacity=*/4);
  std::map<int, std::map<std::string, int64_t>> oracle;
  std::mt19937 rng(777);

  for (int step = 0; step < 4000; step++) {
    const int k = (int)(rng() % 12);
    // Long payload so compaction thresholds actually trip
    const std::string v(50 + rng() % 100, 'a' + (char)(rng() % 4));
    const int64_t w = (rng() % 3 == 0) ? -1 : 1;
    idx.update(key_digest_of(k), {{{Value(v)}, w}});
    auto &ob = oracle[k];
    ob[v] += w;
    if (ob[v] == 0) {
      ob.erase(v);
    }
    if (ob.empty()) {
      oracle.erase(k);
    }

    if (step % 251 == 0) {
      for (int q = 0; q < 12; q++) {
        const auto *bucket = idx.probe(key_digest_of(q));
        auto oit = oracle.find(q);
        if (oit == oracle.end()) {
          REQUIRE(bucket == nullptr);
          continue;
        }
        REQUIRE(bucket != nullptr);
        REQUIRE(bucket->size() == oit->second.size());
        for (const auto &[row, w2] : *bucket) {
          const std::string key = row[0].GetValue<std::string>();
          INFO("step " << step << " bucket " << q << " row " << key);
          REQUIRE(oit->second.at(key) == w2);
        }
      }
    }
  }
  REQUIRE(idx.key_count() == oracle.size());
}

TEST_CASE("row codec roundtrips every value shape", "[spill][codec]") {
  using duckdb::Value;
  // fast-path types, fallback types, and typed NULLs — the codec must
  // reproduce values that hash and compare identically
  std::vector<duckdb::Value> row = {
      Value::INTEGER(42),
      Value::BIGINT(-7),
      Value::DOUBLE(3.25),
      Value("plain string"),
      Value(""),
      Value::BOOLEAN(true),
      Value(duckdb::LogicalType::INTEGER),  // typed NULL, fast path
      Value(duckdb::LogicalType::VARCHAR),  // typed NULL, fast path
      Value::DECIMAL(int64_t(12345), 12, 3),
      Value::DATE(duckdb::date_t(19000)),
      Value::TIMESTAMP(duckdb::timestamp_t(1700000000000000)),
      Value(duckdb::LogicalType::DECIMAL(10, 2)), // typed NULL, fallback
      Value::LIST({Value::INTEGER(1), Value::INTEGER(2)}),
      Value::STRUCT({{"a", Value::INTEGER(9)}, {"b", Value("x")}}),
  };
  std::vector<uint8_t> bytes;
  dbsp_native::serialize_row(row, bytes);
  auto back = dbsp_native::deserialize_row(bytes.data(), bytes.size());
  REQUIRE(back.size() == row.size());
  for (size_t i = 0; i < row.size(); i++) {
    INFO("column " << i << ": " << row[i].ToString());
    REQUIRE(row[i].IsNull() == back[i].IsNull());
    if (!row[i].IsNull()) {
      REQUIRE(row[i] == back[i]);
      REQUIRE(row[i].type() == back[i].type());
    } else {
      REQUIRE(row[i].type() == back[i].type());
    }
  }
}

// Data-plane micro-benchmarks: where does chunk -> Z-set ingestion time
// go? Splits the cost into row building (Value boxing), row hashing, and
// hash-map insertion, and measures the vectorized chunk hasher against
// the lazy per-Value path it must replicate bit-for-bit.

#include "../test_helpers.hpp"
#include "catch.hpp"
#include <chrono>
#include <iostream>

using namespace dbsp_test;
using namespace std::chrono;

namespace {

constexpr idx_t N_ROWS = 1000000;

// materialize a 1M-row three-column table into chunks once
duckdb::unique_ptr<duckdb::MaterializedQueryResult> make_rows(
    DuckDBTestHarness &db) {
  db.exec("CREATE TABLE bd AS SELECT i AS id, i % 1000 AS grp, "
          "'val_' || (i % 977) AS name FROM range(1000000) t(i)");
  return db.query("SELECT id, grp, name FROM bd");
}

template <typename Fn> double ms(Fn &&fn) {
  auto s = high_resolution_clock::now();
  fn();
  auto e = high_resolution_clock::now();
  return duration_cast<microseconds>(e - s).count() / 1000.0;
}

} // namespace

TEST_CASE("Benchmark: chunk ingestion cost split", "[benchmark][dataplane]") {
  DuckDBTestHarness db;
  auto res = make_rows(db);

  // collect chunks once so fetch cost is excluded
  std::vector<duckdb::unique_ptr<duckdb::DataChunk>> chunks;
  while (auto c = res->Fetch()) {
    if (c->size() == 0) break;
    chunks.push_back(std::move(c));
  }

  size_t sink = 0;

  // (a) row building only: Value boxing per cell
  std::vector<dbsp_native::DuckDBRow> rows;
  rows.reserve(N_ROWS);
  const double t_build = ms([&] {
    for (auto &c : chunks) {
      for (idx_t i = 0; i < c->size(); i++) {
        dbsp_native::DuckDBRow row;
        std::vector<duckdb::Value> vals;
        vals.reserve(3);
        for (idx_t col = 0; col < 3; col++) {
          vals.push_back(c->GetValue(col, i));
        }
        row.columns.assign(std::move(vals));
        rows.push_back(std::move(row));
      }
    }
  });

  // (b) lazy per-Value hashing of the built rows
  const double t_hash = ms([&] {
    for (auto &r : rows) {
      sink ^= r.columns.hash();
    }
  });

  // (c) Z-set insertion (hash cached from b — map cost only)
  dbsp_native::DuckDBZSet zset;
  const double t_insert = ms([&] {
    for (auto &r : rows) {
      zset.insert(r, 1);
    }
  });

  // (d) vectorized hashing + pre-seeded build + insert (the DP1 path)
  dbsp_native::DuckDBZSet zset2;
  const double t_vectorized = ms([&] {
    std::vector<size_t> hashes;
    for (auto &c : chunks) {
      dbsp_native::chunk_row_hashes(*c, hashes);
      for (idx_t i = 0; i < c->size(); i++) {
        dbsp_native::DuckDBRow row;
        std::vector<duckdb::Value> vals;
        vals.reserve(3);
        for (idx_t col = 0; col < 3; col++) {
          vals.push_back(c->GetValue(col, i));
        }
        row.columns.assign(std::move(vals));
        row.columns.set_hash(hashes[i]);
        zset2.insert(std::move(row), 1);
      }
    }
  });
  REQUIRE(zset2.size() == N_ROWS);
  const double t_legacy = t_build + t_hash + t_insert;

  std::cout << "[dataplane] 1M rows x 3 cols:\n"
            << "[dataplane]   build (Value boxing): " << t_build << " ms\n"
            << "[dataplane]   lazy hash:            " << t_hash << " ms\n"
            << "[dataplane]   zset insert (cached): " << t_insert << " ms\n"
            << "[dataplane]   vectorized end-to-end:  " << t_vectorized
            << " ms (legacy " << t_legacy << " ms, "
            << (t_legacy / t_vectorized) << "x)\n"
            << "[dataplane]   sink=" << (sink & 1)
            << " zset=" << zset.size() << "\n";
  REQUIRE(zset.size() == N_ROWS);
  // DP1 gate: pre-seeded ingestion must beat the legacy split soundly
  REQUIRE(t_vectorized * 1.5 < t_legacy);
}

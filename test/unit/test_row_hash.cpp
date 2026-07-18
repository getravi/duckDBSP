// chunk_row_hashes must replicate ColumnVec::hash EXACTLY: rows reach
// Z-sets from many construction paths, and two hashes for one logical
// row would break dedup silently. This is the correctness gate for the
// vectorized pre-seeding in stream_table_rows.

#include "catch.hpp"
#include "dbsp_duckdb_types.hpp"
#include "duckdb.hpp"

using namespace dbsp_native;

namespace {

void check_query(duckdb::Connection &con, const std::string &sql) {
  auto res = con.Query(sql);
  REQUIRE_FALSE(res->HasError());
  while (auto chunk = res->Fetch()) {
    const auto n = chunk->size();
    if (n == 0) {
      break;
    }
    std::vector<size_t> vectorized;
    chunk_row_hashes(*chunk, vectorized);
    for (duckdb::idx_t i = 0; i < n; i++) {
      std::vector<duckdb::Value> vals;
      for (duckdb::idx_t c = 0; c < chunk->ColumnCount(); c++) {
        vals.push_back(chunk->GetValue(c, i));
      }
      DuckDBRow row;
      row.columns.assign(std::move(vals));
      INFO(sql << " row " << i);
      REQUIRE(row.columns.hash() == vectorized[i]);
    }
  }
}

} // namespace

TEST_CASE("chunk_row_hashes matches the lazy per-Value hash",
          "[row_hash]") {
  duckdb::DuckDB db(nullptr);
  duckdb::Connection con(db);

  SECTION("scalar types with NULLs") {
    check_query(con,
                "SELECT i::INTEGER, i::BIGINT, i::DOUBLE, "
                "CASE WHEN i % 3 = 0 THEN NULL ELSE 'v_' || i END, "
                "i % 2 = 0, i::DECIMAL(12,3) / 7, "
                "DATE '2020-01-01' + INTERVAL (i) DAY, "
                "CASE WHEN i % 5 = 0 THEN NULL ELSE i END "
                "FROM range(3000) t(i)");
  }
  SECTION("nested types") {
    check_query(con, "SELECT [i, i + 1], {'a': i, 'b': 'x' || i}, "
                     "CASE WHEN i % 4 = 0 THEN NULL ELSE [i::VARCHAR] END "
                     "FROM range(500) t(i)");
  }
  SECTION("all-NULL column and empty strings") {
    check_query(con, "SELECT NULL::INTEGER, '', 'a' FROM range(100)");
  }
  SECTION("column subset selection and ordering") {
    auto res = con.Query("SELECT i, i * 2, 'x' || i FROM range(200) t(i)");
    auto chunk = res->Fetch();
    REQUIRE(chunk);
    // rows built from columns (2, 0) must match subset hashing
    std::vector<size_t> vectorized;
    chunk_row_hashes(*chunk, {2, 0}, vectorized);
    for (duckdb::idx_t i = 0; i < chunk->size(); i++) {
      DuckDBRow row;
      row.columns.assign({chunk->GetValue(2, i), chunk->GetValue(0, i)});
      REQUIRE(row.columns.hash() == vectorized[i]);
    }
  }
}

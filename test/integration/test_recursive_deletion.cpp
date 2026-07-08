// Differential tests for incremental recursive-view DELETION (DRed).
// Oracle: the same WITH RECURSIVE query run directly against the base table
// (non-incremental) must equal the incrementally-maintained view after every
// mutation.

#include "../test_helpers.hpp"
#include <algorithm>
#include <random>

using namespace dbsp_test;
using namespace duckdb;

namespace {

// Normalize a result set to a sorted multiset of stringified rows.
std::vector<std::string> sortedRows(const std::vector<std::vector<Value>> &rows) {
  std::vector<std::string> out;
  out.reserve(rows.size());
  for (const auto &r : rows) {
    std::string s;
    for (const auto &v : r) {
      s += v.IsNull() ? std::string("NULL") : v.ToString();
      s += '|';
    }
    out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> queryRows(DuckDBTestHarness &h, const std::string &sql) {
  auto result = h.query(sql);
  REQUIRE_FALSE(result->HasError());
  std::vector<std::vector<Value>> rows;
  for (size_t i = 0; i < result->RowCount(); i++) {
    std::vector<Value> row;
    for (size_t j = 0; j < result->ColumnCount(); j++)
      row.push_back(result->GetValue(j, i));
    rows.push_back(std::move(row));
  }
  return sortedRows(rows);
}

// Assert the incremental view equals the oracle recursive query.
// `oracle_sql` is a self-contained WITH RECURSIVE ... SELECT run on base tables.
void assertViewMatchesOracle(DuckDBTestHarness &h, const std::string &view_name,
                             const std::string &oracle_sql) {
  auto view = sortedRows(h.getViewRows(view_name));
  auto oracle = queryRows(h, oracle_sql);
  INFO("view rows=" << view.size() << " oracle rows=" << oracle.size());
  REQUIRE(view == oracle);
}

const char *kTcOracle =
    "WITH RECURSIVE tc AS ("
    "  SELECT src, dst FROM edges "
    "  UNION "
    "  SELECT tc.src, edges.dst FROM tc JOIN edges ON tc.dst = edges.src"
    ") SELECT * FROM tc";

// Create the transitive-closure view over an `edges(src,dst)` table.
void makeTcView(DuckDBTestHarness &h) {
  auto r = h.query(std::string("SELECT * FROM dbsp_create_view('tc', '") +
                   "WITH RECURSIVE tc AS ("
                   "  SELECT src, dst FROM edges "
                   "  UNION "
                   "  SELECT tc.src, edges.dst FROM tc JOIN edges ON tc.dst = edges.src"
                   ") SELECT * FROM tc')");
  REQUIRE_FALSE(r->HasError());
}

} // namespace

TEST_CASE("Recursive deletion: edge removal kills reachability",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  h.exec("INSERT INTO edges VALUES (1,2),(2,3),(3,4)");
  makeTcView(h);
  assertViewMatchesOracle(h, "tc", kTcOracle); // 6 rows

  h.exec("DELETE FROM edges WHERE src=2 AND dst=3");
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle); // now {(1,2),(3,4)}
}

TEST_CASE("Recursive deletion: alternate path keeps derived rows",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  // 1->2->4 and 1->3->4 : (1,4) has two supporting paths.
  h.exec("INSERT INTO edges VALUES (1,2),(2,4),(1,3),(3,4)");
  makeTcView(h);
  assertViewMatchesOracle(h, "tc", kTcOracle);

  // Remove one path; (1,4) must survive on the other (rederive).
  h.exec("DELETE FROM edges WHERE src=2 AND dst=4");
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle);
}

TEST_CASE("Recursive deletion: cycle edge removal",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  h.exec("INSERT INTO edges VALUES (1,2),(2,3),(3,1),(3,4)"); // cycle 1-2-3
  makeTcView(h);
  assertViewMatchesOracle(h, "tc", kTcOracle);

  h.exec("DELETE FROM edges WHERE src=3 AND dst=1"); // break the cycle
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle);
}

TEST_CASE("Recursive deletion: insert then delete round-trip",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  h.exec("INSERT INTO edges VALUES (1,2),(2,3)");
  makeTcView(h);
  assertViewMatchesOracle(h, "tc", kTcOracle);

  h.exec("INSERT INTO edges VALUES (3,4)");
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle);

  h.exec("DELETE FROM edges WHERE src=3 AND dst=4");
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle); // back to 3-row state
}

TEST_CASE("Recursive deletion: mixed insert+delete in one sync (no phantom)",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  h.exec("INSERT INTO edges VALUES (1,2),(2,3)");
  makeTcView(h);
  // Disable auto-sync so the batched INSERT+DELETE below coalesce into one
  // mixed delta at the single manual sync (auto-sync would split them).
  h.exec("SELECT * FROM dbsp_auto_sync(false)");
  assertViewMatchesOracle(h, "tc", kTcOracle); // {(1,2),(2,3),(1,3)}

  // Batch an INSERT and a DELETE before a single sync -> one mixed delta.
  // Oracle after this is {(2,3),(3,4),(2,4)}. The buggy insert_seed path
  // also emitted a phantom (1,4) (support computed against the pre-delete
  // sentinel), so this asserts the view equals the oracle exactly.
  h.exec("INSERT INTO edges VALUES (3,4)");
  h.exec("DELETE FROM edges WHERE src=1 AND dst=2");
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle);
}

TEST_CASE("Recursive deletion: single UPDATE statement (delete+insert delta)",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  h.exec("INSERT INTO edges VALUES (1,2),(2,3),(3,4)");
  makeTcView(h);
  h.exec("SELECT * FROM dbsp_auto_sync(false)");
  assertViewMatchesOracle(h, "tc", kTcOracle);

  // A single UPDATE is a delete-old + insert-new mixed delta on one sync.
  // Re-point 2->3 to 2->5: reachability from 1 and 2 shifts accordingly.
  h.exec("UPDATE edges SET dst=5 WHERE src=2 AND dst=3");
  h.exec("SELECT * FROM dbsp_sync('edges')");
  assertViewMatchesOracle(h, "tc", kTcOracle);
}

TEST_CASE("Recursive deletion: randomized differential (DRed == oracle)",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  makeTcView(h);

  std::mt19937 rng(1234);
  std::uniform_int_distribution<int> node(1, 7);
  std::set<std::pair<int, int>> present;

  for (int round = 0; round < 200; round++) {
    int s = node(rng), d = node(rng);
    bool insert = (rng() & 1);
    if (insert && !present.count({s, d})) {
      h.exec("INSERT INTO edges VALUES (" + std::to_string(s) + "," +
             std::to_string(d) + ")");
      present.insert({s, d});
    } else if (!insert && present.count({s, d})) {
      h.exec("DELETE FROM edges WHERE src=" + std::to_string(s) +
             " AND dst=" + std::to_string(d));
      present.erase({s, d});
    } else {
      continue; // no-op round
    }
    h.exec("SELECT * FROM dbsp_sync('edges')");
    assertViewMatchesOracle(h, "tc", kTcOracle);
  }
}

TEST_CASE("Recursive deletion: randomized MIXED-delta differential (DRed == oracle)",
          "[integration][recursive][deletion]") {
  DuckDBTestHarness h;
  h.exec("CREATE TABLE edges (src INTEGER, dst INTEGER)");
  makeTcView(h);
  // Disable auto-sync so multiple ops per round coalesce into one mixed delta.
  h.exec("SELECT * FROM dbsp_auto_sync(false)");

  std::mt19937 rng(9876);
  std::uniform_int_distribution<int> node(1, 7);
  std::uniform_int_distribution<int> opcount(1, 4);
  std::set<std::pair<int, int>> present;

  for (int round = 0; round < 200; round++) {
    // Perform a random number (1-4) of mixed INSERT/DELETE ops BEFORE a
    // single sync, so the scan-diff produces a mixed (insert+delete) delta.
    int ops = opcount(rng);
    for (int k = 0; k < ops; k++) {
      int s = node(rng), d = node(rng);
      bool insert = (rng() & 1);
      if (insert && !present.count({s, d})) {
        h.exec("INSERT INTO edges VALUES (" + std::to_string(s) + "," +
               std::to_string(d) + ")");
        present.insert({s, d});
      } else if (!insert && present.count({s, d})) {
        h.exec("DELETE FROM edges WHERE src=" + std::to_string(s) +
               " AND dst=" + std::to_string(d));
        present.erase({s, d});
      }
    }
    h.exec("SELECT * FROM dbsp_sync('edges')");
    assertViewMatchesOracle(h, "tc", kTcOracle);
  }
}

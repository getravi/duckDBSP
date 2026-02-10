
#include "catch.hpp"
#include "dbsp_cdc.hpp"
#include "duckdb.hpp"

using namespace dbsp_native;

TEST_CASE("Recursive CTE: Graph Reachability", "[recursive]") {
  CDCManager manager;
  manager.reset();

  duckdb::DuckDB db(nullptr);
  duckdb::Connection con(db);

  // Create edges table (autocommit)
  REQUIRE_NOTHROW(con.Query("CREATE TABLE edges (src INTEGER, dst INTEGER)"));

  // Track it - uses BeginTransaction() C++ API (not SQL BEGIN TRANSACTION)
  // which allows catalog access without blocking context.Query() calls
  con.BeginTransaction();
  REQUIRE(manager.track_table(*con.context, "edges"));
  con.Commit();

  // Insert initial data: 1->2, 2->3 (autocommit)
  REQUIRE_NOTHROW(con.Query("INSERT INTO edges VALUES (1, 2), (2, 3)"));

  // Sync in autocommit mode
  REQUIRE(manager.sync_table(*con.context, "edges"));

  // Define recursive view finding all paths
  std::string sql = R"(
        WITH RECURSIVE path AS (
            SELECT src, dst FROM edges
            UNION ALL
            SELECT p.src, e.dst
            FROM path p
            JOIN edges e ON p.dst = e.src
        )
        SELECT * FROM path
    )";

  // Create view - needs BeginTransaction() for catalog access on source tables
  con.BeginTransaction();
  REQUIRE(manager.create_view(*con.context, "all_paths", sql));
  con.Commit();

  auto result = manager.query_view("all_paths");
  REQUIRE(result != nullptr);

  // 1->2, 2->3 implies 1->3
  // Result should be (1,2), (2,3), (1,3)
  REQUIRE(result->size() == 3);

  // Verify content
  struct Edge {
    int src;
    int dst;
  };
  std::vector<Edge> paths;
  for (const auto &[row, weight] : *result) {
    if (weight > 0) {
      paths.push_back(
          {row.columns[0].GetValue<int>(), row.columns[1].GetValue<int>()});
    }
  }

  bool found_1_3 = false;
  for (const auto &p : paths) {
    if (p.src == 1 && p.dst == 3)
      found_1_3 = true;
  }
  REQUIRE(found_1_3);

  // Update: Add edge 3->4 (autocommit)
  REQUIRE_NOTHROW(con.Query("INSERT INTO edges VALUES (3, 4)"));

  // Sync in autocommit mode
  REQUIRE(manager.sync_table(*con.context, "edges"));

  // New paths expected:
  // (3,4) - from anchor
  // (2,4) - from (2,3) join (3,4)
  // (1,4) - from (1,3) join (3,4)
  // Total paths: 3 (old) + 3 (new) = 6

  auto result_updated = manager.query_view("all_paths");
  REQUIRE(result_updated->size() == 6);

  paths.clear();
  for (const auto &[row, weight] : *result_updated) {
    if (weight > 0) {
      paths.push_back(
          {row.columns[0].GetValue<int>(), row.columns[1].GetValue<int>()});
    }
  }

  bool found_1_4 = false;
  for (const auto &p : paths) {
    if (p.src == 1 && p.dst == 4)
      found_1_4 = true;
  }
  REQUIRE(found_1_4);
}

TEST_CASE("Recursive CTE: Sequence Generation", "[recursive]") {
  CDCManager manager;
  manager.reset();
  duckdb::DuckDB db(nullptr);
  duckdb::Connection con(db);

  // Create and track table
  REQUIRE_NOTHROW(con.Query("CREATE TABLE trigger_table (i INTEGER)"));
  con.BeginTransaction();
  REQUIRE(manager.track_table(*con.context, "trigger_table"));
  con.Commit();

  // Driven sequence:
  std::string sql_driven = R"(
        WITH RECURSIVE t(n) AS (
            SELECT i FROM trigger_table
            UNION ALL
            SELECT n+1 FROM t WHERE n < 5
        )
        SELECT * FROM t
    )";

  con.BeginTransaction();
  REQUIRE(manager.create_view(*con.context, "seq_view", sql_driven));
  con.Commit();

  // Insert 1 (autocommit)
  REQUIRE_NOTHROW(con.Query("INSERT INTO trigger_table VALUES (1)"));

  // Sync in autocommit mode
  REQUIRE(manager.sync_table(*con.context, "trigger_table"));

  auto result = manager.query_view("seq_view");
  REQUIRE(result != nullptr);
  // 1, 2, 3, 4, 5. Size = 5.
  REQUIRE(result->size() == 5);
}

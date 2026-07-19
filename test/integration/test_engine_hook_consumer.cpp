// End-to-end tests for the engine-hook consumer (dbsp_engine_hook.hpp):
// with DBSP_ENGINE_HOOK compiled in and the patched engine in the submodule,
// the extension's views must stay current fed ONLY by the engine's exact
// commit deltas — the design-1/plan-tee capture stack is disarmed, no commit
// guards run. See docs/superpowers/plans/2026-07-18-engine-hook-impl.md.
#include "../test_helpers.hpp"
#include "catch.hpp"
#include "dbsp_engine_hook.hpp"

using namespace dbsp_test;

TEST_CASE("engine hook consumer: flag flips on first delivery, not registration",
          "[engine_hook_consumer]") {
  // Proof-of-life contract: registration alone must NOT disarm the capture
  // stack; only a delivered ingest proves the engine actually consults the
  // registry (guards against the two-image static-registry failure mode).
  DuckDBTestHarness db;
  db.createTable("poke", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('poke')");
  db.exec("INSERT INTO poke VALUES (1, 1.0)");
  REQUIRE(dbsp_native::engine_hook_active());
}

TEST_CASE("engine hook consumer: views stay current via engine deltas", "[engine_hook_consumer]") {
  DuckDBTestHarness db;
  db.createTable("items", "id INTEGER, name VARCHAR, price DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('items')");
  db.exec("SELECT * FROM dbsp_create_view('expensive', "
          "'SELECT * FROM items WHERE price > 50')");

  const auto ingests_before = dbsp_native::engine_hook_stats().tables_ingested.load();

  SECTION("autocommit insert / update / delete") {
    db.exec("INSERT INTO items VALUES (1, 'widget', 10.0), (2, 'gadget', 100.0)");
    auto rows = db.getViewRows("expensive");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][1].ToString() == "gadget");

    db.exec("UPDATE items SET price = 60.0 WHERE id = 1");
    rows = db.getViewRows("expensive");
    REQUIRE(rows.size() == 2);

    db.exec("DELETE FROM items WHERE id = 2");
    rows = db.getViewRows("expensive");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][1].ToString() == "widget");

    // proves the engine path fed these commits, not the capture stack
    REQUIRE(dbsp_native::engine_hook_stats().tables_ingested.load() > ingests_before);
  }

  SECTION("explicit transaction with mixed DML in one commit") {
    db.exec("INSERT INTO items VALUES (1, 'a', 80.0), (2, 'b', 90.0), (3, 'c', 20.0)");
    db.exec("BEGIN TRANSACTION");
    db.exec("INSERT INTO items VALUES (4, 'd', 200.0)");
    db.exec("UPDATE items SET price = 55.0 WHERE id = 3");
    db.exec("DELETE FROM items WHERE id = 1");
    db.exec("COMMIT");

    auto rows = db.getViewRows("expensive");
    // survivors > 50: (2,b,90), (3,c,55), (4,d,200)
    REQUIRE(rows.size() == 3);
    REQUIRE(dbsp_native::engine_hook_stats().tables_ingested.load() > ingests_before);
  }

  SECTION("rollback leaves views untouched") {
    db.exec("INSERT INTO items VALUES (1, 'keep', 70.0)");
    db.exec("BEGIN TRANSACTION");
    db.exec("INSERT INTO items VALUES (2, 'phantom', 500.0)");
    db.exec("UPDATE items SET price = 5.0 WHERE id = 1");
    db.exec("ROLLBACK");

    auto rows = db.getViewRows("expensive");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][1].ToString() == "keep");
  }

  SECTION("update then delete of same row in one txn") {
    db.exec("INSERT INTO items VALUES (1, 'x', 60.0), (2, 'y', 70.0)");
    db.exec("BEGIN TRANSACTION");
    db.exec("UPDATE items SET price = 999.0 WHERE id = 1");
    db.exec("DELETE FROM items WHERE id = 1");
    db.exec("COMMIT");

    auto rows = db.getViewRows("expensive");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][1].ToString() == "y");
  }
}

TEST_CASE("engine hook consumer: untracked tables ignored", "[engine_hook_consumer]") {
  DuckDBTestHarness db;
  db.createTable("tracked", "id INTEGER, v DOUBLE", {});
  db.createTable("untracked", "id INTEGER, v DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('tracked')");
  db.exec("SELECT * FROM dbsp_create_view('big', 'SELECT * FROM tracked WHERE v > 1')");

  db.exec("INSERT INTO untracked VALUES (1, 100.0)");
  db.exec("INSERT INTO tracked VALUES (1, 2.0)");

  auto rows = db.getViewRows("big");
  REQUIRE(rows.size() == 1);
}

TEST_CASE("engine hook consumer: multi-statement batch and aggregation view", "[engine_hook_consumer]") {
  DuckDBTestHarness db;
  db.createTable("sales", "region VARCHAR, amount DOUBLE", {});
  db.exec("SELECT * FROM dbsp_track('sales')");
  db.exec("SELECT * FROM dbsp_create_view('totals', "
          "'SELECT region, SUM(amount) AS total FROM sales GROUP BY region')");

  db.exec("INSERT INTO sales VALUES ('west', 10.0), ('west', 20.0), ('east', 5.0)");
  db.exec("UPDATE sales SET amount = 30.0 WHERE region = 'east'");

  auto rows = db.getViewRows("totals");
  REQUIRE(rows.size() == 2);
  double west = -1, east = -1;
  for (auto &r : rows) {
    if (r[0].ToString() == "west") {
      west = r[1].GetValue<double>();
    }
    if (r[0].ToString() == "east") {
      east = r[1].GetValue<double>();
    }
  }
  REQUIRE(west == 30.0);
  REQUIRE(east == 30.0);
}

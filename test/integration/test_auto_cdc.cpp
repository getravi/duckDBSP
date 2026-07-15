#include "../test_helpers.hpp"
#include "catch.hpp"
#include <random>

using namespace dbsp_test;
using namespace std;

TEST_CASE("Automatic CDC via Transaction Hooks", "[dbsp][auto_cdc]") {
  DuckDBTestHarness db;

  // Setup table
  db.createTable("items", "id INTEGER, name VARCHAR, price DECIMAL(10,2)", {});
  db.exec("SELECT * FROM dbsp_track('items')");

  // Create view (use SELECT * since projection returns all columns)
  db.exec("SELECT * FROM dbsp_create_view('expensive_items', "
          "'SELECT * FROM items WHERE price > 50')");

  SECTION("Manual sync updates views correctly") {
    // Insert data using standard SQL
    db.exec("INSERT INTO items VALUES (1, 'Widget', 10.00)");
    db.exec("INSERT INTO items VALUES (2, 'Gadget', 100.00)");

    // Sync required until automatic CDC (P4.1) is implemented
    db.exec("SELECT * FROM dbsp_sync('items')");

    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 1);
    // Columns: id, name, price
    REQUIRE(rows[0][1].ToString() == "Gadget");
  }

  SECTION("Sync after transaction commit") {
    db.exec("BEGIN TRANSACTION");
    db.exec("INSERT INTO items VALUES (3, 'Premium', 200.00)");
    db.exec("INSERT INTO items VALUES (4, 'Cheap', 20.00)");

    db.exec("COMMIT");

    // Sync required until automatic CDC (P4.1) is implemented
    db.exec("SELECT * FROM dbsp_sync('items')");

    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 1);
    // Columns: id, name, price
    REQUIRE(rows[0][1].ToString() == "Premium");
  }

  SECTION("No data after rollback and sync") {
    db.exec("BEGIN TRANSACTION");
    db.exec("INSERT INTO items VALUES (5, 'Mistake', 500.00)");

    db.exec("ROLLBACK");

    db.exec("SELECT * FROM dbsp_sync('items')");

    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 0);
  }

  SECTION("Auto-sync without manual dbsp_sync") {
    // Enable automatic CDC
    db.exec("SELECT * FROM dbsp_auto_sync(true)");

    // Insert data using standard SQL
    db.exec("INSERT INTO items VALUES (10, 'AutoWidget', 75.00)");
    db.exec("INSERT INTO items VALUES (11, 'AutoGadget', 150.00)");

    // NO manual sync - auto-sync should handle it!
    // db.exec("SELECT * FROM dbsp_sync('items')"); // REMOVED

    // Views should be updated automatically
    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 2);

    // Verify both items are present
    bool found_widget = false;
    bool found_gadget = false;
    for (const auto &row : rows) {
      std::string name = row[1].ToString();
      if (name == "AutoWidget") found_widget = true;
      if (name == "AutoGadget") found_gadget = true;
    }
    REQUIRE(found_widget);
    REQUIRE(found_gadget);
  }

  SECTION("Auto-sync with explicit transaction") {
    // Enable automatic CDC
    db.exec("SELECT * FROM dbsp_auto_sync(true)");

    // Use explicit transaction
    db.exec("BEGIN TRANSACTION");
    db.exec("INSERT INTO items VALUES (20, 'TxnPremium', 300.00)");
    db.exec("INSERT INTO items VALUES (21, 'TxnCheap', 15.00)");
    db.exec("COMMIT");

    // NO manual sync - auto-sync should trigger on commit!
    // db.exec("SELECT * FROM dbsp_sync('items')"); // REMOVED

    // View should have exactly 1 expensive item
    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][1].ToString() == "TxnPremium");
  }

  SECTION("Auto-sync can be toggled on/off") {
    // Disable auto-sync
    db.exec("SELECT * FROM dbsp_auto_sync(false)");

    // Insert data
    db.exec("INSERT INTO items VALUES (30, 'NoAutoSync', 500.00)");

    // View should NOT be updated (auto-sync is off)
    auto rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 0);

    // Manual sync should still work
    db.exec("SELECT * FROM dbsp_sync('items')");
    rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 1);

    // Re-enable auto-sync
    db.exec("SELECT * FROM dbsp_auto_sync(true)");
    db.exec("INSERT INTO items VALUES (31, 'AutoAgain', 600.00)");

    // Should auto-update now
    rows = db.getViewRows("expensive_items");
    REQUIRE(rows.size() == 2);
  }
}

// ===== G2: captured-delta fast path =====

TEST_CASE("G2: explicit-txn inserts sync via captured deltas",
          "[integration][auto_cdc][capture]") {
  DuckDBTestHarness db;
  db.createTable("ct", "id INT, val INT", {"(1, 10)"});
  db.exec("SELECT * FROM dbsp_track('ct')");
  db.exec("SELECT * FROM dbsp_sync('ct')");
  db.exec("SELECT * FROM dbsp_create_view('v_cap', "
          "'SELECT id, val FROM ct WHERE val > 5')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  auto &manager = db.manager();
  const uint64_t before = manager.captured_delta_syncs();

  db.exec("BEGIN");
  db.exec("INSERT INTO ct VALUES (2, 20), (3, 3)");
  db.exec("INSERT INTO ct VALUES (4, 40)");
  db.exec("COMMIT");

  // Fast path must have served the commit (no scan-and-diff)...
  REQUIRE(manager.captured_delta_syncs() == before + 1);
  // ...and the view must be correct: vals 10, 20, 40 pass (3 filtered)
  db.assertViewRowCount("v_cap", 3);

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("G2: txn with a delete falls back to scan-and-diff",
          "[integration][auto_cdc][capture]") {
  DuckDBTestHarness db;
  db.createTable("ct2", "id INT, val INT", {"(1, 10)", "(2, 20)"});
  db.exec("SELECT * FROM dbsp_track('ct2')");
  db.exec("SELECT * FROM dbsp_sync('ct2')");
  db.exec("SELECT * FROM dbsp_create_view('v_cap2', "
          "'SELECT id FROM ct2 WHERE val > 5')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  auto &manager = db.manager();
  const uint64_t before = manager.captured_delta_syncs();

  db.exec("BEGIN");
  db.exec("INSERT INTO ct2 VALUES (3, 30)");
  db.exec("DELETE FROM ct2 WHERE id = 1"); // poisons capture
  db.exec("COMMIT");

  REQUIRE(manager.captured_delta_syncs() == before); // no fast path
  db.assertViewRowCount("v_cap2", 2);                // but still correct

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("G2: rolled-back txn leaves views untouched",
          "[integration][auto_cdc][capture]") {
  DuckDBTestHarness db;
  db.createTable("ct3", "id INT, val INT", {"(1, 10)"});
  db.exec("SELECT * FROM dbsp_track('ct3')");
  db.exec("SELECT * FROM dbsp_sync('ct3')");
  db.exec("SELECT * FROM dbsp_create_view('v_cap3', "
          "'SELECT id FROM ct3 WHERE val > 5')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  db.exec("BEGIN");
  db.exec("INSERT INTO ct3 VALUES (2, 20)");
  db.exec("ROLLBACK");

  db.assertViewRowCount("v_cap3", 1); // captured rows never happened

  // A subsequent committed txn works normally
  db.exec("BEGIN");
  db.exec("INSERT INTO ct3 VALUES (3, 30)");
  db.exec("COMMIT");
  db.assertViewRowCount("v_cap3", 2);

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("G2: autocommit inserts still sync (scan path)",
          "[integration][auto_cdc][capture]") {
  DuckDBTestHarness db;
  db.createTable("ct4", "id INT, val INT", {"(1, 10)"});
  db.exec("SELECT * FROM dbsp_track('ct4')");
  db.exec("SELECT * FROM dbsp_sync('ct4')");
  db.exec("SELECT * FROM dbsp_create_view('v_cap4', "
          "'SELECT id FROM ct4 WHERE val > 5')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  db.exec("INSERT INTO ct4 VALUES (2, 20)");
  db.assertViewRowCount("v_cap4", 2);

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("G2: randomized explicit-txn churn stays correct",
          "[integration][auto_cdc][capture]") {
  DuckDBTestHarness db;
  db.createTable("cr", "id INT, val INT", {"(0, 5)"});
  db.exec("SELECT * FROM dbsp_track('cr')");
  db.exec("SELECT * FROM dbsp_sync('cr')");
  db.exec("SELECT * FROM dbsp_create_view('v_cr', "
          "'SELECT id, val FROM cr WHERE val > 10')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  std::mt19937 rng(4242);
  for (int round = 0; round < 30; round++) {
    db.exec("BEGIN");
    int inserts = static_cast<int>(rng() % 4) + 1;
    for (int i = 0; i < inserts; i++) {
      db.exec("INSERT INTO cr VALUES (" + std::to_string(round * 10 + i) +
              ", " + std::to_string(rng() % 40) + ")");
    }
    if (rng() % 4 == 0) {
      // Poison ~25% of transactions with a delete: fallback must engage
      db.exec("DELETE FROM cr WHERE id = " +
              std::to_string(rng() % (round * 10 + 1)));
    }
    if (rng() % 5 == 0) {
      db.exec("ROLLBACK");
    } else {
      db.exec("COMMIT");
    }

    auto expected = db.query("SELECT * FROM (SELECT id, val FROM cr "
                             "WHERE val > 10) ORDER BY ALL");
    auto actual =
        db.query("SELECT * FROM dbsp_query('v_cr') ORDER BY ALL");
    REQUIRE_FALSE(expected->HasError());
    REQUIRE_FALSE(actual->HasError());
    REQUIRE(actual->RowCount() == expected->RowCount());
    for (size_t r = 0; r < expected->RowCount(); r++) {
      for (size_t c = 0; c < expected->ColumnCount(); c++) {
        REQUIRE(actual->GetValue(c, r).ToString() ==
                expected->GetValue(c, r).ToString());
      }
    }
  }
  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("H1: commits scan only the tables they touched",
          "[integration][auto_cdc][scoping]") {
  DuckDBTestHarness db;
  db.createTable("sa", "id INT", {"(1)"});
  db.createTable("sb", "id INT", {"(1)"});
  db.exec("SELECT * FROM dbsp_track('sa')");
  db.exec("SELECT * FROM dbsp_track('sb')");
  db.exec("SELECT * FROM dbsp_sync('sa')");
  db.exec("SELECT * FROM dbsp_sync('sb')");
  db.exec("SELECT * FROM dbsp_create_view('v_sa', 'SELECT id FROM sa')");
  db.exec("SELECT * FROM dbsp_create_view('v_sb', 'SELECT id FROM sb')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");

  auto &manager = db.manager();
  // This test proves SCAN SCOPING, so force the scan path: capturable
  // UPDATE/DELETE statements would otherwise skip the scan entirely
  // (write capture, covered by its own tests below)
  manager.set_write_capture_enabled(false);

  // Autocommit DELETE on sa: exactly one table scanned, not two
  uint64_t scans = manager.scan_syncs();
  db.exec("DELETE FROM sa WHERE id = 1");
  REQUIRE(manager.scan_syncs() == scans + 1);
  db.assertViewRowCount("v_sa", 0);
  db.assertViewRowCount("v_sb", 1);

  // Explicit txn UPDATE on sb: one scan
  scans = manager.scan_syncs();
  db.exec("BEGIN");
  db.exec("UPDATE sb SET id = 2 WHERE id = 1");
  db.exec("COMMIT");
  REQUIRE(manager.scan_syncs() == scans + 1);
  db.assertViewRowCount("v_sb", 1);

  // Read-only explicit txn: zero scans
  scans = manager.scan_syncs();
  db.exec("BEGIN");
  db.exec("SELECT * FROM sa");
  db.exec("COMMIT");
  REQUIRE(manager.scan_syncs() == scans);

  // Captured-delta commit: zero scans (fast path, no scan at all)
  scans = manager.scan_syncs();
  const uint64_t caps = manager.captured_delta_syncs();
  db.exec("BEGIN");
  db.exec("INSERT INTO sa VALUES (7)");
  db.exec("COMMIT");
  REQUIRE(manager.captured_delta_syncs() == caps + 1);
  REQUIRE(manager.scan_syncs() == scans);
  db.assertViewRowCount("v_sa", 1);

  manager.set_write_capture_enabled(true);
  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

// ===== Write capture: O(Δ) UPDATE/DELETE auto-sync =====
// Differential matrix (docs/DESIGN_WRITE_CAPTURE.md): every scenario
// checks the views against direct SQL over the base tables, and asserts
// via counters WHICH path served the commit (captured vs scan).

namespace {

// Base tables wt/wd/we + aggregate, join, and chained views
struct WriteCaptureFixture {
  DuckDBTestHarness db;

  WriteCaptureFixture() {
    db.createTable("wt", "id INT, grp INT, val INT",
                   {"(1, 1, 10)", "(2, 1, 20)", "(3, 2, 30)", "(4, 2, NULL)"});
    db.createTable("wd", "grp INT, name VARCHAR", {"(1, 'a')", "(2, 'b')"});
    db.createTable("we", "id INT, tag VARCHAR", {"(1, 'x')", "(2, 'y')"});
    db.exec("SELECT * FROM dbsp_track('wt')");
    db.exec("SELECT * FROM dbsp_track('wd')");
    db.exec("SELECT * FROM dbsp_track('we')");
    db.exec("SELECT * FROM dbsp_sync('wt')");
    db.exec("SELECT * FROM dbsp_sync('wd')");
    db.exec("SELECT * FROM dbsp_sync('we')");
    db.exec("SELECT * FROM dbsp_create_view('wv_agg', 'SELECT grp, "
            "SUM(val) AS s, COUNT(*) AS n FROM wt GROUP BY grp')");
    db.exec("SELECT * FROM dbsp_create_view('wv_join', 'SELECT wt.id, "
            "wt.val, wd.name FROM wt JOIN wd ON wt.grp = wd.grp')");
    db.exec("SELECT * FROM dbsp_create_view('wv_chain', "
            "'SELECT grp, s FROM wv_agg WHERE n >= 2')");
    db.exec("SELECT * FROM dbsp_auto_sync(true)");
  }

  void finish() {
    db.manager().set_write_capture_enabled(true);
    db.exec("SELECT * FROM dbsp_auto_sync(false)");
  }

  // Every view must equal the same query run directly over base tables
  void check_views() {
    check("wv_agg",
          "SELECT grp, SUM(val) AS s, COUNT(*) AS n FROM wt GROUP BY grp");
    check("wv_join", "SELECT wt.id, wt.val, wd.name FROM wt JOIN wd "
                     "ON wt.grp = wd.grp");
    check("wv_chain", "SELECT grp, SUM(val) AS s FROM wt GROUP BY grp "
                      "HAVING COUNT(*) >= 2");
  }

  void check(const std::string &view, const std::string &sql) {
    auto expected = db.query("SELECT * FROM (" + sql + ") ORDER BY ALL");
    auto actual =
        db.query("SELECT * FROM dbsp_query('" + view + "') ORDER BY ALL");
    REQUIRE_FALSE(expected->HasError());
    REQUIRE_FALSE(actual->HasError());
    INFO("view " << view);
    REQUIRE(actual->RowCount() == expected->RowCount());
    for (size_t r = 0; r < expected->RowCount(); r++) {
      for (size_t c = 0; c < expected->ColumnCount(); c++) {
        REQUIRE(actual->GetValue(c, r).ToString() ==
                expected->GetValue(c, r).ToString());
      }
    }
  }
};

} // namespace

TEST_CASE("write capture: single-row UPDATE (autocommit + explicit txn)",
          "[integration][auto_cdc][write_capture]") {
  WriteCaptureFixture fx;
  auto &m = fx.db.manager();

  uint64_t caps = m.captured_delta_syncs();
  uint64_t scans = m.scan_syncs();
  fx.db.exec("UPDATE wt SET val = 99 WHERE id = 1"); // autocommit
  REQUIRE(m.captured_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check_views();

  caps = m.captured_delta_syncs();
  scans = m.scan_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("UPDATE wt SET val = 7 WHERE id = 3");
  fx.db.exec("COMMIT");
  REQUIRE(m.captured_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check_views();

  fx.finish();
}

TEST_CASE("write capture: multi-row expression UPDATE",
          "[integration][auto_cdc][write_capture]") {
  WriteCaptureFixture fx;
  auto &m = fx.db.manager();
  const uint64_t caps = m.captured_delta_syncs();
  const uint64_t scans = m.scan_syncs();
  fx.db.exec("UPDATE wt SET val = val + 1 WHERE grp = 1");
  REQUIRE(m.captured_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check_views();
  fx.finish();
}

TEST_CASE("write capture: UPDATE moves a group-by key",
          "[integration][auto_cdc][write_capture]") {
  WriteCaptureFixture fx;
  auto &m = fx.db.manager();
  const uint64_t caps = m.captured_delta_syncs();
  fx.db.exec("UPDATE wt SET grp = 2 WHERE id = 1");
  REQUIRE(m.captured_delta_syncs() == caps + 1);
  fx.check_views();
  fx.finish();
}

TEST_CASE("write capture: NULL handling in SET and WHERE",
          "[integration][auto_cdc][write_capture]") {
  WriteCaptureFixture fx;
  auto &m = fx.db.manager();
  uint64_t caps = m.captured_delta_syncs();
  fx.db.exec("UPDATE wt SET val = NULL WHERE id = 2");
  REQUIRE(m.captured_delta_syncs() == caps + 1);
  fx.check_views();

  caps = m.captured_delta_syncs();
  fx.db.exec("UPDATE wt SET val = 5 WHERE val IS NULL");
  REQUIRE(m.captured_delta_syncs() == caps + 1);
  fx.check_views();
  fx.finish();
}

TEST_CASE("write capture: DELETE (autocommit + explicit txn + delete-all)",
          "[integration][auto_cdc][write_capture]") {
  WriteCaptureFixture fx;
  auto &m = fx.db.manager();

  uint64_t caps = m.captured_delta_syncs();
  uint64_t scans = m.scan_syncs();
  fx.db.exec("DELETE FROM wt WHERE id = 4");
  REQUIRE(m.captured_delta_syncs() == caps + 1);
  REQUIRE(m.scan_syncs() == scans);
  fx.check_views();

  caps = m.captured_delta_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("DELETE FROM wt WHERE grp = 1");
  fx.db.exec("COMMIT");
  REQUIRE(m.captured_delta_syncs() == caps + 1);
  fx.check_views();

  caps = m.captured_delta_syncs();
  fx.db.exec("DELETE FROM wt"); // delete-all, still capturable
  REQUIRE(m.captured_delta_syncs() == caps + 1);
  fx.check_views();
  fx.finish();
}

TEST_CASE("write capture: fallback shapes take the scan path",
          "[integration][auto_cdc][write_capture]") {
  WriteCaptureFixture fx;
  auto &m = fx.db.manager();

  SECTION("DELETE with subquery WHERE") {
    const uint64_t caps = m.captured_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec(
        "DELETE FROM wt WHERE grp IN (SELECT grp FROM wd WHERE name = 'a')");
    REQUIRE(m.captured_delta_syncs() == caps);
    REQUIRE(m.scan_syncs() == scans + 1);
    fx.check_views();
  }
  SECTION("DELETE USING") {
    const uint64_t caps = m.captured_delta_syncs();
    fx.db.exec("DELETE FROM wt USING wd WHERE wt.grp = wd.grp "
               "AND wd.name = 'b'");
    REQUIRE(m.captured_delta_syncs() == caps);
    fx.check_views();
  }
  SECTION("non-deterministic predicate") {
    const uint64_t caps = m.captured_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("UPDATE wt SET val = 1 WHERE random() < -1.0");
    REQUIRE(m.captured_delta_syncs() == caps);
    REQUIRE(m.scan_syncs() == scans + 1);
    fx.check_views();
  }
  fx.finish();
}

TEST_CASE("write capture: upsert and indexed-column UPDATE fall back",
          "[integration][auto_cdc][write_capture]") {
  DuckDBTestHarness db;
  db.createTable("wpk", "id INT PRIMARY KEY, val INT", {"(1, 10)", "(2, 20)"});
  db.exec("SELECT * FROM dbsp_track('wpk')");
  db.exec("SELECT * FROM dbsp_sync('wpk')");
  db.exec("SELECT * FROM dbsp_create_view('wv_pk', "
          "'SELECT id, val FROM wpk WHERE val > 5')");
  db.exec("SELECT * FROM dbsp_auto_sync(true)");
  auto &m = db.manager();

  // PK column in an index: update_is_del_and_insert, rowids unstable
  uint64_t caps = m.captured_delta_syncs();
  db.exec("UPDATE wpk SET id = id + 10 WHERE val = 10");
  REQUIRE(m.captured_delta_syncs() == caps);
  db.assertViewRowCount("wv_pk", 2);

  // Non-indexed column of the same table IS capturable
  caps = m.captured_delta_syncs();
  db.exec("UPDATE wpk SET val = 50 WHERE id = 2");
  REQUIRE(m.captured_delta_syncs() == caps + 1);

  // Upsert stays on the scan path
  caps = m.captured_delta_syncs();
  const uint64_t scans = m.scan_syncs();
  db.exec("INSERT INTO wpk VALUES (2, 99) ON CONFLICT (id) "
          "DO UPDATE SET val = excluded.val");
  REQUIRE(m.captured_delta_syncs() == caps);
  REQUIRE(m.scan_syncs() == scans + 1);

  auto expected = db.query("SELECT * FROM (SELECT id, val FROM wpk "
                           "WHERE val > 5) ORDER BY ALL");
  auto actual = db.query("SELECT * FROM dbsp_query('wv_pk') ORDER BY ALL");
  REQUIRE(actual->RowCount() == expected->RowCount());

  db.exec("SELECT * FROM dbsp_auto_sync(false)");
}

TEST_CASE("write capture: mixed INSERT+UPDATE+DELETE transaction",
          "[integration][auto_cdc][write_capture]") {
  WriteCaptureFixture fx;
  auto &m = fx.db.manager();

  SECTION("across different tables: fully captured") {
    const uint64_t caps = m.captured_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("BEGIN");
    fx.db.exec("INSERT INTO wt VALUES (9, 1, 90)");
    fx.db.exec("UPDATE wd SET name = 'z' WHERE grp = 2");
    fx.db.exec("DELETE FROM we WHERE id = 1");
    fx.db.exec("COMMIT");
    // one apply per touched table, zero scans
    REQUIRE(m.captured_delta_syncs() == caps + 3);
    REQUIRE(m.scan_syncs() == scans);
    fx.check_views();
  }
  SECTION("same table written twice: whole txn falls back") {
    const uint64_t caps = m.captured_delta_syncs();
    const uint64_t scans = m.scan_syncs();
    fx.db.exec("BEGIN");
    fx.db.exec("INSERT INTO wt VALUES (9, 1, 90)");
    fx.db.exec("UPDATE wt SET val = 1 WHERE id = 9");
    fx.db.exec("COMMIT");
    REQUIRE(m.captured_delta_syncs() == caps);
    REQUIRE(m.scan_syncs() == scans + 1); // one table touched, one scan
    fx.check_views();
  }
  fx.finish();
}

TEST_CASE("write capture: rollback discards captured writes",
          "[integration][auto_cdc][write_capture]") {
  WriteCaptureFixture fx;
  auto &m = fx.db.manager();
  const uint64_t caps = m.captured_delta_syncs();
  fx.db.exec("BEGIN");
  fx.db.exec("UPDATE wt SET val = 1000 WHERE id = 1");
  fx.db.exec("DELETE FROM we WHERE id = 2");
  fx.db.exec("ROLLBACK");
  REQUIRE(m.captured_delta_syncs() == caps);
  fx.check_views(); // views still match (unchanged) base tables
  fx.finish();
}

TEST_CASE("write capture: forced scan path stays differential-identical",
          "[integration][auto_cdc][write_capture]") {
  // Same edits under capture and under forced scan must land identical
  // view contents (both checked against direct SQL)
  auto run_edits = [](WriteCaptureFixture &fx) {
    fx.db.exec("UPDATE wt SET val = val * 2 WHERE grp = 1");
    fx.db.exec("DELETE FROM wt WHERE id = 3");
    fx.db.exec("BEGIN");
    fx.db.exec("UPDATE wd SET name = 'q' WHERE grp = 1");
    fx.db.exec("COMMIT");
    fx.check_views();
  };

  WriteCaptureFixture captured;
  const uint64_t caps = captured.db.manager().captured_delta_syncs();
  run_edits(captured);
  REQUIRE(captured.db.manager().captured_delta_syncs() > caps);
  captured.finish();

  WriteCaptureFixture scanned;
  scanned.db.manager().set_write_capture_enabled(false);
  const uint64_t caps2 = scanned.db.manager().captured_delta_syncs();
  run_edits(scanned);
  REQUIRE(scanned.db.manager().captured_delta_syncs() == caps2);
  scanned.finish();
}

TEST_CASE("write capture: notify-API replay matches captured auto-sync",
          "[integration][auto_cdc][write_capture]") {
  // Path (a): captured auto-sync
  WriteCaptureFixture captured;
  captured.db.exec("UPDATE wt SET val = 42 WHERE id = 1");
  captured.db.exec("DELETE FROM wt WHERE id = 3");
  auto a = captured.db.query(
      "SELECT * FROM dbsp_query('wv_agg') ORDER BY ALL");
  REQUIRE_FALSE(a->HasError());
  captured.finish();

  // Path (c): auto-sync off, SQL writes + manual notify replay of the
  // same logical delta (storage first, then notify — the notify contract)
  WriteCaptureFixture notified;
  notified.db.exec("SELECT * FROM dbsp_auto_sync(false)");
  notified.db.exec("UPDATE wt SET val = 42 WHERE id = 1");
  notified.db.exec("DELETE FROM wt WHERE id = 3");
  notified.db.exec("SELECT * FROM dbsp_notify_delete('wt', 1, 1, 10)");
  notified.db.exec("SELECT * FROM dbsp_notify_insert('wt', 1, 1, 42)");
  notified.db.exec("SELECT * FROM dbsp_notify_delete('wt', 3, 2, 30)");
  auto c = notified.db.query(
      "SELECT * FROM dbsp_query('wv_agg') ORDER BY ALL");
  REQUIRE_FALSE(c->HasError());

  REQUIRE(a->RowCount() == c->RowCount());
  for (size_t r = 0; r < a->RowCount(); r++) {
    for (size_t col = 0; col < a->ColumnCount(); col++) {
      REQUIRE(a->GetValue(col, r).ToString() ==
              c->GetValue(col, r).ToString());
    }
  }
}

TEST_CASE("write capture: guard fallbacks are counted",
          "[integration][auto_cdc][write_capture]") {
  WriteCaptureFixture fx;
  auto &m = fx.db.manager();
  // A capturable statement whose guard cannot fail normally: no fallback
  const uint64_t fb = m.capture_guard_fallbacks();
  fx.db.exec("UPDATE wt SET val = 8 WHERE id = 2");
  REQUIRE(m.capture_guard_fallbacks() == fb);
  // commit_seq advances with every applied delta
  const uint64_t seq = m.commit_seq();
  fx.db.exec("UPDATE wt SET val = 9 WHERE id = 2");
  REQUIRE(m.commit_seq() > seq);
  fx.finish();
}

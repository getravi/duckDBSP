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

  auto &manager = dbsp_native::get_cdc_manager();
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

  auto &manager = dbsp_native::get_cdc_manager();
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

#include "../test_helpers.hpp"
#include "catch.hpp"

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

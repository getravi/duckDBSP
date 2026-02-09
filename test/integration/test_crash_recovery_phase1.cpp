// Phase 1 Crash Recovery Integration Tests
// Tests automatic view recreation, table resync, and dependency graph rebuild

#include "../../include/dbsp_cdc.hpp"
#include "../../include/dbsp_recovery.hpp"
#include "../../include/dbsp_crash_marker.hpp"
#include "../test_helpers.hpp"
#include "catch.hpp"
#include <filesystem>

using namespace dbsp_native;
using namespace duckdb;

TEST_CASE("Phase 1.1: View definitions persist across restarts",
          "[crash_recovery][phase1]") {
  DuckDB db(nullptr);
  Connection con(db);

  // Initialize recovery manager (disable auto-recovery for testing)
  auto &recovery_manager = get_recovery_manager();
  recovery_manager.set_recovery_enabled(false);  // Disable auto-recovery

  // Simulate first session: create views
  {
    auto &cdc_manager = get_cdc_manager();
    cdc_manager.reset();

    // Initialize persistence manually
    REQUIRE(recovery_manager.initialize_persistence(*con.context));

    // Create source table
    con.Query("CREATE TABLE orders (id INTEGER, amount INTEGER)");

    // Track table (need a transaction)
    con.BeginTransaction();
    if (!cdc_manager.track_table(*con.context, "orders")) {
      std::cerr << "track_table failed: " << cdc_manager.last_error() << std::endl;
    }
    REQUIRE(cdc_manager.track_table(*con.context, "orders"));
    con.Commit();

    // Create view (need transaction for persistence save)
    con.BeginTransaction();
    std::string view_sql = "SELECT * FROM orders WHERE amount > 100";
    REQUIRE(cdc_manager.create_view(*con.context, "high_value", view_sql));
    con.Commit();

    // Verify view exists
    REQUIRE(cdc_manager.view_exists("high_value"));

    // Check that view was saved to _dbsp_views
    auto result = con.Query("SELECT COUNT(*) FROM _dbsp_views WHERE name = 'high_value'");
    auto &mat = result->Cast<duckdb::MaterializedQueryResult>();
    REQUIRE(mat.GetValue(0, 0).GetValue<int64_t>() == 1);
  }

  // Simulate crash and recovery: reset CDC manager but keep database
  {
    auto &cdc_manager = get_cdc_manager();
    cdc_manager.reset();

    // Verify view doesn't exist in memory
    REQUIRE_FALSE(cdc_manager.view_exists("high_value"));

    // Perform recovery manually (re-enable recovery first)
    recovery_manager.set_recovery_enabled(true);
    con.BeginTransaction();
    REQUIRE(recovery_manager.recover_from_crash(*con.context, ""));
    con.Commit();

    // Verify view was recreated
    REQUIRE(cdc_manager.view_exists("high_value"));

    // Verify we can query the view (use CDC manager directly since extension isn't loaded)
    auto view_result = cdc_manager.query_view("high_value");
    REQUIRE(view_result != nullptr);
    // View should be empty since table wasn't synced before crash
    REQUIRE(view_result->empty());
  }
}

TEST_CASE("Phase 1.2: Multiple views with dependencies recover correctly",
          "[crash_recovery][phase1]") {
  DuckDB db(nullptr);
  Connection con(db);

  auto &recovery_manager = get_recovery_manager();
  auto &cdc_manager = get_cdc_manager();

  recovery_manager.set_recovery_enabled(false);  // Disable auto-recovery for testing
  cdc_manager.reset();

  // Initialize persistence
  REQUIRE(recovery_manager.initialize_persistence(*con.context));

  // Create source table
  con.Query("CREATE TABLE products (id INTEGER, price INTEGER, category VARCHAR)");

  // Track table (need a transaction)
  con.BeginTransaction();
  REQUIRE(cdc_manager.track_table(*con.context, "products"));
  con.Commit();

  // Create cascading views (need transactions)
  con.BeginTransaction();
  REQUIRE(cdc_manager.create_view(*con.context, "expensive",
                                  "SELECT * FROM products WHERE price > 1000"));
  con.Commit();

  con.BeginTransaction();
  REQUIRE(cdc_manager.create_view(*con.context, "luxury",
                                  "SELECT * FROM expensive WHERE price > 5000"));
  con.Commit();

  // Verify both views exist
  REQUIRE(cdc_manager.view_exists("expensive"));
  REQUIRE(cdc_manager.view_exists("luxury"));

  // Check dependencies are correct
  auto deps = cdc_manager.get_dependents("expensive");
  REQUIRE(deps.size() == 1);
  REQUIRE(deps[0] == "luxury");

  // Simulate crash
  cdc_manager.reset();
  REQUIRE_FALSE(cdc_manager.view_exists("expensive"));
  REQUIRE_FALSE(cdc_manager.view_exists("luxury"));

  // Recover manually (re-enable recovery first)
  recovery_manager.set_recovery_enabled(true);
  con.BeginTransaction();
  REQUIRE(recovery_manager.recover_from_crash(*con.context, ""));
  con.Commit();

  // Verify views recovered
  REQUIRE(cdc_manager.view_exists("expensive"));
  REQUIRE(cdc_manager.view_exists("luxury"));

  // Verify dependencies rebuilt
  deps = cdc_manager.get_dependents("expensive");
  REQUIRE(deps.size() == 1);
  REQUIRE(deps[0] == "luxury");
}

TEST_CASE("Phase 1.3: Table data resyncs after recovery",
          "[crash_recovery][phase1]") {
  DuckDB db(nullptr);
  Connection con(db);

  auto &recovery_manager = get_recovery_manager();
  auto &cdc_manager = get_cdc_manager();

  recovery_manager.set_recovery_enabled(false);  // Disable auto-recovery for testing
  cdc_manager.reset();

  // Initialize persistence
  REQUIRE(recovery_manager.initialize_persistence(*con.context));

  // Create and populate table
  con.Query("CREATE TABLE items (id INTEGER, value INTEGER)");
  con.Query("INSERT INTO items VALUES (1, 100), (2, 200), (3, 300)");

  // Track table and create view (need transactions)
  con.BeginTransaction();
  REQUIRE(cdc_manager.track_table(*con.context, "items"));
  con.Commit();

  con.BeginTransaction();
  REQUIRE(cdc_manager.create_view(*con.context, "high_items",
                                  "SELECT * FROM items WHERE value >= 200"));
  con.Commit();

  // Initial sync (need a transaction)
  con.BeginTransaction();
  REQUIRE(cdc_manager.sync_table(*con.context, "items"));
  con.Commit();

  // Verify view has correct data (use CDC manager directly)
  auto view_result = cdc_manager.query_view("high_items");
  REQUIRE(view_result != nullptr);
  REQUIRE(view_result->size() == 2);  // Should have rows 2 and 3

  // Add more data to table
  con.Query("INSERT INTO items VALUES (4, 400)");

  // Simulate crash before sync
  cdc_manager.reset();

  // Recover - should resync from table (re-enable recovery first)
  recovery_manager.set_recovery_enabled(true);
  con.BeginTransaction();
  REQUIRE(recovery_manager.recover_from_crash(*con.context, ""));
  con.Commit();

  // Verify view has ALL data (including item 4)
  view_result = cdc_manager.query_view("high_items");
  REQUIRE(view_result != nullptr);
  REQUIRE(view_result->size() == 3);  // Should have rows 2, 3, and 4
}

TEST_CASE("Phase 1.4: Crash markers work correctly",
          "[crash_recovery][phase1]") {
  // Create a temporary recovery directory
  std::string test_recovery_path = ".test_recovery_" + std::to_string(time(nullptr));

  // Clean up any existing test directory
  std::filesystem::remove_all(test_recovery_path);

  // Mark session start
  DBSPCrashMarker::mark_session_start(test_recovery_path);

  // Verify lock file exists
  std::string lock_path = test_recovery_path + "/.dbsp.lock";
  REQUIRE(std::filesystem::exists(lock_path));

  // Simulate crash detection (lock file still exists)
  REQUIRE(DBSPCrashMarker::detect_crash(test_recovery_path));

  // Verify crash was logged
  std::string crash_log_path = test_recovery_path + "/.dbsp.crash";
  REQUIRE(std::filesystem::exists(crash_log_path));

  // Mark session start again (after crash)
  DBSPCrashMarker::mark_session_start(test_recovery_path);
  REQUIRE(std::filesystem::exists(lock_path));

  // Mark clean session end
  DBSPCrashMarker::mark_session_end(test_recovery_path);

  // Verify lock file removed
  REQUIRE_FALSE(std::filesystem::exists(lock_path));

  // Verify no crash detected on next start
  REQUIRE_FALSE(DBSPCrashMarker::detect_crash(test_recovery_path));

  // Cleanup
  std::filesystem::remove_all(test_recovery_path);
}

TEST_CASE("Phase 1.5: Drop view removes from persistence",
          "[crash_recovery][phase1]") {
  DuckDB db(nullptr);
  Connection con(db);

  auto &recovery_manager = get_recovery_manager();
  auto &cdc_manager = get_cdc_manager();

  recovery_manager.set_recovery_enabled(false);  // Disable auto-recovery for testing
  cdc_manager.reset();

  // Initialize persistence
  REQUIRE(recovery_manager.initialize_persistence(*con.context));

  // Create source table
  con.Query("CREATE TABLE data (id INTEGER, value INTEGER)");

  // Track table (need a transaction)
  con.BeginTransaction();
  REQUIRE(cdc_manager.track_table(*con.context, "data"));
  con.Commit();

  // Create view (need transaction)
  con.BeginTransaction();
  REQUIRE(cdc_manager.create_view(*con.context, "test_view",
                                  "SELECT * FROM data WHERE value > 50"));
  con.Commit();

  // Verify saved to persistence
  auto result = con.Query("SELECT COUNT(*) FROM _dbsp_views WHERE name = 'test_view'");
  auto &mat = result->Cast<duckdb::MaterializedQueryResult>();
  REQUIRE(mat.GetValue(0, 0).GetValue<int64_t>() == 1);

  // Drop view (need transaction)
  con.BeginTransaction();
  REQUIRE(cdc_manager.drop_view("test_view", con.context.get()));
  con.Commit();

  // Verify removed from persistence
  result = con.Query("SELECT COUNT(*) FROM _dbsp_views WHERE name = 'test_view'");
  auto &mat2 = result->Cast<duckdb::MaterializedQueryResult>();
  REQUIRE(mat2.GetValue(0, 0).GetValue<int64_t>() == 0);

  // Simulate crash and recovery (re-enable recovery first)
  cdc_manager.reset();
  recovery_manager.set_recovery_enabled(true);
  con.BeginTransaction();
  REQUIRE(recovery_manager.recover_from_crash(*con.context, ""));
  con.Commit();

  // Verify view NOT recreated
  REQUIRE_FALSE(cdc_manager.view_exists("test_view"));
}

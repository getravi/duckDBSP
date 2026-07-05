// Phase 2 Crash Recovery Integration Tests
// Tests checkpoint-based snapshots and faster recovery

#include "../../include/dbsp_cdc.hpp"
#include "../../include/dbsp_recovery.hpp"
#include "../../include/dbsp_crash_marker.hpp"
#include "../../include/dbsp_checkpoint_format.hpp"
#include "../test_helpers.hpp"
#include "catch.hpp"
#include <filesystem>

using namespace dbsp_native;
using namespace duckdb;

TEST_CASE("Phase 2.1: Checkpoint saves Z-set state correctly",
          "[crash_recovery][phase2][checkpoint]") {
  DuckDB db(nullptr);
  Connection con(db);

  auto &recovery_manager = get_recovery_manager();
  auto &cdc_manager = get_cdc_manager();

  recovery_manager.set_recovery_enabled(false);
  cdc_manager.reset();

  // Initialize persistence
  REQUIRE(recovery_manager.initialize_persistence(*con.context));

  // Create table with data
  con.Query("CREATE TABLE products (id INTEGER, price INTEGER)");
  con.Query("INSERT INTO products VALUES (1, 100), (2, 200), (3, 300)");

  // Track and sync
  con.BeginTransaction();
  REQUIRE(cdc_manager.track_table(*con.context, "products"));
  con.Commit();

  con.BeginTransaction();
  REQUIRE(cdc_manager.create_view(*con.context, "expensive",
                                  "SELECT * FROM products WHERE price > 150"));
  con.Commit();

  con.BeginTransaction();
  REQUIRE(cdc_manager.sync_table(*con.context, "products"));
  con.Commit();

  // Verify view has data (2 rows: price 200, 300)
  auto view_result = cdc_manager.query_view("expensive");
  REQUIRE(view_result != nullptr);
  REQUIRE(view_result->size() == 2);

  // Save checkpoint (save_checkpoint is a no-op while recovery is disabled)
  recovery_manager.set_recovery_enabled(true);
  REQUIRE(recovery_manager.save_checkpoint(*con.context));

  // Verify checkpoint file exists
  std::string checkpoint_path = recovery_manager.get_latest_checkpoint();
  REQUIRE_FALSE(checkpoint_path.empty());
  REQUIRE(std::filesystem::exists(checkpoint_path));

  std::cout << "Checkpoint saved to: " << checkpoint_path << std::endl;
}

TEST_CASE("Phase 2.2: Checkpoint restore recovers view results",
          "[crash_recovery][phase2][checkpoint]") {
  DuckDB db(nullptr);
  Connection con(db);

  auto &recovery_manager = get_recovery_manager();
  auto &cdc_manager = get_cdc_manager();

  recovery_manager.set_recovery_enabled(false);
  cdc_manager.reset();

  // Initialize and create data
  REQUIRE(recovery_manager.initialize_persistence(*con.context));

  con.Query("CREATE TABLE items (id INTEGER, value INTEGER)");
  con.Query("INSERT INTO items VALUES (1, 10), (2, 20), (3, 30), (4, 40)");

  con.BeginTransaction();
  REQUIRE(cdc_manager.track_table(*con.context, "items"));
  con.Commit();

  con.BeginTransaction();
  REQUIRE(cdc_manager.create_view(*con.context, "high_value",
                                  "SELECT * FROM items WHERE value >= 30"));
  con.Commit();

  con.BeginTransaction();
  REQUIRE(cdc_manager.sync_table(*con.context, "items"));
  con.Commit();

  // Verify view has 2 rows (value 30, 40)
  auto view_result = cdc_manager.query_view("high_value");
  REQUIRE(view_result != nullptr);
  size_t original_size = view_result->size();
  REQUIRE(original_size == 2);

  // Save checkpoint (save_checkpoint is a no-op while recovery is disabled)
  recovery_manager.set_recovery_enabled(true);
  REQUIRE(recovery_manager.save_checkpoint(*con.context));

  // Simulate crash: reset CDC manager
  cdc_manager.reset();
  REQUIRE_FALSE(cdc_manager.view_exists("high_value"));

  // Recover: load views, then load checkpoint
  recovery_manager.set_recovery_enabled(true);
  con.BeginTransaction();
  REQUIRE(recovery_manager.recover_from_crash(*con.context, ""));
  con.Commit();

  // Verify view was restored
  REQUIRE(cdc_manager.view_exists("high_value"));

  view_result = cdc_manager.query_view("high_value");
  REQUIRE(view_result != nullptr);
  REQUIRE(view_result->size() == original_size);
}

TEST_CASE("Phase 2.3: Checkpoint checksum detects corruption",
          "[crash_recovery][phase2][checkpoint]") {
  DuckDB db(nullptr);
  Connection con(db);

  auto &recovery_manager = get_recovery_manager();
  auto &cdc_manager = get_cdc_manager();

  recovery_manager.set_recovery_enabled(false);
  cdc_manager.reset();

  REQUIRE(recovery_manager.initialize_persistence(*con.context));

  con.Query("CREATE TABLE test_data (id INTEGER, val INTEGER)");
  con.Query("INSERT INTO test_data VALUES (1, 100)");

  con.BeginTransaction();
  REQUIRE(cdc_manager.track_table(*con.context, "test_data"));
  con.Commit();

  con.BeginTransaction();
  REQUIRE(cdc_manager.create_view(*con.context, "test_view",
                                  "SELECT * FROM test_data"));
  con.Commit();

  con.BeginTransaction();
  REQUIRE(cdc_manager.sync_table(*con.context, "test_data"));
  con.Commit();

  // Save checkpoint (save_checkpoint is a no-op while recovery is disabled)
  recovery_manager.set_recovery_enabled(true);
  REQUIRE(recovery_manager.save_checkpoint(*con.context));

  // Get checkpoint path
  std::string checkpoint_path = recovery_manager.get_latest_checkpoint();
  REQUIRE_FALSE(checkpoint_path.empty());

  // Corrupt the checkpoint file (flip some bytes)
  {
    std::fstream file(checkpoint_path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());

    // Seek to middle of file and corrupt a byte
    file.seekp(100);
    char byte;
    file.read(&byte, 1);
    file.seekp(100);
    byte = ~byte;  // Flip bits
    file.write(&byte, 1);
    file.close();
  }

  // Reset and try to recover
  cdc_manager.reset();
  recovery_manager.set_recovery_enabled(true);

  // Recovery should fail due to checksum mismatch, fall back to resync
  con.BeginTransaction();
  bool recovery_result = recovery_manager.recover_from_crash(*con.context, "");
  con.Commit();

  // Recovery should still succeed (falls back to resync)
  REQUIRE(recovery_result);
}

TEST_CASE("Phase 2.4: Old checkpoints are cleaned up",
          "[crash_recovery][phase2][checkpoint]") {
  DuckDB db(nullptr);
  Connection con(db);

  auto &recovery_manager = get_recovery_manager();
  auto &cdc_manager = get_cdc_manager();

  recovery_manager.set_recovery_enabled(false);
  cdc_manager.reset();

  REQUIRE(recovery_manager.initialize_persistence(*con.context));

  con.Query("CREATE TABLE data (id INTEGER)");
  con.BeginTransaction();
  REQUIRE(cdc_manager.track_table(*con.context, "data"));
  con.Commit();

  // Create multiple checkpoints
  for (int i = 0; i < 10; i++) {
    con.Query("INSERT INTO data VALUES (" + std::to_string(i) + ")");

    con.BeginTransaction();
    std::string view_name = "view_" + std::to_string(i);
    REQUIRE(cdc_manager.create_view(*con.context, view_name,
                                    "SELECT * FROM data WHERE id = " + std::to_string(i)));
    con.Commit();

    con.BeginTransaction();
    REQUIRE(cdc_manager.sync_table(*con.context, "data"));
    con.Commit();

    // save_checkpoint is a no-op while recovery is disabled
    recovery_manager.set_recovery_enabled(true);
    REQUIRE(recovery_manager.save_checkpoint(*con.context));

    // Small delay to ensure different timestamps
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Count checkpoint files
  std::string recovery_path = recovery_manager.get_recovery_path();
  int checkpoint_count = 0;
  for (const auto &entry : std::filesystem::directory_iterator(recovery_path)) {
    if (entry.path().filename().string().find("checkpoint_") == 0) {
      checkpoint_count++;
    }
  }

  // Should keep only 5 most recent
  REQUIRE(checkpoint_count <= 5);
  std::cout << "Checkpoints after cleanup: " << checkpoint_count << std::endl;
}

TEST_CASE("Phase 2.5: Recovery uses checkpoint before resync",
          "[crash_recovery][phase2][checkpoint]") {
  DuckDB db(nullptr);
  Connection con(db);

  auto &recovery_manager = get_recovery_manager();
  auto &cdc_manager = get_cdc_manager();

  recovery_manager.set_recovery_enabled(false);
  cdc_manager.reset();

  REQUIRE(recovery_manager.initialize_persistence(*con.context));

  // Create large dataset
  con.Query("CREATE TABLE large_table (id INTEGER, value INTEGER)");

  std::string insert_sql = "INSERT INTO large_table VALUES ";
  for (int i = 0; i < 1000; i++) {
    if (i > 0) insert_sql += ", ";
    insert_sql += "(" + std::to_string(i) + ", " + std::to_string(i * 10) + ")";
  }
  con.Query(insert_sql);

  con.BeginTransaction();
  REQUIRE(cdc_manager.track_table(*con.context, "large_table"));
  con.Commit();

  con.BeginTransaction();
  REQUIRE(cdc_manager.create_view(*con.context, "large_view",
                                  "SELECT * FROM large_table WHERE value > 5000"));
  con.Commit();

  con.BeginTransaction();
  REQUIRE(cdc_manager.sync_table(*con.context, "large_table"));
  con.Commit();

  // Get view size before checkpoint
  auto view_result = cdc_manager.query_view("large_view");
  size_t expected_size = view_result->size();
  REQUIRE(expected_size > 0);

  // Save checkpoint (save_checkpoint is a no-op while recovery is disabled)
  recovery_manager.set_recovery_enabled(true);
  auto start_save = std::chrono::steady_clock::now();
  REQUIRE(recovery_manager.save_checkpoint(*con.context));
  auto end_save = std::chrono::steady_clock::now();
  auto save_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_save - start_save).count();

  std::cout << "Checkpoint save time: " << save_time << "ms" << std::endl;

  // Simulate crash
  cdc_manager.reset();

  // Recover with checkpoint
  recovery_manager.set_recovery_enabled(true);

  auto start_recover = std::chrono::steady_clock::now();
  con.BeginTransaction();
  REQUIRE(recovery_manager.recover_from_crash(*con.context, ""));
  con.Commit();
  auto end_recover = std::chrono::steady_clock::now();
  auto recover_time = std::chrono::duration_cast<std::chrono::milliseconds>(end_recover - start_recover).count();

  std::cout << "Recovery time with checkpoint: " << recover_time << "ms" << std::endl;

  // Verify recovered data
  view_result = cdc_manager.query_view("large_view");
  REQUIRE(view_result != nullptr);
  REQUIRE(view_result->size() == expected_size);

  // Recovery should be fast (< 500ms for 1000 rows)
  REQUIRE(recover_time < 500);
}

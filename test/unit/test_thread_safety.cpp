#include "catch.hpp"
#include "../../include/dbsp_cdc.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace dbsp_native;

TEST_CASE("Singleton thread-safety", "[thread_safety][singleton]") {
  // Test that get_cdc_manager() is thread-safe
  std::vector<std::thread> threads;
  std::vector<CDCManager*> managers;
  std::mutex managers_mutex;

  const int num_threads = 10;

  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back([&managers, &managers_mutex]() {
      CDCManager& mgr = get_cdc_manager();
      std::lock_guard<std::mutex> lock(managers_mutex);
      managers.push_back(&mgr);
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All threads should get the same singleton instance
  REQUIRE(managers.size() == num_threads);
  for (size_t i = 1; i < managers.size(); i++) {
    REQUIRE(managers[i] == managers[0]);
  }
}

TEST_CASE("Concurrent view creation", "[thread_safety][views]") {
  // Test concurrent view creation doesn't cause race conditions
  duckdb::DuckDB db(nullptr);
  duckdb::Connection conn(db);
  auto& context = *conn.context;

  // Create a test table
  conn.Query("CREATE TABLE test_table (id INTEGER, value INTEGER)");
  conn.Query("INSERT INTO test_table VALUES (1, 10), (2, 20), (3, 30)");

  CDCManager& manager = get_cdc_manager();
  manager.track_table(context, "test_table");

  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};
  std::atomic<int> error_count{0};

  const int num_threads = 5;

  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back([&manager, &context, i, &success_count, &error_count]() {
      std::string view_name = "view_" + std::to_string(i);
      std::string sql = "SELECT * FROM test_table WHERE id = " + std::to_string(i + 1);

      if (manager.create_view(context, view_name, sql)) {
        success_count++;
      } else {
        error_count++;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All view creations should succeed
  REQUIRE(success_count == num_threads);
  REQUIRE(error_count == 0);

  // Verify all views were created
  for (int i = 0; i < num_threads; i++) {
    std::string view_name = "view_" + std::to_string(i);
    auto views = manager.list_views();
    bool found = false;
    for (const auto& v : views) {
      if (v == view_name) {
        found = true;
        break;
      }
    }
    REQUIRE(found);
  }

  // Cleanup
  for (int i = 0; i < num_threads; i++) {
    manager.drop_view("view_" + std::to_string(i));
  }
  manager.untrack_table("test_table");
}

TEST_CASE("Concurrent table tracking", "[thread_safety][tracking]") {
  // Test concurrent table tracking
  duckdb::DuckDB db(nullptr);
  duckdb::Connection conn(db);
  auto& context = *conn.context;

  // Create multiple test tables
  const int num_tables = 5;
  for (int i = 0; i < num_tables; i++) {
    std::string table_name = "table_" + std::to_string(i);
    conn.Query("CREATE TABLE " + table_name + " (id INTEGER, value INTEGER)");
    conn.Query("INSERT INTO " + table_name + " VALUES (1, 10)");
  }

  CDCManager& manager = get_cdc_manager();
  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  for (int i = 0; i < num_tables; i++) {
    threads.emplace_back([&manager, &context, i, &success_count]() {
      std::string table_name = "table_" + std::to_string(i);
      if (manager.track_table(context, table_name)) {
        success_count++;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All tracking operations should succeed
  REQUIRE(success_count == num_tables);

  // Cleanup
  for (int i = 0; i < num_tables; i++) {
    manager.untrack_table("table_" + std::to_string(i));
  }
}

TEST_CASE("Concurrent sync operations", "[thread_safety][sync]") {
  // Test concurrent sync operations
  duckdb::DuckDB db(nullptr);
  duckdb::Connection conn(db);
  auto& context = *conn.context;

  // Create test table
  conn.Query("CREATE TABLE sync_table (id INTEGER, value INTEGER)");
  conn.Query("INSERT INTO sync_table VALUES (1, 10), (2, 20)");

  CDCManager& manager = get_cdc_manager();
  manager.track_table(context, "sync_table");
  manager.create_view(context, "sync_view", "SELECT * FROM sync_table");

  std::vector<std::thread> threads;
  std::atomic<int> sync_count{0};

  const int num_syncs = 10;

  for (int i = 0; i < num_syncs; i++) {
    threads.emplace_back([&manager, &context, &sync_count, i]() {
      // Each thread adds data and syncs
      duckdb::Connection thread_conn(*context.db);
      thread_conn.Query("INSERT INTO sync_table VALUES (" +
                       std::to_string(100 + i) + ", " +
                       std::to_string(200 + i) + ")");

      manager.sync_table(*thread_conn.context, "sync_table");
      sync_count++;
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All syncs should complete
  REQUIRE(sync_count == num_syncs);

  // Cleanup
  manager.drop_view("sync_view");
  manager.untrack_table("sync_table");
}

TEST_CASE("Parallel sync flag thread-safety", "[thread_safety][parallel_sync]") {
  // Test thread-safe access to parallel sync flag
  CDCManager& manager = get_cdc_manager();

  std::vector<std::thread> threads;
  std::atomic<int> read_count{0};
  std::atomic<int> write_count{0};

  const int num_readers = 10;
  const int num_writers = 5;

  // Reader threads
  for (int i = 0; i < num_readers; i++) {
    threads.emplace_back([&manager, &read_count]() {
      for (int j = 0; j < 100; j++) {
        bool enabled = manager.get_parallel_sync();
        (void)enabled; // Use the value
        read_count++;
      }
    });
  }

  // Writer threads
  for (int i = 0; i < num_writers; i++) {
    threads.emplace_back([&manager, &write_count, i]() {
      for (int j = 0; j < 20; j++) {
        manager.set_parallel_sync(i % 2 == 0);
        write_count++;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All operations should complete
  REQUIRE(read_count == num_readers * 100);
  REQUIRE(write_count == num_writers * 20);
}

TEST_CASE("Concurrent drop operations", "[thread_safety][drop]") {
  // Test concurrent drop operations with proper synchronization
  duckdb::DuckDB db(nullptr);
  duckdb::Connection conn(db);
  auto& context = *conn.context;

  conn.Query("CREATE TABLE drop_test (id INTEGER)");

  CDCManager& manager = get_cdc_manager();
  manager.track_table(context, "drop_test");

  // Create views sequentially
  const int num_views = 10;
  for (int i = 0; i < num_views; i++) {
    std::string view_name = "drop_view_" + std::to_string(i);
    manager.create_view(context, view_name, "SELECT * FROM drop_test");
  }

  // Drop views concurrently
  std::vector<std::thread> threads;
  std::atomic<int> drop_count{0};

  for (int i = 0; i < num_views; i++) {
    threads.emplace_back([&manager, i, &drop_count]() {
      std::string view_name = "drop_view_" + std::to_string(i);
      if (manager.drop_view(view_name)) {
        drop_count++;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All drops should succeed
  REQUIRE(drop_count == num_views);

  // Verify all views are gone
  auto remaining_views = manager.list_views();
  for (int i = 0; i < num_views; i++) {
    std::string view_name = "drop_view_" + std::to_string(i);
    bool found = false;
    for (const auto& v : remaining_views) {
      if (v == view_name) {
        found = true;
        break;
      }
    }
    REQUIRE_FALSE(found);
  }

  manager.untrack_table("drop_test");
}

TEST_CASE("propagate_changes thread-safety", "[thread_safety][propagate]") {
  // Test that propagate_changes is thread-safe
  duckdb::DuckDB db(nullptr);
  duckdb::Connection conn(db);
  auto& context = *conn.context;

  conn.Query("CREATE TABLE prop_table (id INTEGER, val INTEGER)");
  conn.Query("INSERT INTO prop_table VALUES (1, 100)");

  CDCManager& manager = get_cdc_manager();
  manager.track_table(context, "prop_table");
  manager.create_view(context, "prop_view", "SELECT * FROM prop_table");

  std::vector<std::thread> threads;
  const int num_propagations = 5;

  for (int i = 0; i < num_propagations; i++) {
    threads.emplace_back([&manager, &conn, i]() {
      // Insert data
      duckdb::Connection thread_conn(*conn.context->db);
      thread_conn.Query("INSERT INTO prop_table VALUES (" +
                       std::to_string(i + 10) + ", " +
                       std::to_string(i * 100) + ")");

      // Sync to trigger propagation
      manager.sync_table(*thread_conn.context, "prop_table");

      // Small delay to interleave operations
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // Verify no crashes occurred and data is consistent
  auto result = conn.Query("SELECT COUNT(*) FROM prop_table");
  REQUIRE_FALSE(result->HasError());
  REQUIRE(result->RowCount() == 1);

  // Cleanup
  manager.drop_view("prop_view");
  manager.untrack_table("prop_table");
}

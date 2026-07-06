#include "catch.hpp"
#include "../../include/dbsp_cdc.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace dbsp_native;

TEST_CASE("Singleton thread-safety", "[thread_safety][singleton]") {
  // Registry get_or_create must be thread-safe and return one manager per
  // instance (Phase D1)
  duckdb::DuckDB db(nullptr);
  std::vector<std::thread> threads;
  std::vector<CDCManager*> managers;
  std::mutex managers_mutex;

  const int num_threads = 10;

  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back([&managers, &managers_mutex, &db]() {
      CDCManager& mgr = get_cdc_manager(*db.instance);
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

  CDCManager& manager = get_cdc_manager(*db.instance);

  // track_table needs an active DuckDB transaction for catalog access
  conn.BeginTransaction();
  bool track_ok = manager.track_table(context, "test_table");
  conn.Commit();
  REQUIRE(track_ok);

  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};
  std::atomic<int> error_count{0};

  const int num_threads = 5;

  // Threads only use already-tracked table (no catalog access needed)
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
  // Test concurrent table tracking (serialized through CDCManager mutex)
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

  CDCManager& manager = get_cdc_manager(*db.instance);
  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};

  // Start a transaction that stays active while all threads run
  // CDCManager mutex serializes access to the shared context
  conn.BeginTransaction();

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

  conn.Commit();

  // All tracking operations should succeed
  REQUIRE(success_count == num_tables);

  // Cleanup
  for (int i = 0; i < num_tables; i++) {
    manager.untrack_table("table_" + std::to_string(i));
  }
}

TEST_CASE("Concurrent list_views thread-safety", "[thread_safety][sync]") {
  // Test that concurrent list_views() calls are safe while views are being created
  duckdb::DuckDB db(nullptr);
  duckdb::Connection conn(db);
  auto& context = *conn.context;

  conn.Query("CREATE TABLE list_test_table (id INTEGER)");

  CDCManager& manager = get_cdc_manager(*db.instance);

  conn.BeginTransaction();
  manager.track_table(context, "list_test_table");
  conn.Commit();

  std::vector<std::thread> threads;
  std::atomic<int> list_count{0};
  std::atomic<int> create_count{0};

  // Create reader threads (list_views)
  for (int i = 0; i < 5; i++) {
    threads.emplace_back([&manager, &list_count]() {
      for (int j = 0; j < 10; j++) {
        auto views = manager.list_views();
        (void)views;
        list_count++;
      }
    });
  }

  // Create writer threads (create/drop views)
  for (int i = 0; i < 5; i++) {
    threads.emplace_back([&manager, &context, i, &create_count]() {
      std::string view_name = "list_view_" + std::to_string(i);
      if (manager.create_view(context, view_name, "SELECT id FROM list_test_table")) {
        create_count++;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All list and create operations should complete
  REQUIRE(list_count == 50);
  REQUIRE(create_count == 5);

  // Cleanup
  for (int i = 0; i < 5; i++) {
    manager.drop_view("list_view_" + std::to_string(i));
  }
  manager.untrack_table("list_test_table");
}

TEST_CASE("Parallel sync flag thread-safety", "[thread_safety][parallel_sync]") {
  // Test thread-safe access to parallel sync flag
  duckdb::DuckDB db(nullptr);
  CDCManager& manager = get_cdc_manager(*db.instance);

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

  CDCManager& manager = get_cdc_manager(*db.instance);

  // Setup: track table and create views (needs active transaction)
  conn.BeginTransaction();
  manager.track_table(context, "drop_test");

  // Create views sequentially
  const int num_views = 10;
  for (int i = 0; i < num_views; i++) {
    std::string view_name = "drop_view_" + std::to_string(i);
    manager.create_view(context, view_name, "SELECT * FROM drop_test");
  }
  conn.Commit();

  // Drop views concurrently (drop_view doesn't use DuckDB catalog)
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

TEST_CASE("Auto-sync and parallel-sync flag concurrency", "[thread_safety][flags]") {
  // Test concurrent access to CDCManager state flags
  duckdb::DuckDB db(nullptr);
  CDCManager& manager = get_cdc_manager(*db.instance);

  std::vector<std::thread> threads;
  std::atomic<int> ops_count{0};

  const int num_threads = 10;

  // Mix of auto-sync and parallel-sync read/write from multiple threads
  for (int i = 0; i < num_threads; i++) {
    threads.emplace_back([&manager, i, &ops_count]() {
      for (int j = 0; j < 50; j++) {
        if (j % 2 == 0) {
          // Toggle auto-sync
          if (i % 2 == 0) {
            manager.enable_auto_sync();
          } else {
            manager.disable_auto_sync();
          }
          // Read auto-sync state
          bool enabled = manager.is_auto_sync_enabled();
          (void)enabled;
        } else {
          // Toggle parallel sync
          manager.set_parallel_sync(j % 3 == 0);
          bool ps = manager.get_parallel_sync();
          (void)ps;
        }
        ops_count++;
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  REQUIRE(ops_count == num_threads * 50);

  // Reset to clean state
  manager.disable_auto_sync();
  manager.set_parallel_sync(false);
}

TEST_CASE("Concurrent scan_view during change propagation",
          "[thread_safety][scan]") {
  // dbsp_query reads views through scan_view, which must hold the read
  // locks for the whole traversal: a writer mutating view state mid-scan
  // is undefined behavior (the pre-fix code scanned via an unlocked
  // get_view pointer). Readers hammer scan_view while a writer pushes
  // deltas through propagate_changes-equivalent syncs.
  duckdb::DuckDB db(nullptr);
  duckdb::Connection conn(db);
  auto &context = *conn.context;

  conn.Query("CREATE TABLE scan_t (id INTEGER, val INTEGER)");
  conn.Query("INSERT INTO scan_t VALUES (1, 10), (2, 20)");

  CDCManager &manager = get_cdc_manager(*db.instance);
  manager.reset();

  conn.BeginTransaction();
  REQUIRE(manager.track_table(context, "scan_t"));
  REQUIRE(manager.create_view(context, "scan_v",
                              "SELECT id, val FROM scan_t WHERE val > 0"));
  conn.Commit();
  conn.BeginTransaction();
  REQUIRE(manager.sync_table(context, "scan_t"));
  conn.Commit();

  std::atomic<bool> stop{false};
  std::atomic<int> scan_errors{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; r++) {
    readers.emplace_back([&]() {
      while (!stop.load()) {
        size_t rows = 0;
        bool ok = manager.scan_view(
            "scan_v", [&](const DuckDBRow &row, Weight w) {
              // Row must be internally consistent (2 columns, sane weight)
              if (row.columns.size() != 2 || w == 0) {
                scan_errors.fetch_add(1);
              }
              rows += static_cast<size_t>(w > 0 ? w : 0);
            });
        if (!ok) {
          scan_errors.fetch_add(1);
        }
      }
    });
  }

  // Writer: keep mutating the table and syncing deltas through the view
  for (int i = 3; i < 200; i++) {
    conn.Query("INSERT INTO scan_t VALUES (" + std::to_string(i) + ", " +
               std::to_string(i * 10) + ")");
    if (i % 3 == 0) {
      conn.Query("DELETE FROM scan_t WHERE id = " + std::to_string(i - 2));
    }
    conn.BeginTransaction();
    manager.sync_table(context, "scan_t");
    conn.Commit();
  }
  stop.store(true);
  for (auto &t : readers) {
    t.join();
  }

  REQUIRE(scan_errors.load() == 0);
}

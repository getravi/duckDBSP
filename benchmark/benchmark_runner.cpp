#include <chrono>
#include <duckdb.hpp>
#include <duckdb/main/extension/extension_loader.hpp>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace std;
using namespace duckdb;

extern "C" void dbsp_duckdb_cpp_init(duckdb::ExtensionLoader &loader);

class BenchmarkRunner {
  DuckDB db;
  Connection con;

public:
  BenchmarkRunner() : db(nullptr), con(db) {}

  void setup() {
    // Register extension directly
    try {
      duckdb::ExtensionLoader loader(*db.instance, "dbsp");
      dbsp_duckdb_cpp_init(loader);
    } catch (const std::exception &e) {
      cerr << "Failed to register extension: " << e.what() << endl;
    }

    con.Query("CREATE TABLE t1 (id INTEGER, val INTEGER)");
    con.Query("CREATE TABLE t2 (id INTEGER, val INTEGER)");
  }

  void teardown() {
    con.Query("DROP VIEW IF EXISTS v_t1");
    con.Query("DROP VIEW IF EXISTS v_t2");
    con.Query("DROP TABLE IF EXISTS t1");
    con.Query("DROP TABLE IF EXISTS t2");
  }

  double measure_ingestion(int num_rows) {
    auto start = chrono::high_resolution_clock::now();
    con.Query("BEGIN TRANSACTION");
    for (int i = 0; i < num_rows; ++i) {
      con.Query("INSERT INTO t1 VALUES (" + to_string(i) + ", " +
                to_string(i * 10) + ")");
    }
    con.Query("COMMIT");
    auto end = chrono::high_resolution_clock::now();
    return chrono::duration<double>(end - start).count();
  }

  double measure_view_maintenance(string view_def, string table_name,
                                  int batch_size) {
    // Setup view
    // 1. Track table
    con.Query("SELECT * FROM dbsp_track('" + table_name + "')");

    // 3. Create View
    string create_view_sql = "SELECT * FROM dbsp_create_view('v_" + table_name +
                             "', '" + view_def + "')";
    auto result = con.Query(create_view_sql);
    if (result->HasError()) {
      cerr << "Error creating view: " << result->GetError() << endl;
      return -1;
    }

    // Insert batch
    con.Query("BEGIN TRANSACTION");
    for (int i = 0; i < batch_size; ++i) {
      con.Query("INSERT INTO " + table_name + " VALUES (" + to_string(i) +
                ", " + to_string(i) + ")");
    }
    con.Query("COMMIT");

    // Measure maintenance (dbsp_sync)
    auto start = chrono::high_resolution_clock::now();
    result = con.Query("SELECT * FROM dbsp_sync('" + table_name + "')");
    auto end = chrono::high_resolution_clock::now();

    if (result->HasError()) {
      cerr << "Error syncing: " << result->GetError() << endl;
    }

    // Cleanup
    con.Query("DELETE FROM " + table_name);
    con.Query("SELECT * FROM dbsp_drop('v_" + table_name + "')");

    return chrono::duration<double>(end - start).count();
  }
};

int main() {
  BenchmarkRunner runner;
  runner.setup();

  cout << "Running Benchmarks..." << endl;
  cout << "------------------------------------------------" << endl;
  cout << fixed << setprecision(4);

  // 1. Ingestion (Pure insert, no tracking)
  cout << "Benchmarking Raw Ingestion (10000 rows)... ";
  double t_ingest = runner.measure_ingestion(10000);
  cout << t_ingest << " s (" << (10000 / t_ingest) << " rows/s)" << endl;

  // 2. View Maintenance
  int batch = 10000;

  // 2a. Simple Projection
  cout << "Benchmarking Maintenance (Projection, batch=" << batch << ")... ";
  double t_proj = runner.measure_view_maintenance("SELECT id, val * 2 FROM t1",
                                                  "t1", batch);
  cout << t_proj << " s (" << (batch / t_proj) << " rows/s)" << endl;

  // 2b. Aggregation
  cout << "Benchmarking Maintenance (Aggregation, batch=" << batch << ")... ";
  double t_agg = runner.measure_view_maintenance(
      "SELECT val, COUNT(*) FROM t2 GROUP BY val", "t2", batch);
  cout << t_agg << " s (" << (batch / t_agg) << " rows/s)" << endl;

  return 0;
}

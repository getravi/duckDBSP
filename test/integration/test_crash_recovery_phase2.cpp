// Crash Recovery: replay-based restore
//
// Recovery rebuilds all Z-set state by replaying DuckDB's committed
// storage through each view's circuit (load_views → create_view →
// tracked-table scan). There is deliberately no snapshot/WAL state
// restore: a sink-only restore cannot reconstruct internal circuit-node
// state (aggregate groups, join indexes, sort/limit multisets, recursive
// dedup), and replaying persisted deltas double-applies on top of
// baselines scanned from committed storage. These tests pin both the
// happy path and the failure modes that killed the old checkpoint/WAL
// subsystem.

#include "../../include/dbsp_cdc.hpp"
#include "../../include/dbsp_recovery.hpp"
#include "../../include/dbsp_crash_marker.hpp"
#include "../test_helpers.hpp"
#include "catch.hpp"
#include <filesystem>

using namespace dbsp_native;
using namespace duckdb;

TEST_CASE("Replay restore: filter view content survives recovery",
          "[crash_recovery][phase2][restore]") {
  DuckDB db(nullptr);
  Connection con(db);

  auto &recovery_manager = get_recovery_manager();
  auto &cdc_manager = get_cdc_manager(*db.instance);

  recovery_manager.set_recovery_enabled(false);
  cdc_manager.reset();
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

  auto view_result = cdc_manager.query_view("high_value");
  REQUIRE(view_result != nullptr);
  REQUIRE(view_result->size() == 2);

  // Simulated crash + recovery (replay from DuckDB storage)
  recovery_manager.set_recovery_enabled(true);
  cdc_manager.reset();
  REQUIRE_FALSE(cdc_manager.view_exists("high_value"));

  con.BeginTransaction();
  REQUIRE(recovery_manager.recover_from_crash(*con.context, ""));
  con.Commit();

  REQUIRE(cdc_manager.view_exists("high_value"));
  view_result = cdc_manager.query_view("high_value");
  REQUIRE(view_result != nullptr);
  REQUIRE(view_result->size() == 2);
}

TEST_CASE("Replay restore: aggregate view stays correct after first "
          "post-recovery delta",
          "[crash_recovery][phase2][restore][restore_audit]") {
  DuckDB db(nullptr);
  Connection con(db);

  auto &recovery_manager = get_recovery_manager();
  auto &cdc_manager = get_cdc_manager(*db.instance);

  recovery_manager.set_recovery_enabled(false);
  cdc_manager.reset();
  REQUIRE(recovery_manager.initialize_persistence(*con.context));

  con.Query("CREATE TABLE nums (g VARCHAR, v INTEGER)");
  con.Query("INSERT INTO nums VALUES ('a', 10), ('a', 20), ('b', 5)");

  con.BeginTransaction();
  REQUIRE(cdc_manager.track_table(*con.context, "nums"));
  con.Commit();
  con.BeginTransaction();
  REQUIRE(cdc_manager.create_view(*con.context, "v_sum",
                                  "SELECT g, SUM(v) AS s FROM nums GROUP BY g"));
  con.Commit();
  con.BeginTransaction();
  REQUIRE(cdc_manager.sync_table(*con.context, "nums"));
  con.Commit();

  auto *before = cdc_manager.query_view("v_sum");
  REQUIRE(before != nullptr);
  REQUIRE(before->size() == 2); // (a,30) (b,5)

  // Simulated crash + recovery
  recovery_manager.set_recovery_enabled(true);
  cdc_manager.reset();
  con.BeginTransaction();
  REQUIRE(recovery_manager.recover_from_crash(*con.context, ""));
  con.Commit();

  auto *restored = cdc_manager.query_view("v_sum");
  REQUIRE(restored != nullptr);
  REQUIRE(restored->size() == 2);

  // First post-recovery delta: group 'a' must become 31 — internal
  // aggregate state has to have been rebuilt by replay, not left empty
  con.Query("INSERT INTO nums VALUES ('a', 1)");
  con.BeginTransaction();
  REQUIRE(cdc_manager.sync_table(*con.context, "nums"));
  con.Commit();

  auto *after = cdc_manager.query_view("v_sum");
  REQUIRE(after != nullptr);
  REQUIRE(after->size() == 2); // still one row per group
  bool found_31 = false;
  for (const auto &[row, w] : *after) {
    if (row.columns[0].ToString() == "a") {
      found_31 = row.columns[1].ToString() == "31" && w == 1;
    }
  }
  REQUIRE(found_31);
}

TEST_CASE("Replay restore: ordered view still scans its rows after recovery",
          "[crash_recovery][phase2][restore][restore_audit]") {
  DuckDB db(nullptr);
  Connection con(db);

  auto &recovery_manager = get_recovery_manager();
  auto &cdc_manager = get_cdc_manager(*db.instance);

  recovery_manager.set_recovery_enabled(false);
  cdc_manager.reset();
  REQUIRE(recovery_manager.initialize_persistence(*con.context));

  con.Query("CREATE TABLE st (id INTEGER, val INTEGER)");
  con.Query("INSERT INTO st VALUES (1, 30), (2, 10), (3, 20)");

  con.BeginTransaction();
  REQUIRE(cdc_manager.track_table(*con.context, "st"));
  con.Commit();
  con.BeginTransaction();
  REQUIRE(cdc_manager.create_view(*con.context, "v_ord",
                                  "SELECT val, id FROM st ORDER BY val DESC"));
  con.Commit();
  con.BeginTransaction();
  REQUIRE(cdc_manager.sync_table(*con.context, "st"));
  con.Commit();

  auto count_scanned = [&]() {
    size_t n = 0;
    const auto *view = cdc_manager.get_view("v_ord");
    REQUIRE(view != nullptr);
    view->scan([&](const DuckDBRow &, Weight w) { n += (w > 0) ? w : 0; });
    return n;
  };
  REQUIRE(count_scanned() == 3);

  recovery_manager.set_recovery_enabled(true);
  cdc_manager.reset();
  con.BeginTransaction();
  REQUIRE(recovery_manager.recover_from_crash(*con.context, ""));
  con.Commit();

  // dbsp_query consumes scan(): the embedded sort view's multiset must
  // have been rebuilt by replay, not left empty next to a populated sink
  REQUIRE(cdc_manager.query_view("v_ord") != nullptr);
  REQUIRE(cdc_manager.query_view("v_ord")->size() == 3);
  REQUIRE(count_scanned() == 3);
}

TEST_CASE("End-to-end crash recovery across database restarts",
          "[crash_recovery][phase2][restore][e2e]") {
  std::filesystem::remove_all(".dbsp_recovery");
  std::filesystem::remove("test_e2e.db");

  // Session 1: normal operation, then simulated crash (no session end)
  {
    DuckDB db("test_e2e.db");
    Connection con(db);

    auto &cdc_manager = get_cdc_manager(*db.instance);
    auto &recovery_manager = get_recovery_manager();
    cdc_manager.reset();

    REQUIRE(recovery_manager.initialize_persistence(*con.context));
    recovery_manager.mark_session_start();

    con.Query("CREATE TABLE accounts (id INTEGER, balance DOUBLE)");
    con.BeginTransaction();
    REQUIRE(cdc_manager.track_table(*con.context, "accounts"));
    REQUIRE(cdc_manager.create_view(
        *con.context, "high_balance",
        "SELECT id, balance FROM accounts WHERE balance > 1000"));
    con.Commit();

    con.Query("INSERT INTO accounts VALUES (1, 1500), (2, 2500), (3, 500)");
    con.BeginTransaction();
    REQUIRE(cdc_manager.sync_table(*con.context, "accounts"));
    con.Commit();

    REQUIRE(cdc_manager.query_view("high_balance")->size() == 2);
    // No mark_session_end: simulates a crash
  }

  // Session 2: crash detected; replay restores committed state
  {
    DuckDB db("test_e2e.db");
    Connection con(db);

    auto &cdc_manager = get_cdc_manager(*db.instance);
    cdc_manager.clear_all_state();

    auto &recovery_manager = get_recovery_manager();
    REQUIRE(DBSPCrashMarker::detect_crash(".dbsp_recovery"));
    con.BeginTransaction(); // catalog access during view replay
    REQUIRE(recovery_manager.recover_from_crash(*con.context, "test_e2e.db"));
    con.Commit();

    REQUIRE(cdc_manager.view_exists("high_balance"));
    // Replay of committed storage: ids 1 and 2 pass the predicate
    REQUIRE(cdc_manager.query_view("high_balance")->size() == 2);

    recovery_manager.mark_session_end();
  }

  // Session 3: clean restart
  {
    DuckDB db("test_e2e.db");
    Connection con(db);

    auto &cdc_manager = get_cdc_manager(*db.instance);
    cdc_manager.clear_all_state();

    auto &recovery_manager = get_recovery_manager();
    REQUIRE_FALSE(DBSPCrashMarker::detect_crash(".dbsp_recovery"));
    con.BeginTransaction();
    REQUIRE(recovery_manager.recover_from_crash(*con.context, "test_e2e.db"));
    con.Commit();

    REQUIRE(cdc_manager.view_exists("high_balance"));
    REQUIRE(cdc_manager.query_view("high_balance")->size() == 2);

    recovery_manager.mark_session_end();
  }

  std::filesystem::remove_all(".dbsp_recovery");
  std::filesystem::remove("test_e2e.db");
}

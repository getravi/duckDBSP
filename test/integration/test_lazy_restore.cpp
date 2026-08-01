// Task 1 (lazy-restore-ntile plan): lazy per-view checkpoint restore
// (D-lazy).
//
// The D3b checkpoint fast path (load_from_duck_table) used to decode
// every checkpointed view's node + sink blobs during the load call
// itself. With dbsp_lazy_restore ON (default), it instead cold-creates
// each view and stashes its already-read blobs undecoded
// (CDCManager::pending_restore_) -- realize_pending_view[_locked] decodes
// a view's stash on first need: a query (scan_view/query_view), an
// incoming delta reaching it or a pending ancestor of it
// (propagate_changes's pre-pass), a view-on-view replay/arrangement
// backfill reading its result (create_view/register_arrangements), or a
// save re-saving the checkpoint (verbatim for anything still pending).
//
// These tests prove:
//   1. Touching one view via dbsp_query decodes ONLY that view's chain
//      (g_lazy_view_decodes counter), leaving siblings pending.
//   2. A delta on a pending view's source realizes it before applying --
//      differential correctness vs a continuously-live twin.
//   3. save-after-partial-realization round-trips: the still-pending
//      view's re-saved (verbatim) blobs decode correctly on the next
//      load, same as the realized view's freshly re-encoded blobs.
//   4. dbsp_lazy_restore(false) reproduces the pre-D-lazy eager behavior
//      (0 pending, no lazy-path decodes at all).

#include "catch.hpp"
#include "../test_helpers.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace dbsp_test;

namespace {

// Snapshot a live view's exact (row, weight) content via scan_view --
// realizes it as a side effect (scan_view calls realize_pending_view), so
// callers that want to observe pending state must check it BEFORE calling
// this on the view in question.
std::vector<std::pair<std::vector<std::string>, int64_t>>
snapshotView(DuckDBTestHarness &db, const std::string &name) {
    std::vector<std::pair<std::vector<std::string>, int64_t>> out;
    bool ok = db.manager().scan_view(
        name, [&](const dbsp_native::DuckDBRow &row, dbsp_native::Weight w) {
            std::vector<std::string> cols;
            cols.reserve(row.columns.size());
            for (const auto &v : row.columns) {
                cols.push_back(v.ToString());
            }
            out.emplace_back(std::move(cols), w);
        });
    REQUIRE(ok);
    std::sort(out.begin(), out.end());
    return out;
}

void setupItemsTable(DuckDBTestHarness &db) {
    db.exec("CREATE TABLE items (id INTEGER, cat VARCHAR, value INTEGER)");
    db.exec("SELECT * FROM dbsp_track('items')");
    db.exec("INSERT INTO items VALUES (1, 'a', 10), (2, 'a', 20), "
            "(3, 'b', 30), (4, 'b', 40)");
    db.exec("SELECT * FROM dbsp_sync('items')");
    db.exec("SELECT * FROM dbsp_use_planner(true)");
}

} // namespace

TEST_CASE("lazy restore: first touch decodes only that view's chain",
          "[integration][checkpoint][lazy_restore]") {
    DuckDBTestHarness db;
    setupItemsTable(db);

    db.exec("SELECT * FROM dbsp_create_view('v_sum', "
            "'SELECT cat, SUM(value) AS s FROM items GROUP BY cat')");
    db.exec("SELECT * FROM dbsp_create_view('v_filt', "
            "'SELECT * FROM items WHERE value >= 20')");
    db.exec("SELECT * FROM dbsp_create_view('v_count', "
            "'SELECT cat, COUNT(*) AS n FROM items GROUP BY cat')");
    REQUIRE(db.manager().get_view("v_sum")->checkpointable());
    REQUIRE(db.manager().get_view("v_filt")->checkpointable());
    REQUIRE(db.manager().get_view("v_count")->checkpointable());

    auto pre_sum = snapshotView(db, "v_sum");
    auto pre_filt = snapshotView(db, "v_filt");
    auto pre_count = snapshotView(db, "v_count");

    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_sum')")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_filt')")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_count')")->HasError());

    auto load_result = db.query("SELECT * FROM dbsp_load()");
    REQUIRE_FALSE(load_result->HasError());
    const std::string load_msg = load_result->GetValue(0, 0).ToString();
    REQUIRE(load_msg.find("3 from checkpoint") != std::string::npos);
    REQUIRE(load_msg.find("3 pending lazy restore") != std::string::npos);

    // Lazy restore ON by default: all three views are pending immediately
    // after load -- nothing decoded yet.
    REQUIRE(db.manager().pending_restore_count() == 3);
    const size_t decodes_before = dbsp_native::g_lazy_view_decodes.load();

    // get_view_info() (dbsp_views()) must report the real row count for a
    // still-pending view -- read from the stashed sink blob's length
    // prefix -- WITHOUT realizing it (no decode, still pending after).
    REQUIRE(db.manager().get_view_info("v_sum").row_count == pre_sum.size());
    REQUIRE(db.manager().get_view_info("v_filt").row_count == pre_filt.size());
    REQUIRE(db.manager().get_view_info("v_count").row_count ==
            pre_count.size());
    REQUIRE(dbsp_native::g_lazy_view_decodes.load() == decodes_before);
    REQUIRE(db.manager().pending_restore_count() == 3);

    // Touch exactly one view via the dbsp_query path (QueryFunc ->
    // scan_view -> realize_pending_view).
    auto q = db.query("SELECT * FROM dbsp_query('v_sum') ORDER BY cat");
    REQUIRE_FALSE(q->HasError());

    // Exactly one decode happened, and exactly one view left pending.
    REQUIRE(dbsp_native::g_lazy_view_decodes.load() == decodes_before + 1);
    REQUIRE(db.manager().pending_restore_count() == 2);

    // The touched view is correct and no longer pending.
    REQUIRE(snapshotView(db, "v_sum") == pre_sum);
    REQUIRE(dbsp_native::g_lazy_view_decodes.load() == decodes_before + 1);

    // Touching the other two now realizes them (one decode each) and
    // their content is correct too.
    REQUIRE(snapshotView(db, "v_filt") == pre_filt);
    REQUIRE(dbsp_native::g_lazy_view_decodes.load() == decodes_before + 2);
    REQUIRE(snapshotView(db, "v_count") == pre_count);
    REQUIRE(dbsp_native::g_lazy_view_decodes.load() == decodes_before + 3);
    REQUIRE(db.manager().pending_restore_count() == 0);
}

TEST_CASE("lazy restore: a delta realizes the pending view before applying",
          "[integration][checkpoint][lazy_restore]") {
    DuckDBTestHarness db;
    setupItemsTable(db);

    const std::string sql =
        "SELECT cat, SUM(value) AS s FROM items GROUP BY cat";

    // v_live never goes through the checkpoint fast path (continuously
    // live). v_restore is checkpointed, dropped in-memory, and reloaded
    // pending -- then touched only by a DELTA, never a direct query,
    // proving propagate_changes's realize-before-apply pre-pass (not
    // scan_view's query-path realize) is what makes it correct.
    db.exec("SELECT * FROM dbsp_create_view('v_live', '" + sql + "')");
    db.exec("SELECT * FROM dbsp_create_view('v_restore', '" + sql + "')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_restore')")->HasError());
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_load()")->HasError());
    REQUIRE(db.manager().pending_restore_count() == 1);

    // Delta on the shared source table -- must realize v_restore before
    // propagate_changes applies this delta to it (Global Constraint:
    // deltas never apply to un-restored state).
    db.exec("INSERT INTO items VALUES (5, 'a', 5)");
    db.exec("SELECT * FROM dbsp_sync('items')");

    // The delta's own propagation realized v_restore -- not a query.
    REQUIRE(db.manager().pending_restore_count() == 0);
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // A second delta (now on an already-realized view) stays correct too.
    db.exec("DELETE FROM items WHERE id = 1");
    db.exec("SELECT * FROM dbsp_sync('items')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));
}

TEST_CASE("lazy restore: save after partial realization round-trips",
          "[integration][checkpoint][lazy_restore]") {
    DuckDBTestHarness db;
    setupItemsTable(db);

    db.exec("SELECT * FROM dbsp_create_view('v_sum', "
            "'SELECT cat, SUM(value) AS s FROM items GROUP BY cat')");
    db.exec("SELECT * FROM dbsp_create_view('v_filt', "
            "'SELECT * FROM items WHERE value >= 20')");
    auto pre_sum = snapshotView(db, "v_sum");
    auto pre_filt = snapshotView(db, "v_filt");

    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_sum')")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_filt')")->HasError());
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_load()")->HasError());
    REQUIRE(db.manager().pending_restore_count() == 2);

    // Realize only v_sum; v_filt stays pending.
    REQUIRE(snapshotView(db, "v_sum") == pre_sum);
    REQUIRE(db.manager().pending_restore_count() == 1);

    // Save again: v_sum re-encodes from its now-live state, v_filt's
    // still-pending stash is re-saved verbatim (no decode).
    const size_t decodes_before_save = dbsp_native::g_lazy_view_decodes.load();
    auto save_result = db.query("SELECT * FROM dbsp_save()");
    REQUIRE_FALSE(save_result->HasError());
    const std::string save_msg = save_result->GetValue(0, 0).ToString();
    REQUIRE(save_msg.find("circuit checkpoint: 2 views") != std::string::npos);
    // Re-saving must not have decoded v_filt's stash.
    REQUIRE(dbsp_native::g_lazy_view_decodes.load() == decodes_before_save);

    // Drop both in-memory and reload from the just-re-saved checkpoint.
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_sum')")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_filt')")->HasError());
    auto load_result = db.query("SELECT * FROM dbsp_load()");
    REQUIRE_FALSE(load_result->HasError());
    const std::string load_msg = load_result->GetValue(0, 0).ToString();
    REQUIRE(load_msg.find("2 from checkpoint") != std::string::npos);
    REQUIRE(db.manager().pending_restore_count() == 2);

    // Both round-trip correctly -- v_filt's content survived a verbatim
    // (never-decoded) re-save intact.
    REQUIRE(snapshotView(db, "v_sum") == pre_sum);
    REQUIRE(snapshotView(db, "v_filt") == pre_filt);
    REQUIRE(db.manager().pending_restore_count() == 0);
}

TEST_CASE("lazy restore: dbsp_lazy_restore(false) is eager, like before",
          "[integration][checkpoint][lazy_restore]") {
    DuckDBTestHarness db;
    setupItemsTable(db);

    db.exec("SELECT * FROM dbsp_create_view('v_sum', "
            "'SELECT cat, SUM(value) AS s FROM items GROUP BY cat')");
    db.exec("SELECT * FROM dbsp_create_view('v_filt', "
            "'SELECT * FROM items WHERE value >= 20')");
    auto pre_sum = snapshotView(db, "v_sum");
    auto pre_filt = snapshotView(db, "v_filt");

    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_sum')")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_filt')")->HasError());

    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_lazy_restore(false)")->HasError());
    const size_t decodes_before = dbsp_native::g_lazy_view_decodes.load();

    auto load_result = db.query("SELECT * FROM dbsp_load()");
    REQUIRE_FALSE(load_result->HasError());
    const std::string load_msg = load_result->GetValue(0, 0).ToString();
    REQUIRE(load_msg.find("2 from checkpoint") != std::string::npos);
    REQUIRE(load_msg.find("pending lazy restore") == std::string::npos);

    // Eager path: nothing pending, and the lazy decode counter did not
    // move (restore_view_state, not realize_pending_view_locked, ran).
    REQUIRE(db.manager().pending_restore_count() == 0);
    REQUIRE(dbsp_native::g_lazy_view_decodes.load() == decodes_before);
    REQUIRE(snapshotView(db, "v_sum") == pre_sum);
    REQUIRE(snapshotView(db, "v_filt") == pre_filt);

    // Restore default for any state leakage across TEST_CASEs sharing a
    // process (each DuckDBTestHarness is its own DatabaseInstance /
    // CDCManager, but be explicit rather than relying on that).
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_lazy_restore(true)")->HasError());
}

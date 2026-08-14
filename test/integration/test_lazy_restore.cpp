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
#include <cstdlib>
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

// Chained-DAG shape (register_arrangements fix): items -> v1 -> v2 -> v3,
// where v2's LEFT JOIN probes v1 as its shared (arrangement-eligible)
// side, and v3's LEFT JOIN probes v2 the same way. side1/side2 are the
// joins' LOCAL (non-shared, fully-replayed) sides -- edits to them are
// what force a *probe* of the other side's shared arrangement, as opposed
// to edits to `items`, which only exercise the arrangement's *write*
// (apply_to_arrangements) path. `suffix` builds a second, independently-
// named parallel chain over the same source tables, used as a
// continuously-live correctness oracle (mirrors v_live/v_restore in the
// other cases here).
void setupChainTables(DuckDBTestHarness &db) {
    setupItemsTable(db);
    db.exec("CREATE TABLE side1 (cat VARCHAR, tag1 VARCHAR)");
    db.exec("SELECT * FROM dbsp_track('side1')");
    db.exec("INSERT INTO side1 VALUES ('a', 't1'), ('b', 't2')");
    db.exec("SELECT * FROM dbsp_sync('side1')");

    db.exec("CREATE TABLE side2 (tag1 VARCHAR, tag2 VARCHAR)");
    db.exec("SELECT * FROM dbsp_track('side2')");
    db.exec("INSERT INTO side2 VALUES ('t1', 'x'), ('t2', 'y')");
    db.exec("SELECT * FROM dbsp_sync('side2')");
}

void createChain(DuckDBTestHarness &db, const std::string &suffix) {
    const std::string v1 = "v1" + suffix;
    const std::string v2 = "v2" + suffix;
    const std::string v3 = "v3" + suffix;
    db.exec("SELECT * FROM dbsp_create_view('" + v1 + "', "
            "'SELECT cat, SUM(value) AS total FROM items GROUP BY cat')");
    // v1 is the RIGHT (probe-target) side of a LEFT JOIN -- arrangement-
    // eligible (not self-padding, referenced once) -- a shared arrangement
    // gets registered over it.
    db.exec("SELECT * FROM dbsp_create_view('" + v2 + "', "
            "'SELECT s.cat AS cat, s.tag1 AS tag1, j.total AS total FROM "
            "side1 s LEFT JOIN " + v1 + " j ON s.cat = j.cat')");
    // Same shape one level down: v2 is the shared side of v3's LEFT JOIN.
    db.exec("SELECT * FROM dbsp_create_view('" + v3 + "', "
            "'SELECT w.tag1 AS tag1, w.tag2 AS tag2, j.total AS total FROM "
            "side2 w LEFT JOIN " + v2 + " j ON w.tag1 = j.tag1')");
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

// Regression test for the register_arrangements defect: on a fully
// chained view-on-view DAG (every view feeds the next, exactly the wfp
// shape), the ORIGINAL D-lazy code called realize_pending_view_locked
// unconditionally for any view-sourced shared-arrangement request during
// register_arrangements -- reached from every view's cold-create call
// inside load_from_duck_table's checkpoint loop, regardless of `cold`.
// Because v2 registers an arrangement over v1, creating v2 (cold)
// transitively realized v1 right there, at load time, before dbsp_load()
// even returned; likewise v3's registration realized v2. The fix defers
// the fill (SharedArrangement::needs_backfill, mirroring D3c's table-
// baseline deferral) instead of realizing eagerly, and generalizes
// backfill_deferred_arrangements_locked (previously table-only) so
// realize_pending_view_locked backfills any arrangement sourced from the
// view it just decoded.
TEST_CASE("lazy restore: chained view-on-view DAG defers the whole chain",
          "[integration][checkpoint][lazy_restore]") {
    DuckDBTestHarness db;
    setupChainTables(db);

    // "" is checkpointed and reloaded pending; "_live" is a second,
    // independently-named parallel chain over the same source tables that
    // never goes through the checkpoint fast path -- the correctness
    // oracle for every comparison below.
    createChain(db, "");
    createChain(db, "_live");
    REQUIRE(db.manager().get_view("v1")->checkpointable());
    REQUIRE(db.manager().get_view("v2")->checkpointable());
    REQUIRE(db.manager().get_view("v3")->checkpointable());

    REQUIRE(snapshotView(db, "v1") == snapshotView(db, "v1_live"));
    REQUIRE(snapshotView(db, "v2") == snapshotView(db, "v2_live"));
    REQUIRE(snapshotView(db, "v3") == snapshotView(db, "v3_live"));

    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
    // Dependency order: v3 depends on v2 depends on v1 -- drop_view
    // refuses to drop a view other views still depend on.
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v3')")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v2')")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v1')")->HasError());

    auto load_result = db.query("SELECT * FROM dbsp_load()");
    REQUIRE_FALSE(load_result->HasError());
    const std::string load_msg = load_result->GetValue(0, 0).ToString();
    REQUIRE(load_msg.find("3 from checkpoint") != std::string::npos);
    REQUIRE(load_msg.find("3 pending lazy restore") != std::string::npos);

    // This is the assertion the original defect would have failed: it
    // eagerly realized v1 (via v2's cold-create registration) and v2 (via
    // v3's) purely from the load loop, before ANY query -- leaving this
    // count well below 3.
    REQUIRE(db.manager().pending_restore_count() == 3);
    const size_t decodes_before = dbsp_native::g_lazy_view_decodes.load();

    // Exercise only v3's ancestor chain: a delta on `items`, v1's sole
    // source. Its transitive dependents (dep graph) are exactly
    // {v1, v2, v3} (and the live twins) -- not side1/side2, which don't
    // depend on items. propagate_changes's realize pre-pass must decode
    // all three before applying the delta.
    db.exec("INSERT INTO items VALUES (5, 'a', 5)");
    db.exec("SELECT * FROM dbsp_sync('items')");

    REQUIRE(dbsp_native::g_lazy_view_decodes.load() == decodes_before + 3);
    REQUIRE(db.manager().pending_restore_count() == 0);

    // Delta-before-query correctness: none of v1/v2/v3 was ever queried
    // before this delta landed, only realized+stepped via the pre-pass.
    REQUIRE(snapshotView(db, "v1") == snapshotView(db, "v1_live"));
    REQUIRE(snapshotView(db, "v2") == snapshotView(db, "v2_live"));
    REQUIRE(snapshotView(db, "v3") == snapshotView(db, "v3_live"));

    // Probe the v1-sourced arrangement from the OTHER (non-shared) side:
    // a new side1 row for an existing cat must find v1's real total via
    // the arrangement register_arrangements deferred and
    // realize_pending_view_locked backfilled above -- an empty/unfilled
    // arrangement here would silently pad NULL instead (LEFT JOIN, no
    // match found).
    db.exec("INSERT INTO side1 VALUES ('a', 't3')");
    db.exec("SELECT * FROM dbsp_sync('side1')");
    REQUIRE(snapshotView(db, "v2") == snapshotView(db, "v2_live"));

    // Same probe one level down: a new side2 row for the tag1 v2 just
    // gained must find v2's real total via v3's (equally deferred and
    // backfilled) shared arrangement over v2.
    db.exec("INSERT INTO side2 VALUES ('t3', 'z')");
    db.exec("SELECT * FROM dbsp_sync('side2')");
    REQUIRE(snapshotView(db, "v3") == snapshotView(db, "v3_live"));
}

// MANDATORY regression test (reviewer-reproduced Critical, fixed same
// day): the case above always lands its first delta on `items` -- v1's
// OWN source -- so v1 is already in that delta's topo (dep_graph_.
// topological_order("items") = {v1, v2, v3}) and gets realized as a
// direct descendant, never exercising the actual gap. The real defect
// only shows when the FIRST post-reopen delta lands on the join's LOCAL
// side (side1) instead: dep_graph_.topological_order("side1") = {v2, v3}
// only -- v1 is a SIBLING dependency of v2 (v2 depends on both side1 and
// v1; side1 does not depend on v1 or vice versa), so walking dependents_
// forward from side1 never reaches it, however pending it still is. The
// original (unfixed) pre-pass realized exactly topo and nothing else,
// leaving v1's shared arrangement `needs_backfill`; v2's LEFT JOIN,
// applying side1's delta, probes that arrangement for cat='a' (probe_side
// has no needs_backfill guard) and finds nothing -- a silent NULL pad
// instead of v1's real SUM(value) = 30 for cat='a'. The reviewer's temp
// repro observed exactly v2 = ('a', 't3', NULL) against a live twin's
// ('a', 't3', 30); this test encodes that exact shape permanently.
TEST_CASE("lazy restore: first post-reopen delta on a join's local side "
          "still realizes its pending shared-arrangement sibling",
          "[integration][checkpoint][lazy_restore]") {
    DuckDBTestHarness db;
    setupChainTables(db);
    createChain(db, "");
    createChain(db, "_live");
    REQUIRE(db.manager().get_view("v1")->checkpointable());
    REQUIRE(db.manager().get_view("v2")->checkpointable());
    REQUIRE(db.manager().get_view("v3")->checkpointable());

    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v3')")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v2')")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v1')")->HasError());
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_load()")->HasError());
    REQUIRE(db.manager().pending_restore_count() == 3);

    // The FIRST delta after reopen -- on side1, not items. v1 is still
    // fully pending, untouched by anything, when this lands.
    db.exec("INSERT INTO side1 VALUES ('a', 't3')");
    db.exec("SELECT * FROM dbsp_sync('side1')");

    // v2's new row must carry v1's real total for cat='a' (30 = 10 + 20),
    // never a NULL pad from an unfilled arrangement -- the exact
    // reviewer-reproduced shape (their repro: v2 got 'a'|'t3'|NULL here).
    REQUIRE(snapshotView(db, "v2") == snapshotView(db, "v2_live"));

    // v1 was realized transitively (it feeds v2, which IS in topo) even
    // though it was never itself queried, edited, or in topo directly --
    // and v3 (also in topo) too. Nothing pending is left half-swept.
    REQUIRE(db.manager().pending_restore_count() == 0);
    REQUIRE(snapshotView(db, "v1") == snapshotView(db, "v1_live"));
    REQUIRE(snapshotView(db, "v3") == snapshotView(db, "v3_live"));
}

// ---------------------------------------------------------------------------
// Phase 3 (disk-backed / lazy-from-table) failure modes. These tests pin
// down the previously "unreproduced flaky" report
//   "lazy-restore stash for view '...' failed to decode; scheduling full
//    rebuild"
// (realize_pending_view_locked, dbsp_cdc.hpp) by building the exact app
// shape deterministically: mv tables ON, a checkpointed view reloaded
// PENDING with placeholder blobs (bytes left in _dbsp_ckpt, fetched at
// realize time -- CkptData::lazy_blobs).
// ---------------------------------------------------------------------------

namespace {

// Build the Phase 3 lazy shape: v_sum checkpointed while mv tables are ON,
// dropped in-memory (its __mv_ table + meta row survive), reloaded via the
// checkpoint fast path -> pending with EMPTY placeholder blobs. v_live is
// the continuously-live correctness oracle (identical SQL, never dropped).
void setupMvPendingPair(DuckDBTestHarness &db) {
    setupItemsTable(db);
    const std::string sql =
        "SELECT cat, SUM(value) AS s FROM items GROUP BY cat";
    db.exec("SELECT * FROM dbsp_create_view('v_sum', '" + sql + "')");
    db.exec("SELECT * FROM dbsp_create_view('v_live', '" + sql + "')");
    db.exec("SELECT * FROM dbsp_mv_tables(true)");
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_sum')")->HasError());
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_load()")->HasError());
    REQUIRE(db.manager().pending_restore_count() == 1);
}

// Smallest persisted blob length for v_sum rows of `kind` in _dbsp_ckpt.
// A Phase 3 placeholder clobber leaves 0-byte rows; real node blobs are
// always non-empty, and even a table-backed view's deliberately-empty sink
// result blob is 8 bytes (u64 row count = 0), never 0.
int64_t minCkptBlobLen(DuckDBTestHarness &db, const std::string &kind) {
    auto r = db.query("SELECT COALESCE(MIN(octet_length(data)), -1) FROM "
                      "_dbsp_ckpt WHERE name = 'v_sum' AND kind = '" +
                      kind + "'");
    REQUIRE_FALSE(r->HasError());
    return r->GetValue(0, 0).GetValue<int64_t>();
}

// Drive the rebuild_pending_ escape hatch to completion (it runs at the
// next statement boundary -- QueryBegin) and prove it self-heals: the
// rebuilt view matches the continuously-live twin exactly.
void requireEscapeHatchHeals(DuckDBTestHarness &db,
                             size_t failures_before) {
    // The failure site fired (realize decode failure -> escape hatch).
    // Asserted via the g_lazy_realize_failures counter: last_error_ is
    // unreachable from realize_pending_view_locked (its callers hold
    // struct_mutex_ shared, so record_error_best_effort's try-lock always
    // fails and the message goes to stderr only).
    REQUIRE(dbsp_native::g_lazy_realize_failures.load() ==
            failures_before + 1);
    REQUIRE(db.manager().rebuild_pending());
    db.exec("SELECT 1"); // statement boundary: rebuild_all_views runs here
    REQUIRE_FALSE(db.manager().rebuild_pending());
    REQUIRE(snapshotView(db, "v_sum") == snapshotView(db, "v_live"));
}

} // namespace

// THE ROOT CAUSE of the flaky report (repro without any fault injection):
// a Phase 3 lazy stash holds EMPTY placeholder blobs -- the real bytes
// stay in _dbsp_ckpt and are fetched per view at realize time. But
// save_checkpoint's "verbatim re-save" of a still-pending view wrote the
// STASH back, i.e. the placeholders: 0-byte node/sink rows replaced the
// only copy of the real bytes. The next realize (same session, or any
// later one -- watermarks still match) fetched the 0-byte rows, failed to
// decode, and fell back to the full rebuild. In the app: attach -> some
// views never touched -> periodic dbsp_save -> first touch of such a view
// logs "lazy-restore stash ... failed to decode". Flaky only because it
// needs a view that is still pending at save time and touched afterwards.
TEST_CASE("lazy restore: dbsp_save while a lazy-from-table view is still "
          "pending preserves its checkpoint bytes",
          "[integration][checkpoint][lazy_restore]") {
    DuckDBTestHarness db;
    setupMvPendingPair(db);

    // The trigger: save while v_sum is still pending (stash = placeholders).
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());

    // The persisted bytes must survive the save. Pre-fix this read 0
    // (placeholder rows clobbered the real blobs).
    REQUIRE(minCkptBlobLen(db, "node") > 0);
    REQUIRE(minCkptBlobLen(db, "sink") > 0);

    // First touch after the save: a delta on v_sum's source realizes it
    // (propagate_changes pre-pass). Must decode cleanly -- no escape hatch.
    db.exec("INSERT INTO items VALUES (5, 'a', 5)");
    db.exec("SELECT * FROM dbsp_sync('items')");
    REQUIRE_FALSE(db.manager().rebuild_pending());
    REQUIRE(db.manager().pending_restore_count() == 0);
    REQUIRE(snapshotView(db, "v_sum") == snapshotView(db, "v_live"));

    // Next-session durability: the re-saved checkpoint round-trips too.
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_sum')")->HasError());
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_load()")->HasError());
    REQUIRE(db.manager().pending_restore_count() == 1);
    db.exec("INSERT INTO items VALUES (6, 'b', 6)");
    db.exec("SELECT * FROM dbsp_sync('items')");
    REQUIRE_FALSE(db.manager().rebuild_pending());
    REQUIRE(snapshotView(db, "v_sum") == snapshotView(db, "v_live"));
}

// F9 failure mode 1: checkpointable() flips false between save and realize
// (restore_circuit_state's first guard). Cannot occur organically from SQL
// alone -- the fingerprint gate re-plans the same SQL -- so it is forced
// via the DBSP_TEST_CKPT_FLIP fault-injection env var (test-only, checked
// at realize, never at save). Proves the diagnostic path fails cleanly and
// the rebuild escape hatch restores exact parity.
TEST_CASE("lazy restore: injected checkpointable() flip at realize fails "
          "cleanly and the rebuild escape hatch heals",
          "[integration][checkpoint][lazy_restore]") {
    DuckDBTestHarness db;
    setupMvPendingPair(db);

    const size_t fails_before = dbsp_native::g_lazy_realize_failures.load();
    setenv("DBSP_TEST_CKPT_FLIP", "1", 1);
    auto q = db.query("SELECT * FROM dbsp_query('v_sum')"); // touch: realize
    unsetenv("DBSP_TEST_CKPT_FLIP");
    REQUIRE_FALSE(q->HasError());

    requireEscapeHatchHeals(db, fails_before);
}

// F9 failure mode 2: a node blob missing at realize (the "plan shape
// drifted since save?" diagnostic). Injected by deleting v_sum's node rows
// from _dbsp_ckpt after the lazy load stashed placeholders but before
// anything fetched the bytes -- the realize-time fetch then comes back
// sink-only, and the aggregate node finds no blob for its id.
TEST_CASE("lazy restore: missing node blob at realize fails cleanly and "
          "the rebuild escape hatch heals",
          "[integration][checkpoint][lazy_restore]") {
    DuckDBTestHarness db;
    setupMvPendingPair(db);

    const size_t fails_before = dbsp_native::g_lazy_realize_failures.load();
    db.exec("DELETE FROM _dbsp_ckpt WHERE name = 'v_sum' AND kind = 'node'");
    auto q = db.query("SELECT * FROM dbsp_query('v_sum')");
    REQUIRE_FALSE(q->HasError());

    requireEscapeHatchHeals(db, fails_before);
}

// F9 failure mode 3: a node blob present but rejected by restore_state
// (truncated/corrupt bytes). Injected by overwriting v_sum's node rows
// with garbage; BlobReader's bounds checks make the node reject the blob.
TEST_CASE("lazy restore: corrupt node blob at realize fails cleanly and "
          "the rebuild escape hatch heals",
          "[integration][checkpoint][lazy_restore]") {
    DuckDBTestHarness db;
    setupMvPendingPair(db);

    const size_t fails_before = dbsp_native::g_lazy_realize_failures.load();
    db.exec("UPDATE _dbsp_ckpt SET data = 'xx'::BLOB "
            "WHERE name = 'v_sum' AND kind = 'node'");
    auto q = db.query("SELECT * FROM dbsp_query('v_sum')");
    REQUIRE_FALSE(q->HasError());

    requireEscapeHatchHeals(db, fails_before);
}

// Sink-blob decode failure (the catch right after restore_circuit_state in
// realize_pending_view_locked): nodes restore fine, the sink blob throws.
TEST_CASE("lazy restore: corrupt sink blob at realize fails cleanly and "
          "the rebuild escape hatch heals",
          "[integration][checkpoint][lazy_restore]") {
    DuckDBTestHarness db;
    setupMvPendingPair(db);

    const size_t fails_before = dbsp_native::g_lazy_realize_failures.load();
    db.exec("UPDATE _dbsp_ckpt SET data = 'x'::BLOB "
            "WHERE name = 'v_sum' AND kind = 'sink'");
    auto q = db.query("SELECT * FROM dbsp_query('v_sum')");
    REQUIRE_FALSE(q->HasError());

    requireEscapeHatchHeals(db, fails_before);
}

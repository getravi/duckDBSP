// Task 3 (durability-ergonomics): LEFT/RIGHT outer-join checkpoint state.
//
// D3b circuit-state checkpointing supported only INNER equi-joins;
// LEFT/RIGHT (and FULL/MARK) joins carry pad/mark bookkeeping the node
// never serialized, so they fell back to rebuild-by-replay on load. This
// file proves:
//   1. state_kind() gate: LEFT/RIGHT (non-spilled) report checkpointable;
//      FULL and a spilled LEFT join still do not.
//   2. round-trip correctness: a checkpointed-and-restored LEFT-join view
//      behaves identically to a continuously-live twin under post-restore
//      deltas, exercising both pad transitions (gains first match / loses
//      last match). Same proof for RIGHT (roles of left_t/right_t
//      reversed).
//   3. format-version gate: a pre-bump checkpoint (missing the version
//      table) is treated as absent and falls back to rebuild, never
//      misparsed.
//   4. (fix-wave, Finding 1) SQL-fingerprint gate: a checkpoint whose
//      view definition has since diverged from `_dbsp_views` (as happens
//      if `dbsp_replace_view`/`CREATE OR REPLACE` swaps the SQL but an
//      unclean close skips the next `save_checkpoint()`) is declined for
//      that view and rebuilt by replay from the *new* SQL, never served
//      stale from the old blob.

#include "catch.hpp"
#include "../test_helpers.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace dbsp_test;

namespace {

// Snapshot a live view's exact (row, weight) content via scan_view — a
// stronger check than dbsp_query's weight-expanded rows, since it also
// catches a wrong weight on an otherwise-correct row.
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

void setupLeftRightTables(DuckDBTestHarness &db) {
    db.exec("CREATE TABLE left_t (k INTEGER, a INTEGER)");
    db.exec("CREATE TABLE right_t (k INTEGER, b INTEGER)");
    db.exec("SELECT * FROM dbsp_track('left_t')");
    db.exec("SELECT * FROM dbsp_track('right_t')");
    db.exec("INSERT INTO left_t VALUES (1, 10), (2, 20)");
    db.exec("INSERT INTO right_t VALUES (1, 100)"); // k=2 unmatched -> padded
    db.exec("SELECT * FROM dbsp_sync('left_t')");
    db.exec("SELECT * FROM dbsp_sync('right_t')");
    db.exec("SELECT * FROM dbsp_use_planner(true)");
}

} // namespace

TEST_CASE("join checkpoint: state_kind gate for LEFT/RIGHT/FULL/spilled",
          "[integration][checkpoint][join]") {
    DuckDBTestHarness db;
    setupLeftRightTables(db);

    const std::string left_sql =
        "SELECT l.k, l.a, r.b FROM left_t l LEFT JOIN right_t r ON l.k = r.k";
    const std::string right_sql =
        "SELECT l.k, l.a, r.b FROM left_t l RIGHT JOIN right_t r ON l.k = r.k";
    const std::string full_sql =
        "SELECT l.k, l.a, r.b FROM left_t l FULL JOIN right_t r ON l.k = r.k";

    db.exec("SELECT * FROM dbsp_create_view('v_left', '" + left_sql + "')");
    REQUIRE(db.manager().get_view("v_left")->checkpointable());

    db.exec("SELECT * FROM dbsp_create_view('v_right', '" + right_sql + "')");
    REQUIRE(db.manager().get_view("v_right")->checkpointable());

    db.exec("SELECT * FROM dbsp_create_view('v_full', '" + full_sql + "')");
    REQUIRE_FALSE(db.manager().get_view("v_full")->checkpointable());

    // A LEFT join created under spill mode has its (pure probe-target)
    // right side spilled to disk (local_spill_right_) — still UNSUPPORTED.
    db.exec("SELECT * FROM dbsp_spill(true)");
    db.exec("SELECT * FROM dbsp_create_view('v_left_spilled', '" + left_sql +
            "')");
    REQUIRE_FALSE(db.manager().get_view("v_left_spilled")->checkpointable());
    db.exec("SELECT * FROM dbsp_spill(false)"); // process-global: reset
}

TEST_CASE("join checkpoint: LEFT join view round-trips pad state",
          "[integration][checkpoint][join]") {
    DuckDBTestHarness db;
    setupLeftRightTables(db);

    const std::string sql =
        "SELECT l.k, l.a, r.b FROM left_t l LEFT JOIN right_t r ON l.k = r.k";

    // Twin A (v_live) stays live for the whole test. Twin B (v_restore) is
    // checkpointed, dropped (in-memory only — the persisted _dbsp_views /
    // _dbsp_ckpt rows survive), and reloaded cold from the checkpoint.
    db.exec("SELECT * FROM dbsp_create_view('v_live', '" + sql + "')");
    db.exec("SELECT * FROM dbsp_create_view('v_restore', '" + sql + "')");
    REQUIRE(db.manager().get_view("v_restore")->checkpointable());
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    auto save_result = db.query("SELECT * FROM dbsp_save()");
    REQUIRE_FALSE(save_result->HasError());
    const std::string save_msg = save_result->GetValue(0, 0).ToString();
    REQUIRE(save_msg.find("circuit checkpoint: 2 views") != std::string::npos);

    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_restore')")->HasError());

    auto load_result = db.query("SELECT * FROM dbsp_load()");
    REQUIRE_FALSE(load_result->HasError());
    const std::string load_msg = load_result->GetValue(0, 0).ToString();
    // Only v_restore needed reloading (v_live was still live, skipped).
    REQUIRE(load_msg.find("1 from checkpoint") != std::string::npos);
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 1: row k=2 gains its first match -> pad retracts.
    db.exec("INSERT INTO right_t VALUES (2, 200)");
    db.exec("SELECT * FROM dbsp_sync('right_t')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 2: row k=1 loses its last match -> pad appears.
    db.exec("DELETE FROM right_t WHERE k = 1");
    db.exec("SELECT * FROM dbsp_sync('right_t')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));
}

namespace {

// Mirrors setupLeftRightTables, but shaped so a RIGHT JOIN (which drives
// from the right side and pads the left) starts with an unmatched right
// row: left_t has only k=1; right_t has k=1 (matched) and k=2 (unmatched,
// -> left columns padded).
void setupRightJoinPadTables(DuckDBTestHarness &db) {
    db.exec("CREATE TABLE left_t (k INTEGER, a INTEGER)");
    db.exec("CREATE TABLE right_t (k INTEGER, b INTEGER)");
    db.exec("SELECT * FROM dbsp_track('left_t')");
    db.exec("SELECT * FROM dbsp_track('right_t')");
    db.exec("INSERT INTO left_t VALUES (1, 10)");
    db.exec("INSERT INTO right_t VALUES (1, 100), (2, 200)");
    db.exec("SELECT * FROM dbsp_sync('left_t')");
    db.exec("SELECT * FROM dbsp_sync('right_t')");
    db.exec("SELECT * FROM dbsp_use_planner(true)");
}

} // namespace

TEST_CASE("join checkpoint: RIGHT join view round-trips pad state",
          "[integration][checkpoint][join]") {
    DuckDBTestHarness db;
    setupRightJoinPadTables(db);

    const std::string sql =
        "SELECT l.k, l.a, r.b FROM left_t l RIGHT JOIN right_t r ON l.k = r.k";

    // Twin A (v_live) stays live for the whole test. Twin B (v_restore) is
    // checkpointed, dropped (in-memory only), and reloaded cold from the
    // checkpoint.
    db.exec("SELECT * FROM dbsp_create_view('v_live', '" + sql + "')");
    db.exec("SELECT * FROM dbsp_create_view('v_restore', '" + sql + "')");
    REQUIRE(db.manager().get_view("v_restore")->checkpointable());
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    auto save_result = db.query("SELECT * FROM dbsp_save()");
    REQUIRE_FALSE(save_result->HasError());
    const std::string save_msg = save_result->GetValue(0, 0).ToString();
    REQUIRE(save_msg.find("circuit checkpoint: 2 views") != std::string::npos);

    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_restore')")->HasError());

    auto load_result = db.query("SELECT * FROM dbsp_load()");
    REQUIRE_FALSE(load_result->HasError());
    const std::string load_msg = load_result->GetValue(0, 0).ToString();
    // Only v_restore needed reloading (v_live was still live, skipped).
    REQUIRE(load_msg.find("1 from checkpoint") != std::string::npos);
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 1: right row k=2 gains its first match (left_t
    // gets k=2) -> pad retracts.
    db.exec("INSERT INTO left_t VALUES (2, 20)");
    db.exec("SELECT * FROM dbsp_sync('left_t')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 2: left row k=1 removed -> right's k=1 loses its
    // last match -> pad appears.
    db.exec("DELETE FROM left_t WHERE k = 1");
    db.exec("SELECT * FROM dbsp_sync('left_t')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));
}

TEST_CASE("join checkpoint: version-mismatched blob falls back to rebuild",
          "[integration][checkpoint][join]") {
    DuckDBTestHarness db;
    setupLeftRightTables(db);

    const std::string sql =
        "SELECT l.k, l.a, r.b FROM left_t l LEFT JOIN right_t r ON l.k = r.k";
    db.exec("SELECT * FROM dbsp_create_view('v_stale', '" + sql + "')");
    auto before = snapshotView(db, "v_stale");

    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
    // Simulate a pre-format-bump checkpoint: the version table is exactly
    // what old blobs lack. Dropping it must make the checkpoint look
    // ABSENT, not cause a misparse of the (now differently-shaped) node
    // blobs.
    db.exec("DROP TABLE _dbsp_ckpt_version");

    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_stale')")->HasError());
    auto load_result = db.query("SELECT * FROM dbsp_load()");
    REQUIRE_FALSE(load_result->HasError());
    const std::string load_msg = load_result->GetValue(0, 0).ToString();
    REQUIRE(load_msg.find("0 from checkpoint") != std::string::npos);

    // Rebuilt-by-replay view must still be correct.
    REQUIRE(snapshotView(db, "v_stale") == before);
}

TEST_CASE("join checkpoint: stale checkpoint declined when view SQL changed "
          "underneath it",
          "[integration][checkpoint][join]") {
    // Finding 1 (durability-ergonomics fix-wave): a checkpoint is only
    // trustworthy for a view whose SQL hasn't changed since the blob was
    // written. Simulate the scenario a live process can't easily produce
    // deterministically (replace_view + a kill before the next
    // save_checkpoint): checkpoint a view, then update its _dbsp_views row
    // directly to a DIFFERENT definition without refreshing the
    // checkpoint, then reload. The view must be rebuilt from the NEW sql,
    // never served from the stale blob (which holds the OLD sql's
    // results).
    DuckDBTestHarness db;
    setupLeftRightTables(db);

    const std::string sql_v1 = "SELECT k, a * 2 AS val FROM left_t";
    db.exec("SELECT * FROM dbsp_create_view('v_fp', '" + sql_v1 + "')");
    REQUIRE(db.manager().get_view("v_fp")->checkpointable());

    // a=10 -> val=20, a=20 -> val=40 (the OLD sql's results).
    auto before = snapshotView(db, "v_fp");

    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());

    // Diverge the persisted definition without a fresh checkpoint save —
    // exactly what a crash/failed-autosave right after a replace_view
    // leaves behind (new _dbsp_views row, stale _dbsp_ckpt blobs).
    db.exec("UPDATE _dbsp_views SET sql = 'SELECT k, a * 3 AS val FROM "
            "left_t' WHERE name = 'v_fp'");

    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_fp')")->HasError());

    auto load_result = db.query("SELECT * FROM dbsp_load()");
    REQUIRE_FALSE(load_result->HasError());
    const std::string load_msg = load_result->GetValue(0, 0).ToString();
    // The fingerprint mismatch must decline the fast path for v_fp: it
    // does not count as "from checkpoint" even though a checkpoint blob
    // for it exists.
    REQUIRE(load_msg.find("0 from checkpoint") != std::string::npos);

    auto after = snapshotView(db, "v_fp");
    REQUIRE(after != before);
    // a=10 -> val=30, a=20 -> val=60 (the NEW sql's results) — proves the
    // view was rebuilt by replay against the new definition, not restored
    // from the old checkpoint blob.
    std::vector<std::pair<std::vector<std::string>, int64_t>> expected = {
        {{"1", "30"}, 1}, {{"2", "60"}, 1}};
    std::sort(expected.begin(), expected.end());
    REQUIRE(after == expected);
}

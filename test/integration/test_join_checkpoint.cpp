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
#include <cstdlib>
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

// Task 2 (cold/restore attack): MIN/MAX (value-collecting) aggregate
// checkpoint state.
//
// PlanAggregateNode previously reported UNSUPPORTED for anything beyond the
// scalar count/sum/avg family, forcing every MIN/MAX view to rebuild by
// replay on load. This extends SERIALIZABLE to plain (non-DISTINCT) MIN/MAX
// by round-tripping the per-group `values` multiset the maintenance path
// already keeps for retractions. DISTINCT and holistic aggregates
// (MEDIAN/QUANTILE/MODE/etc.) and any group whose values have spilled to
// disk (N4) stay UNSUPPORTED.
namespace {

// Three groups shaped for the round-trip tests below: g=1 and g=3 each
// carry three distinct values (multiset depth >1, so deleting the current
// extreme must retreat to the next one, not just go blank); g=2 carries a
// single value (so a fresh insert cleanly demonstrates the extreme
// advancing past it).
void setupAggTables(DuckDBTestHarness &db) {
    db.exec("CREATE TABLE nums (g INTEGER, v INTEGER)");
    db.exec("SELECT * FROM dbsp_track('nums')");
    db.exec("INSERT INTO nums VALUES (1, 5), (1, 15), (1, 25), "
            "(2, 100), (3, 10), (3, 20), (3, 30)");
    db.exec("SELECT * FROM dbsp_sync('nums')");
    db.exec("SELECT * FROM dbsp_use_planner(true)");
}

} // namespace

TEST_CASE("aggregate checkpoint: state_kind gate for MIN/MAX vs "
          "DISTINCT/MEDIAN/spilled",
          "[integration][checkpoint][aggregate]") {
    DuckDBTestHarness db;
    setupAggTables(db);

    db.exec("SELECT * FROM dbsp_create_view('v_max', "
            "'SELECT g, MAX(v) AS m FROM nums GROUP BY g')");
    REQUIRE(db.manager().get_view("v_max")->checkpointable());

    db.exec("SELECT * FROM dbsp_create_view('v_min', "
            "'SELECT g, MIN(v) AS m FROM nums GROUP BY g')");
    REQUIRE(db.manager().get_view("v_min")->checkpointable());

    db.exec("SELECT * FROM dbsp_create_view('v_max_distinct', "
            "'SELECT g, MAX(DISTINCT v) AS m FROM nums GROUP BY g')");
    REQUIRE_FALSE(db.manager().get_view("v_max_distinct")->checkpointable());

    db.exec("SELECT * FROM dbsp_create_view('v_median', "
            "'SELECT g, MEDIAN(v) AS m FROM nums GROUP BY g')");
    REQUIRE_FALSE(db.manager().get_view("v_median")->checkpointable());

    // A group whose values multiset has spilled to a disk bucket log (N4:
    // >65536 values under dbsp_spill(true)) cannot be reconstructed from a
    // checkpoint blob — mirrors the join node's exclusion of spilled
    // equi-key indexes (state_kind gate test above).
    db.exec("CREATE TABLE many (g INTEGER, v INTEGER)");
    db.exec("SELECT * FROM dbsp_track('many')");
    db.exec("SELECT * FROM dbsp_spill(true)");
    db.exec("SELECT * FROM dbsp_create_view('v_max_spilled', "
            "'SELECT g, MAX(v) AS m FROM many GROUP BY g')");
    db.exec("INSERT INTO many SELECT 1, i FROM range(70000) AS t(i)");
    db.exec("SELECT * FROM dbsp_sync('many')");
    REQUIRE_FALSE(db.manager().get_view("v_max_spilled")->checkpointable());
    db.exec("SELECT * FROM dbsp_spill(false)"); // process-global: reset
}

TEST_CASE("aggregate checkpoint: MAX round-trips through save/restore, "
          "advancing and retreating (twice, same group) on post-restore "
          "deltas",
          "[integration][checkpoint][aggregate]") {
    DuckDBTestHarness db;
    setupAggTables(db);

    const std::string sql = "SELECT g, MAX(v) AS m FROM nums GROUP BY g";

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

    // Post-restore delta 1: group 2 (single value 100) gets a higher value
    // -> max advances 100 -> 500.
    db.exec("INSERT INTO nums VALUES (2, 500)");
    db.exec("SELECT * FROM dbsp_sync('nums')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 2/3: group 3 ({10, 20, 30}) has its current max
    // deleted TWICE in a row -> max retreats 30 -> 20 -> 10. This only
    // works if the restored state kept the whole multiset, not just the
    // cached top value.
    db.exec("DELETE FROM nums WHERE g = 3 AND v = 30");
    db.exec("SELECT * FROM dbsp_sync('nums')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    db.exec("DELETE FROM nums WHERE g = 3 AND v = 20");
    db.exec("SELECT * FROM dbsp_sync('nums')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 4: group 1 ({5, 15, 25}) has its max deleted once
    // -> max retreats 25 -> 15.
    db.exec("DELETE FROM nums WHERE g = 1 AND v = 25");
    db.exec("SELECT * FROM dbsp_sync('nums')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));
}

TEST_CASE("aggregate checkpoint: MIN round-trips through save/restore, "
          "falling and rising (twice, same group) on post-restore deltas",
          "[integration][checkpoint][aggregate]") {
    DuckDBTestHarness db;
    setupAggTables(db);

    const std::string sql = "SELECT g, MIN(v) AS m FROM nums GROUP BY g";

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
    REQUIRE(load_msg.find("1 from checkpoint") != std::string::npos);
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 1: group 2 (single value 100) gets a lower value
    // -> min falls 100 -> 1.
    db.exec("INSERT INTO nums VALUES (2, 1)");
    db.exec("SELECT * FROM dbsp_sync('nums')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 2/3: group 3 ({10, 20, 30}) has its current min
    // deleted TWICE in a row -> min rises 10 -> 20 -> 30. Proves the
    // restored multiset kept the full depth, not just the cached bottom
    // value.
    db.exec("DELETE FROM nums WHERE g = 3 AND v = 10");
    db.exec("SELECT * FROM dbsp_sync('nums')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    db.exec("DELETE FROM nums WHERE g = 3 AND v = 20");
    db.exec("SELECT * FROM dbsp_sync('nums')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 4: group 1 ({5, 15, 25}) has its min deleted once
    // -> min rises 5 -> 15.
    db.exec("DELETE FROM nums WHERE g = 1 AND v = 5");
    db.exec("SELECT * FROM dbsp_sync('nums')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));
}

// Task 3 (cold/restore attack): fast typed codec for checkpoint blobs.
//
// The byte-level row codec (serialize_row/deserialize_row) was already
// shared with the spill path from D3b's first commit onward -- checkpoint
// blob decode was never a separate, slower implementation. The real cost
// (observed ~15us/row on a 58k-row sink, matching the design doc's
// "851ms/58k rows") was every restored row paying Value::Hash()'s lazy
// per-Value cost (a fresh 1-element Vector + VectorOperations::Hash per
// column) the first time it entered a hash-keyed structure (aggregate
// states_, join Index/RowWeights, DuckDBZSet) -- exactly the "~2/3 of
// ingestion time" dbsp_engine_hook.hpp already documents for the same lazy
// path elsewhere. BlobReader::hashed_row() pre-seeds the hash cache using
// hash_row_fast, which calls the same duckdb::Hash<T> primitives
// VectorOperations::Hash dispatches to per element, without the temporary
// Vector. These tests are the correctness guard for that invariant: a
// wrong hash silently corrupts hash-map lookups (a post-restore delta
// fails to find its matching group/bucket) rather than crashing.

TEST_CASE("checkpoint blob codec: hashed_row hash matches the lazy "
          "per-Value path across every codec value shape",
          "[unit][checkpoint][codec]") {
    using duckdb::Value;
    // Fast-path types (+ typed NULLs) and one fallback type (+ typed NULL)
    // -- the same shape list test_spill_store.cpp's codec test uses, since
    // hashed_row must stay correct for both branches of hash_row_fast.
    std::vector<Value> row = {
        Value::INTEGER(42),
        Value::BIGINT(-7),
        Value::DOUBLE(3.25),
        Value("plain string"),
        Value(""),
        Value::BOOLEAN(true),
        Value(duckdb::LogicalType::INTEGER), // typed NULL, fast path
        Value(duckdb::LogicalType::VARCHAR), // typed NULL, fast path
        Value::DECIMAL(int64_t(12345), 12, 3),      // fallback, non-NULL
        Value(duckdb::LogicalType::DECIMAL(10, 2)), // fallback, NULL
    };

    dbsp_native::BlobWriter w;
    w.row(row);
    auto bytes = w.take();

    dbsp_native::BlobReader r(bytes.data(), bytes.size());
    // const: ColumnVec's non-const operator[] calls mutate(), which
    // invalidates the hash cache -- exactly the pre-seeded value this test
    // exists to check, so any read access here must go through the const
    // (read-only) accessor instead.
    const dbsp_native::DuckDBRow hashed = r.hashed_row();
    REQUIRE(r.done());

    // Round-trip correctness (same bar as the spill codec's own test).
    REQUIRE(hashed.columns.size() == row.size());
    for (size_t i = 0; i < row.size(); i++) {
        INFO("column " << i << ": " << row[i].ToString());
        REQUIRE(row[i].IsNull() == hashed.columns[i].IsNull());
        if (!row[i].IsNull()) {
            REQUIRE(row[i] == hashed.columns[i]);
        }
    }

    // A fresh row built from the identical values, hash left uncomputed
    // (forces the lazy per-Value path on first .hash() call) -- must land
    // on the exact same hash as the pre-seeded one.
    dbsp_native::DuckDBRow lazy;
    std::vector<Value> same = row;
    lazy.columns.assign(std::move(same));
    REQUIRE_FALSE(lazy.columns.hash_valid());
    REQUIRE(hashed.columns.hash() == lazy.columns.hash());
}

namespace {

void setupTypedRowsTable(DuckDBTestHarness &db) {
    db.exec("CREATE TABLE typed_rows (g INTEGER, big BIGINT, amt DOUBLE, "
            "label VARCHAR, flag BOOLEAN, price DECIMAL(10,2))");
    db.exec("SELECT * FROM dbsp_track('typed_rows')");
    db.exec("INSERT INTO typed_rows VALUES "
            "(1, 100, 1.5, 'alpha', true, 12.34), "
            "(2, NULL, NULL, NULL, NULL, NULL), "
            "(3, 300, 3.5, 'gamma', false, 99.99)");
    db.exec("SELECT * FROM dbsp_sync('typed_rows')");
    db.exec("SELECT * FROM dbsp_use_planner(true)");
}

} // namespace

TEST_CASE("checkpoint blob codec: view spanning every codec value shape "
          "round-trips, and a post-restore delta matching an existing "
          "restored group's key bucket-matches instead of duplicating it",
          "[integration][checkpoint][codec]") {
    DuckDBTestHarness db;
    setupTypedRowsTable(db);

    const std::string sql =
        "SELECT g, big, amt, label, flag, price, COUNT(*) AS n "
        "FROM typed_rows GROUP BY g, big, amt, label, flag, price";

    db.exec("SELECT * FROM dbsp_create_view('v_live', '" + sql + "')");
    db.exec("SELECT * FROM dbsp_create_view('v_restore', '" + sql + "')");
    REQUIRE(db.manager().get_view("v_restore")->checkpointable());
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    auto save_result = db.query("SELECT * FROM dbsp_save()");
    REQUIRE_FALSE(save_result->HasError());
    REQUIRE(save_result->GetValue(0, 0).ToString().find(
                "circuit checkpoint: 2 views") != std::string::npos);

    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_restore')")->HasError());

    auto load_result = db.query("SELECT * FROM dbsp_load()");
    REQUIRE_FALSE(load_result->HasError());
    REQUIRE(load_result->GetValue(0, 0).ToString().find(
                "1 from checkpoint") != std::string::npos);
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta: a NEW row with the EXACT SAME key tuple as the
    // already-restored group 1 (g=1, big=100, amt=1.5, label='alpha',
    // flag=true, price=12.34). If the restored group's pre-seeded hash
    // diverged from what a freshly-built row with equal content hashes to,
    // this INSERT would silently land in the wrong bucket (or fail the
    // hash-map's equality fast-reject) and create a second, spurious group
    // instead of bumping n: 1 -> 2 on the existing one.
    db.exec("INSERT INTO typed_rows VALUES "
            "(1, 100, 1.5, 'alpha', true, 12.34)");
    db.exec("SELECT * FROM dbsp_sync('typed_rows')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Also exercise the all-NULL group (g=2) retreating/advancing so the
    // typed-NULL codec path round-trips through a real hash-map lookup too.
    db.exec("INSERT INTO typed_rows VALUES (2, NULL, NULL, NULL, NULL, NULL)");
    db.exec("SELECT * FROM dbsp_sync('typed_rows')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));
}

// Task 1 (restore-tail): PlanRecursiveNode checkpoint state.
//
// PlanRecursiveNode previously had no state_kind() override, so every
// WITH RECURSIVE view fell back to a full rebuild-by-replay on load. This
// extends SERIALIZABLE to UNION ALL recursion whose nested step circuit is
// itself wholly checkpointable, by round-tripping the integrated
// accumulated_/anchor_total_/base_totals_ totals plus the step circuit's
// own per-node state (embedded sub-blob). UNION (set-semantics) recursion
// and a step containing any other unsupported node stay UNSUPPORTED.
//
// `chain` below is the wfp roll-forward shape (linear UNION ALL: a
// per-partition running total over an ordered time spine, `net(p,t,v)` ->
// rf.acc = running sum) — mirrors test_recursive_deletion.cpp's `chain`
// view, replicated locally since no shared header covers it.
namespace {

const char *kChainSql =
    "WITH RECURSIVE rf AS ("
    "  SELECT p, t, v AS acc FROM net WHERE t = 0 "
    "  UNION ALL "
    "  SELECT n.p, n.t, rf.acc + n.v FROM rf JOIN net n "
    "    ON n.p = rf.p AND n.t = rf.t + 1"
    ") SELECT * FROM rf";

void seedNetTable(DuckDBTestHarness &db) {
    db.exec("CREATE TABLE net (p INTEGER, t INTEGER, v DOUBLE)");
    db.exec("SELECT * FROM dbsp_track('net')");
    db.exec("INSERT INTO net VALUES "
            "(1,0,10.0),(1,1,20.0),(1,2,30.0),"
            "(2,0,5.0),(2,1,5.0),(2,2,5.0)");
    db.exec("SELECT * FROM dbsp_sync('net')");
    db.exec("SELECT * FROM dbsp_use_planner(true)");
}

// RAII env-var override for DBSP_REC_MAX_ITER (test-only PlanRecursiveNode
// hook, read once at construction): guarantees the process-wide env var is
// cleared even if a REQUIRE inside the guarded scope fails and unwinds the
// stack. Replicated from test_recursive_deletion.cpp — no shared header
// covers it, and this file's convention (like that one's) is to keep test
// fixtures local.
struct EnvGuard {
    std::string name;
    EnvGuard(std::string n, const char *value) : name(std::move(n)) {
        setenv(name.c_str(), value, 1);
    }
    ~EnvGuard() { unsetenv(name.c_str()); }
};

} // namespace

TEST_CASE("recursive-view checkpoint: state_kind gate for linear UNION ALL, "
          "UNION recursion, and a step containing an unsupported node",
          "[integration][checkpoint][recursive]") {
    DuckDBTestHarness db;
    seedNetTable(db);

    db.exec("SELECT * FROM dbsp_create_view('v_chain', '" +
            std::string(kChainSql) + "')");
    REQUIRE(db.manager().get_view("v_chain")->checkpointable());

    const std::string union_sql =
        "WITH RECURSIVE rf AS ("
        "  SELECT p, t, v AS acc FROM net WHERE t = 0 "
        "  UNION "
        "  SELECT n.p, n.t, rf.acc + n.v FROM rf JOIN net n "
        "    ON n.p = rf.p AND n.t = rf.t + 1"
        ") SELECT * FROM rf";
    db.exec("SELECT * FROM dbsp_create_view('v_union', '" + union_sql + "')");
    REQUIRE_FALSE(db.manager().get_view("v_union")->checkpointable());

    // union_all_ is true here, but the step's JOIN is forced into
    // spill-mode storage (dbsp_spill(true), the same process-global test
    // knob the LEFT-join gate test above uses to force local_spill_right_)
    // -> that inner JOIN node reports UNSUPPORTED -> step_view_->
    // checkpointable() is false -> the recursive node declines regardless
    // of union_all_, proving state_kind() actually composes through the
    // nested circuit rather than only checking union_all_.
    db.exec("SELECT * FROM dbsp_spill(true)");
    db.exec("SELECT * FROM dbsp_create_view('v_chain_spilled', '" +
            std::string(kChainSql) + "')");
    REQUIRE_FALSE(db.manager().get_view("v_chain_spilled")->checkpointable());
    db.exec("SELECT * FROM dbsp_spill(false)"); // process-global: reset
}

TEST_CASE("recursive-view checkpoint: linear UNION ALL round-trips through "
          "save/restore, mid-chain retractions resume the signed path",
          "[integration][checkpoint][recursive]") {
    DuckDBTestHarness db;
    seedNetTable(db);

    // Twin A (v_live) stays live for the whole test. Twin B (v_restore) is
    // checkpointed, dropped (in-memory only), and reloaded cold from the
    // checkpoint.
    db.exec("SELECT * FROM dbsp_create_view('v_live', '" +
            std::string(kChainSql) + "')");
    db.exec("SELECT * FROM dbsp_create_view('v_restore', '" +
            std::string(kChainSql) + "')");
    REQUIRE(db.manager().get_view("v_restore")->checkpointable());
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    auto save_result = db.query("SELECT * FROM dbsp_save()");
    REQUIRE_FALSE(save_result->HasError());
    REQUIRE(save_result->GetValue(0, 0).ToString().find(
                "circuit checkpoint: 2 views") != std::string::npos);

    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_restore')")->HasError());

    auto load_result = db.query("SELECT * FROM dbsp_load()");
    REQUIRE_FALSE(load_result->HasError());
    // Only v_restore needed reloading (v_live was still live, skipped).
    REQUIRE(load_result->GetValue(0, 0).ToString().find(
                "1 from checkpoint") != std::string::npos);
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 1: an ordinary insert-only extension (p=1, t=3) —
    // exercises the generic insert-only admit()/iterate() path against
    // restored accumulated_/step_view_ state.
    db.exec("INSERT INTO net VALUES (1, 3, 40.0)");
    db.exec("SELECT * FROM dbsp_sync('net')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 2: UPDATE = retraction + insertion on a mid-chain
    // link (p=1, t=1) — must resume the SIGNED retraction path from the
    // restored accumulated_/anchor_total_/base_totals_/step_view_ state,
    // propagating the new running total through t=2,3 exactly as the
    // continuously-live twin does.
    db.exec("UPDATE net SET v = 100.0 WHERE p = 1 AND t = 1");
    db.exec("SELECT * FROM dbsp_sync('net')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Post-restore delta 3: a mid-chain DELETE severs everything downstream
    // -- same signed path, a pure retraction with no insertion in the same
    // delta.
    db.exec("DELETE FROM net WHERE p = 1 AND t = 1");
    db.exec("SELECT * FROM dbsp_sync('net')");
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));
}

TEST_CASE("recursive-view checkpoint: post-restore max_iterations_ trip "
          "falls back to a correct recompute()",
          "[integration][checkpoint][recursive]") {
    // Force the signed path to trip max_iterations_ AFTER restore
    // (DBSP_REC_MAX_ITER=2, a chain far deeper than that beyond the
    // retraction point) and check both halves of fallback correctness: the
    // fallback actually fires (g_recompute_invocations increments) AND it
    // produces the right answer — proving recompute() rebuilds correctly
    // from the just-restored anchor_total_/base_totals_ + step_view_ reset
    // state, not merely that it runs without crashing.
    //
    // The cap is read once at PlanRecursiveNode construction (both at cold
    // create AND at the construction restore_view_state() runs right after
    // — see state_kind()'s doc comment), and applies to EVERY commit,
    // including insert-only ones. So the chain is built one row per commit
    // (each single-row append only ever needs the seed admission, zero
    // iterate() rounds, comfortably under cap=2), same shape
    // test_recursive_deletion.cpp's own max_iterations test uses.
    EnvGuard cap("DBSP_REC_MAX_ITER", "2");
    DuckDBTestHarness db;
    db.exec("CREATE TABLE net (p INTEGER, t INTEGER, v DOUBLE)");
    db.exec("SELECT * FROM dbsp_track('net')");
    db.exec("INSERT INTO net VALUES (1, 0, 10.0)");
    db.exec("SELECT * FROM dbsp_sync('net')");
    db.exec("SELECT * FROM dbsp_use_planner(true)");

    db.exec("SELECT * FROM dbsp_create_view('v_live', '" +
            std::string(kChainSql) + "')");
    db.exec("SELECT * FROM dbsp_create_view('v_restore', '" +
            std::string(kChainSql) + "')");

    for (int t = 1; t <= 7; t++) {
        db.exec("INSERT INTO net VALUES (1, " + std::to_string(t) + ", " +
                std::to_string(10.0 * (t + 1)) + ")");
        db.exec("SELECT * FROM dbsp_sync('net')");
    }
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));
    REQUIRE(db.getViewRows("v_restore").size() == 8);

    auto save_result = db.query("SELECT * FROM dbsp_save()");
    REQUIRE_FALSE(save_result->HasError());
    REQUIRE(save_result->GetValue(0, 0).ToString().find(
                "circuit checkpoint: 2 views") != std::string::npos);

    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v_restore')")->HasError());
    auto load_result = db.query("SELECT * FROM dbsp_load()");
    REQUIRE_FALSE(load_result->HasError());
    REQUIRE(load_result->GetValue(0, 0).ToString().find(
                "1 from checkpoint") != std::string::npos);
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Retracting t=1 must propagate through t=2..7 -- 6 hops, far past
    // cap=2 -- so this commit trips max_iterations_ on BOTH twins and
    // falls back to recompute(). v_restore's recompute() rebuilds entirely
    // from its just-restored state; if restore had left accumulated_/
    // anchor_total_/base_totals_/step_view_ stale or partial, v_restore
    // would diverge from the continuously-live v_live right here.
    size_t before = dbsp_native::g_recompute_invocations.load();
    db.exec("DELETE FROM net WHERE p = 1 AND t = 1");
    db.exec("SELECT * FROM dbsp_sync('net')");
    REQUIRE(dbsp_native::g_recompute_invocations.load() > before);
    REQUIRE(snapshotView(db, "v_live") == snapshotView(db, "v_restore"));

    // Hand-computed truth: retracting t=1 severs the chain; only t=0 (the
    // anchor row, acc = 10.0) survives.
    auto rows = db.getViewRows("v_restore");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0][0].GetValue<int32_t>() == 1);
    REQUIRE(rows[0][1].GetValue<int32_t>() == 0);
    REQUIRE(rows[0][2].GetValue<double>() == 10.0);
}

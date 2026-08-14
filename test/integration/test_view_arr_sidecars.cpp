// View-sourced arrangement sidecars (design note at save_checkpoint's
// sidecar loop in dbsp_cdc.hpp).
//
// A TABLE-sourced shared arrangement's sidecar is trusted via the source
// table's load-verified watermark. A VIEW-sourced arrangement has no
// independently verifiable source, so its sidecar is stamped with the
// checkpoint's per-save random id (the _dbsp_ckpt 'saveid' row, written
// in the same transaction as the view blobs): adoption at register time
// requires the source view to be PENDING (its stash was accepted) AND
// the file stamp to equal the loaded checkpoint's save-id. These tests
// prove:
//   1. A reattach ADOPTS the view-sourced sidecar instead of scanning the
//      view's __mv_ backing table (g_view_arr_sidecar_adopts moves,
//      g_arr_backfills does not), and stays parity-clean through both the
//      probe path (local-side edit) and the write path (source edit).
//   2. A save while the view is still pending re-stamps the clean sidecar
//      in place (g_view_arr_sidecar_restamps) — the 0396972
//      pending-preserve semantics extended to the arrangement file — and
//      the next load adopts again.
//   3. A source edit AFTER the save declines the stash (stale closure),
//      so no adoption happens and the rebuild is correct.
//   4. A sidecar file from an OLDER save (stale stamp) is rejected; the
//      arrangement falls back to the backfill scan and stays correct.

#include "catch.hpp"
#include "../test_helpers.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace duckdb;

namespace {

// File-backed variant of DuckDBTestHarness: view-arrangement sidecars
// need spill_durable_dir_, which only exists next to an on-disk database
// file (<dbfile>.dbsp_spill). Fresh temp dir per instance.
class FileDBHarness {
public:
    FileDBHarness() {
        static std::atomic<int> seq{0};
        dir_ = (std::filesystem::temp_directory_path() /
                ("dbsp_viewarr_" + std::to_string(::getpid()) + "_" +
                 std::to_string(seq.fetch_add(1))))
                   .string();
        std::filesystem::create_directories(dir_);
        path_ = dir_ + "/model.duckdb";
        db_ = std::make_unique<DuckDB>(path_);
        dbsp_native::get_cdc_registry().take(db_->instance.get());
        try {
            ExtensionLoader loader(*db_->instance, "dbsp");
            dbsp_duckdb_cpp_init(loader);
        } catch (const std::exception &) {
            // Registration failed - tests will fail with descriptive errors
        }
        conn_ = std::make_unique<Connection>(*db_);
    }

    ~FileDBHarness() {
        conn_.reset();
        dbsp_native::get_cdc_registry().take(db_->instance.get());
        db_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    dbsp_native::CDCManager &manager() {
        return dbsp_native::get_cdc_manager(*db_->instance);
    }

    unique_ptr<MaterializedQueryResult> query(const std::string &sql) {
        return conn_->Query(sql);
    }

    void exec(const std::string &sql) {
        auto result = conn_->Query(sql);
        INFO("SQL: " << sql);
        if (result->HasError()) {
            INFO("Error: " << result->GetError());
        }
        REQUIRE_FALSE(result->HasError());
    }

    std::string spillDir() const { return path_ + ".dbsp_spill"; }

private:
    std::string dir_;
    std::string path_;
    std::unique_ptr<DuckDB> db_;
    std::unique_ptr<Connection> conn_;
};

std::vector<std::pair<std::vector<std::string>, int64_t>>
snapshotView(FileDBHarness &db, const std::string &name) {
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

void setupSources(FileDBHarness &db) {
    // DOUBLE value: SUM(DOUBLE) stays DOUBLE (packed-codec-clean), so the
    // shared arrangement over v1 is packed_ok and sidecar-eligible.
    db.exec("CREATE TABLE items (id INTEGER, cat VARCHAR, value DOUBLE)");
    db.exec("SELECT * FROM dbsp_track('items')");
    db.exec("INSERT INTO items VALUES (1, 'a', 10), (2, 'a', 20), "
            "(3, 'b', 30), (4, 'b', 40)");
    db.exec("SELECT * FROM dbsp_sync('items')");
    db.exec("CREATE TABLE side1 (cat VARCHAR, tag1 VARCHAR)");
    db.exec("SELECT * FROM dbsp_track('side1')");
    db.exec("INSERT INTO side1 VALUES ('a', 't1'), ('b', 't2')");
    db.exec("SELECT * FROM dbsp_sync('side1')");
    db.exec("SELECT * FROM dbsp_use_planner(true)");
}

// v1<sfx> is the RIGHT (probe-target) side of v2<sfx>'s LEFT JOIN — a
// shared arrangement is registered over it (same shape as the lazy
// restore tests). The "_live" pair is a continuously-live oracle.
void createPair(FileDBHarness &db, const std::string &sfx) {
    db.exec("SELECT * FROM dbsp_create_view('v1" + sfx + "', "
            "'SELECT cat, SUM(value) AS total FROM items GROUP BY cat')");
    db.exec("SELECT * FROM dbsp_create_view('v2" + sfx + "', "
            "'SELECT s.cat AS cat, s.tag1 AS tag1, j.total AS total FROM "
            "side1 s LEFT JOIN v1" + sfx + " j ON s.cat = j.cat')");
}

void setupSavedPairs(FileDBHarness &db) {
    setupSources(db);
    createPair(db, "");
    createPair(db, "_live");
    db.exec("SELECT * FROM dbsp_mv_tables(true)");
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
}

void dropPrimaryPair(FileDBHarness &db) {
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v2')")->HasError());
    REQUIRE_FALSE(db.query("SELECT dbsp_drop('v1')")->HasError());
}

std::map<std::string, std::string> readSharrFiles(const std::string &dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("sharr_", 0) == 0 &&
            e.path().extension() == ".flat") {
            std::ifstream in(e.path(), std::ios::binary);
            out[e.path().string()] = std::string(
                std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
        }
    }
    return out;
}

} // namespace

TEST_CASE("view-arr sidecars: reattach adopts without a backfill scan and "
          "stays parity-clean through edits",
          "[integration][checkpoint][view_arr_sidecar]") {
    FileDBHarness db;
    setupSavedPairs(db);
    // The save wrote sidecar files for both pairs' shared arrangements.
    REQUIRE(readSharrFiles(db.spillDir()).size() >= 2);

    dropPrimaryPair(db);
    const size_t adopts0 = dbsp_native::g_view_arr_sidecar_adopts.load();
    const size_t backfills0 = dbsp_native::g_arr_backfills.load();
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_load()")->HasError());
    REQUIRE(db.manager().pending_restore_count() == 2);
    // v2's arrangement over pending v1 was adopted from its sidecar; no
    // fill site scanned v1's __mv_ backing table.
    REQUIRE(dbsp_native::g_view_arr_sidecar_adopts.load() == adopts0 + 1);
    REQUIRE(dbsp_native::g_arr_backfills.load() == backfills0);

    // Probe path: a local-side edit makes v2's LEFT JOIN probe v1's
    // adopted arrangement (the reviewer shape — a wrong/empty adopt would
    // null-pad the new row).
    db.exec("INSERT INTO side1 VALUES ('a', 't3')");
    db.exec("SELECT * FROM dbsp_sync('side1')");
    // Realize skipped the backfill for the adopted arrangement too.
    REQUIRE(dbsp_native::g_arr_backfills.load() == backfills0);
    REQUIRE(snapshotView(db, "v2") == snapshotView(db, "v2_live"));

    // Write path: a source edit's delta flows THROUGH the adopted
    // arrangement (apply_to_arrangements) and future probes see it.
    db.exec("INSERT INTO items VALUES (5, 'a', 15)");
    db.exec("SELECT * FROM dbsp_sync('items')");
    REQUIRE(snapshotView(db, "v1") == snapshotView(db, "v1_live"));
    REQUIRE(snapshotView(db, "v2") == snapshotView(db, "v2_live"));
}

TEST_CASE("view-arr sidecars: a save while the view is still pending "
          "re-stamps the clean sidecar and the next load adopts it again",
          "[integration][checkpoint][view_arr_sidecar]") {
    FileDBHarness db;
    setupSavedPairs(db);
    dropPrimaryPair(db);
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_load()")->HasError());
    REQUIRE(db.manager().pending_restore_count() == 2);

    // Save while v1/v2 are still pending: their checkpoint bytes are
    // preserved verbatim (0396972) and their adopted, untouched
    // arrangement sidecar must move to the NEW save-id by an in-place
    // re-stamp — not a re-fold, and never a clobber.
    const size_t restamps0 = dbsp_native::g_view_arr_sidecar_restamps.load();
    const size_t adopts0 = dbsp_native::g_view_arr_sidecar_adopts.load();
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
    REQUIRE(dbsp_native::g_view_arr_sidecar_restamps.load() >= restamps0 + 1);
    REQUIRE(db.manager().pending_restore_count() == 2); // save realized nothing

    dropPrimaryPair(db); // discards the pending stash; ckpt rows survive
    const size_t backfills0 = dbsp_native::g_arr_backfills.load();
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_load()")->HasError());
    REQUIRE(db.manager().pending_restore_count() == 2);
    REQUIRE(dbsp_native::g_view_arr_sidecar_adopts.load() == adopts0 + 1);
    REQUIRE(dbsp_native::g_arr_backfills.load() == backfills0);

    db.exec("INSERT INTO side1 VALUES ('b', 't4')");
    db.exec("SELECT * FROM dbsp_sync('side1')");
    REQUIRE(snapshotView(db, "v2") == snapshotView(db, "v2_live"));
}

TEST_CASE("view-arr sidecars: a source edit after the save declines the "
          "stash, so nothing is adopted and the rebuild is correct",
          "[integration][checkpoint][view_arr_sidecar]") {
    FileDBHarness db;
    setupSavedPairs(db);
    // items' live watermark no longer matches the checkpoint: v1/v2's
    // source closure is stale, their stashes are declined, and the
    // sidecar (keyed to the pending-stash gate) must not be adopted.
    db.exec("INSERT INTO items VALUES (6, 'b', 5)");
    db.exec("SELECT * FROM dbsp_sync('items')");
    dropPrimaryPair(db);

    const size_t adopts0 = dbsp_native::g_view_arr_sidecar_adopts.load();
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_load()")->HasError());
    REQUIRE(db.manager().pending_restore_count() == 0); // cold replay
    REQUIRE(dbsp_native::g_view_arr_sidecar_adopts.load() == adopts0);
    REQUIRE(snapshotView(db, "v1") == snapshotView(db, "v1_live"));
    REQUIRE(snapshotView(db, "v2") == snapshotView(db, "v2_live"));
}

TEST_CASE("view-arr sidecars: a sidecar from an older save is rejected "
          "and the arrangement backfills instead",
          "[integration][checkpoint][view_arr_sidecar]") {
    FileDBHarness db;
    setupSavedPairs(db);
    // Keep the save-#1 sidecar bytes, move the data + checkpoint to
    // save #2, then put the OLD files back: stale stamps under a valid
    // checkpoint — the exact wrong-adopt hazard the save-id blocks.
    const auto old_files = readSharrFiles(db.spillDir());
    REQUIRE_FALSE(old_files.empty());
    db.exec("INSERT INTO items VALUES (7, 'a', 5)");
    db.exec("SELECT * FROM dbsp_sync('items')");
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_save()")->HasError());
    for (const auto &[p, bytes] : old_files) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    dropPrimaryPair(db);

    const size_t adopts0 = dbsp_native::g_view_arr_sidecar_adopts.load();
    const size_t backfills0 = dbsp_native::g_arr_backfills.load();
    REQUIRE_FALSE(db.query("SELECT * FROM dbsp_load()")->HasError());
    REQUIRE(db.manager().pending_restore_count() == 2); // ckpt is valid
    REQUIRE(dbsp_native::g_view_arr_sidecar_adopts.load() == adopts0);

    // Realize falls back to the scan and the result is still correct.
    db.exec("INSERT INTO side1 VALUES ('b', 't5')");
    db.exec("SELECT * FROM dbsp_sync('side1')");
    REQUIRE(dbsp_native::g_arr_backfills.load() > backfills0);
    REQUIRE(snapshotView(db, "v2") == snapshotView(db, "v2_live"));
}

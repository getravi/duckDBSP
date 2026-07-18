// Differential tests for the engine hook (patches/v1.5.4-dbsp-txn-callback):
// TransactionModificationCallback must receive, per modified table per commit,
// the exact full-width pre-images (old_rows, Z-weight -1) and post-images
// (new_rows, Z-weight +1) of that transaction's tuple modifications.
// See docs/superpowers/plans/2026-07-18-engine-hook-impl.md (A3).
#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/transaction/undo_buffer.hpp"

#include <algorithm>
#include <string>
#include <vector>

using duckdb::ColumnDataCollection;
using duckdb::Connection;
using duckdb::DataTableInfo;
using duckdb::DBConfig;
using duckdb::DuckDB;
using duckdb::TransactionModificationCallback;
using duckdb::TransactionModifications;
using duckdb::Value;

namespace {

using Row = std::vector<Value>;

struct HookEvent {
	std::string table;
	std::vector<Row> old_rows;
	std::vector<Row> new_rows;
};

std::vector<Row> materialize(ColumnDataCollection &cdc) {
	std::vector<Row> rows;
	for (auto &chunk : cdc.Chunks()) {
		for (duckdb::idx_t r = 0; r < chunk.size(); r++) {
			Row row;
			for (duckdb::idx_t c = 0; c < chunk.ColumnCount(); c++) {
				row.push_back(chunk.GetValue(c, r));
			}
			rows.push_back(std::move(row));
		}
	}
	return rows;
}

// order-insensitive comparison: sort by string rendering
void sort_rows(std::vector<Row> &rows) {
	std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
		std::string sa, sb;
		for (auto &v : a)
			sa += v.ToString() + "|";
		for (auto &v : b)
			sb += v.ToString() + "|";
		return sa < sb;
	});
}

bool rows_equal(std::vector<Row> got, std::vector<Row> want) {
	if (got.size() != want.size())
		return false;
	sort_rows(got);
	sort_rows(want);
	for (size_t i = 0; i < got.size(); i++) {
		if (got[i].size() != want[i].size())
			return false;
		for (size_t c = 0; c < got[i].size(); c++) {
			if (!(got[i][c] == want[i][c]))
				return false;
		}
	}
	return true;
}

Row iv(int i, const char *s) {
	return Row {Value::INTEGER(i), Value(s)};
}

struct HookedDB {
	DuckDB db;
	Connection con;
	std::vector<HookEvent> events;

	HookedDB() : db(nullptr), con(db) {
		TransactionModificationCallback cb;
		cb.on_commit = [this](duckdb::ClientContext &, DataTableInfo &info, TransactionModifications &m) {
			HookEvent ev;
			ev.table = info.GetTableName();
			if (m.old_rows) {
				ev.old_rows = materialize(*m.old_rows);
			}
			if (m.new_rows) {
				ev.new_rows = materialize(*m.new_rows);
			}
			events.push_back(std::move(ev));
		};
		// side registry (ABI-neutral surface); erased in ~DatabaseInstance
		duckdb::RegisterTxnModificationCallback(*db.instance, std::move(cb));
	}

	void q(const std::string &sql) {
		auto res = con.Query(sql);
		REQUIRE_FALSE(res->HasError());
	}
	// events for one table only (ignores other tables' events)
	std::vector<HookEvent> for_table(const std::string &name) {
		std::vector<HookEvent> out;
		for (auto &e : events) {
			if (e.table == name) {
				out.push_back(e);
			}
		}
		return out;
	}
};

} // namespace

TEST_CASE("engine hook: plain INSERT streams new images only", "[engine_hook]") {
	HookedDB h;
	h.q("CREATE TABLE t(i INTEGER, s VARCHAR)");
	h.events.clear();
	h.q("INSERT INTO t VALUES (1,'a'), (2,'b'), (3,'c')");

	auto evs = h.for_table("t");
	REQUIRE(evs.size() == 1);
	REQUIRE(evs[0].old_rows.empty());
	REQUIRE(rows_equal(evs[0].new_rows, {iv(1, "a"), iv(2, "b"), iv(3, "c")}));
}

TEST_CASE("engine hook: UPDATE streams old + new images", "[engine_hook]") {
	HookedDB h;
	h.q("CREATE TABLE t(i INTEGER, s VARCHAR)");
	h.q("INSERT INTO t VALUES (1,'a'), (2,'b'), (3,'c')");
	h.events.clear();
	h.q("UPDATE t SET i = i + 10 WHERE i = 2");

	auto evs = h.for_table("t");
	REQUIRE(evs.size() == 1);
	REQUIRE(rows_equal(evs[0].old_rows, {iv(2, "b")}));
	REQUIRE(rows_equal(evs[0].new_rows, {iv(12, "b")}));
}

TEST_CASE("engine hook: DELETE streams old images only", "[engine_hook]") {
	HookedDB h;
	h.q("CREATE TABLE t(i INTEGER, s VARCHAR)");
	h.q("INSERT INTO t VALUES (1,'a'), (2,'b'), (3,'c')");
	h.events.clear();
	h.q("DELETE FROM t WHERE i >= 2");

	auto evs = h.for_table("t");
	REQUIRE(evs.size() == 1);
	REQUIRE(rows_equal(evs[0].old_rows, {iv(2, "b"), iv(3, "c")}));
	REQUIRE(evs[0].new_rows.empty());
}

TEST_CASE("engine hook: mixed txn — insert + update + delete in one commit", "[engine_hook]") {
	HookedDB h;
	h.q("CREATE TABLE t(i INTEGER, s VARCHAR)");
	h.q("INSERT INTO t VALUES (1,'a'), (2,'b'), (3,'c')");
	h.events.clear();
	h.q("BEGIN");
	h.q("INSERT INTO t VALUES (10,'x')");
	h.q("UPDATE t SET s = 'B' WHERE i = 2");
	h.q("DELETE FROM t WHERE i = 3");
	h.q("COMMIT");

	auto evs = h.for_table("t");
	REQUIRE(evs.size() == 1);
	// old: pre-images of the updated and the deleted row
	REQUIRE(rows_equal(evs[0].old_rows, {iv(2, "b"), iv(3, "c")}));
	// new: the insert and the updated row's post-image
	REQUIRE(rows_equal(evs[0].new_rows, {iv(10, "x"), iv(2, "B")}));
}

TEST_CASE("engine hook: update then delete of same row nets to one old image", "[engine_hook]") {
	HookedDB h;
	h.q("CREATE TABLE t(i INTEGER, s VARCHAR)");
	h.q("INSERT INTO t VALUES (1,'a'), (2,'b')");
	h.events.clear();
	h.q("BEGIN");
	h.q("UPDATE t SET i = 99 WHERE i = 1");
	h.q("DELETE FROM t WHERE i = 99");
	h.q("COMMIT");

	auto evs = h.for_table("t");
	REQUIRE(evs.size() == 1);
	// net effect: row (1,'a') disappeared. No trace of the intermediate 99.
	REQUIRE(rows_equal(evs[0].old_rows, {iv(1, "a")}));
	REQUIRE(evs[0].new_rows.empty());
}

TEST_CASE("engine hook: insert then update of same row in one txn yields final image", "[engine_hook]") {
	HookedDB h;
	h.q("CREATE TABLE t(i INTEGER, s VARCHAR)");
	h.events.clear();
	h.q("BEGIN");
	h.q("INSERT INTO t VALUES (20,'y')");
	h.q("UPDATE t SET s = 'z' WHERE i = 20");
	h.q("COMMIT");

	auto evs = h.for_table("t");
	REQUIRE(evs.size() == 1);
	REQUIRE(evs[0].old_rows.empty());
	REQUIRE(rows_equal(evs[0].new_rows, {iv(20, "z")}));
}

TEST_CASE("engine hook: insert then delete of same row in one txn nets to zero", "[engine_hook]") {
	HookedDB h;
	h.q("CREATE TABLE t(i INTEGER, s VARCHAR)");
	h.events.clear();
	h.q("BEGIN");
	h.q("INSERT INTO t VALUES (30,'gone')");
	h.q("DELETE FROM t WHERE i = 30");
	h.q("COMMIT");

	// either no event at all, or an event with empty old and new
	for (auto &ev : h.for_table("t")) {
		REQUIRE(ev.old_rows.empty());
		REQUIRE(ev.new_rows.empty());
	}
}

TEST_CASE("engine hook: rollback fires nothing", "[engine_hook]") {
	HookedDB h;
	h.q("CREATE TABLE t(i INTEGER, s VARCHAR)");
	h.q("INSERT INTO t VALUES (1,'a')");
	h.events.clear();
	h.q("BEGIN");
	h.q("INSERT INTO t VALUES (2,'b')");
	h.q("UPDATE t SET s = 'x' WHERE i = 1");
	h.q("ROLLBACK");

	REQUIRE(h.for_table("t").empty());
}

TEST_CASE("engine hook: two tables in one txn fire separate per-table events", "[engine_hook]") {
	HookedDB h;
	h.q("CREATE TABLE a(i INTEGER, s VARCHAR)");
	h.q("CREATE TABLE b(i INTEGER, s VARCHAR)");
	h.events.clear();
	h.q("BEGIN");
	h.q("INSERT INTO a VALUES (1,'a')");
	h.q("INSERT INTO b VALUES (2,'b')");
	h.q("COMMIT");

	auto ea = h.for_table("a");
	auto eb = h.for_table("b");
	REQUIRE(ea.size() == 1);
	REQUIRE(eb.size() == 1);
	REQUIRE(rows_equal(ea[0].new_rows, {iv(1, "a")}));
	REQUIRE(rows_equal(eb[0].new_rows, {iv(2, "b")}));
}

TEST_CASE("engine hook: multi-vector batch (5000 rows) survives chunked fetch", "[engine_hook]") {
	HookedDB h;
	h.q("CREATE TABLE t(i INTEGER, s VARCHAR)");
	h.events.clear();
	h.q("INSERT INTO t SELECT range::INTEGER, 'r' || range::VARCHAR FROM range(5000)");

	auto evs = h.for_table("t");
	REQUIRE(evs.size() == 1);
	REQUIRE(evs[0].old_rows.empty());
	REQUIRE(evs[0].new_rows.size() == 5000);

	// spot-check first/last by value
	sort_rows(evs[0].new_rows);
	bool found0 = false, found4999 = false;
	for (auto &r : evs[0].new_rows) {
		if (r[0] == Value::INTEGER(0) && r[1] == Value("r0"))
			found0 = true;
		if (r[0] == Value::INTEGER(4999) && r[1] == Value("r4999"))
			found4999 = true;
	}
	REQUIRE(found0);
	REQUIRE(found4999);

	// now update all of them — old and new must both carry 5000 rows
	h.events.clear();
	h.q("UPDATE t SET i = i + 10000");
	evs = h.for_table("t");
	REQUIRE(evs.size() == 1);
	REQUIRE(evs[0].old_rows.size() == 5000);
	REQUIRE(evs[0].new_rows.size() == 5000);
}

TEST_CASE("engine hook: no tuple changes means no event", "[engine_hook]") {
	HookedDB h;
	h.q("CREATE TABLE t(i INTEGER, s VARCHAR)");
	h.events.clear();
	h.q("UPDATE t SET i = 1 WHERE i = 42"); // matches nothing
	h.q("SELECT * FROM t");
	REQUIRE(h.for_table("t").empty());
}

"""D2 regression test: materialized views over ATTACHed database tables.

Covers the NuEPM open_model_db pattern: an in-memory main catalog with model
databases attached, views defined over attached tables, canonical
catalog.schema.table keying (bare vs qualified refs), same-name tables in
two catalogs, and notify/sync against qualified names.

Run: python test_attached_catalog.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import sys
import tempfile

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 60


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)

with tempfile.TemporaryDirectory() as tmp:
    a_path = os.path.join(tmp, "model_a.duckdb")
    b_path = os.path.join(tmp, "model_b.duckdb")
    for path, base in ((a_path, 0), (b_path, 100)):
        setup = duckdb.connect(path)
        setup.execute("CREATE TABLE li (k INTEGER, v DOUBLE)")
        setup.execute(f"INSERT INTO li SELECT i % 3, i + {base} FROM range(6) t(i)")
        setup.close()

    conn = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    conn.execute(f"LOAD '{EXT}'")
    conn.execute(f"ATTACH '{a_path}' AS a (READ_WRITE)")
    conn.execute(f"ATTACH '{b_path}' AS b (READ_WRITE)")

    # The spike Q2 failure case: MV over an attached table.
    conn.execute(
        "CREATE MATERIALIZED VIEW a_sum AS SELECT k, SUM(v) AS s FROM a.li GROUP BY k"
    )
    # Same-name table in a second catalog must not collide.
    conn.execute(
        "CREATE MATERIALIZED VIEW b_sum AS SELECT k, SUM(v) AS s FROM b.li GROUP BY k"
    )

    def view(name):
        return dict(conn.execute(f"SELECT k, s FROM dbsp_query('{name}')").fetchall())

    def truth(cat):
        return dict(conn.execute(f"SELECT k, SUM(v) FROM {cat}.li GROUP BY k").fetchall())

    assert view("a_sum") == truth("a"), f"a_sum wrong: {view('a_sum')} != {truth('a')}"
    assert view("b_sum") == truth("b"), f"b_sum wrong: {view('b_sum')} != {truth('b')}"
    assert view("a_sum") != view("b_sum"), "catalogs must not collide"

    # Auto-sync propagation of edits to each attached table, only to its view.
    before_b = view("b_sum")
    conn.execute("INSERT INTO a.li VALUES (0, 50.0)")
    assert view("a_sum") == truth("a"), f"a edit lost: {view('a_sum')} != {truth('a')}"
    assert view("b_sum") == before_b, "a edit leaked into b_sum"

    conn.execute("DELETE FROM b.li WHERE k = 1")
    assert view("b_sum") == truth("b"), f"b delete lost: {view('b_sum')} != {truth('b')}"

    # Notify path with a qualified name resolves to the same tracked key.
    conn.execute("SELECT * FROM dbsp_auto_sync(false)")
    old = conn.execute("SELECT v FROM a.li WHERE k = 2 AND v = 2.0").fetchone()
    if old is not None:
        conn.execute("UPDATE a.li SET v = 99.0 WHERE k = 2 AND v = 2.0")
        conn.execute("SELECT * FROM dbsp_notify_delete('a.li', 2, 2.0)")
        conn.execute("SELECT * FROM dbsp_notify_insert('a.li', 2, 99.0)")
        assert view("a_sum") == truth("a"), f"notify path wrong: {view('a_sum')} != {truth('a')}"
    conn.execute("SELECT * FROM dbsp_auto_sync(true)")

    # dbsp_changes works for attached-table views.
    conn.execute("INSERT INTO a.li VALUES (0, 1.0)")
    delta = conn.execute("SELECT k, s, weight FROM dbsp_changes('a_sum')").fetchall()
    assert delta and all(w in (-1, 1) for _, _, w in delta), f"delta malformed: {delta}"

    # DETACH with live views: graceful degradation — the view keeps its
    # last state and stays queryable/droppable; sync fails cleanly; the
    # engine keeps working for other tables.
    conn.execute("DETACH b")
    assert view("b_sum") is not None  # stale snapshot still readable
    conn.execute("INSERT INTO a.li VALUES (1, 2.0)")  # must not crash
    assert view("a_sum") == truth("a"), "a_sum broken after detach of b"
    conn.execute("SELECT dbsp_drop('b_sum')")

    conn.close()

signal.alarm(0)
print("PASS: views over attached catalogs track, propagate, and stay isolated", flush=True)

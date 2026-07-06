"""D3 regression test: zero-arg dbsp_save()/dbsp_load() persist view
definitions in the database file's _dbsp_views table, so views travel with
the file (and copies of it). Also covers column-name fidelity for
positional GROUP BY (planner naming fix).

Run: python test_table_persistence.py <path-to-dbsp.duckdb_extension>
"""

import os
import shutil
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


def open_db(path):
    conn = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    conn.execute(f"LOAD '{EXT}'")
    return conn


with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "model.duckdb")

    # Session 1: tables, two views (one with positional GROUP BY), save.
    conn = open_db(path)
    conn.execute("CREATE TABLE li_1 (dim_0 INTEGER, dim_1 INTEGER, value DOUBLE)")
    conn.execute("INSERT INTO li_1 SELECT i % 4, i % 2, i * 1.5 FROM range(20) t(i)")
    conn.execute("CREATE TABLE closure (child_id INTEGER, parent_id INTEGER)")
    conn.execute("INSERT INTO closure SELECT i, 100 + i // 2 FROM range(4) t(i)")
    conn.execute(
        "CREATE MATERIALIZED VIEW rollup_sum AS "
        "SELECT c.parent_id, l.dim_1, SUM(l.value) AS value "
        "FROM li_1 l JOIN closure c ON l.dim_0 = c.child_id GROUP BY 1, 2"
    )
    conn.execute("CREATE MATERIALIZED VIEW li_totals AS SELECT dim_1, SUM(value) AS s FROM li_1 GROUP BY dim_1")

    r = conn.execute("SELECT * FROM dbsp_query('rollup_sum')")
    cols_created = [d[0] for d in r.description]
    assert cols_created == ["parent_id", "dim_1", "value"], f"creation names wrong: {cols_created}"

    expected_rollup = sorted(conn.execute(
        "SELECT c.parent_id, l.dim_1, SUM(l.value) FROM li_1 l "
        "JOIN closure c ON l.dim_0 = c.child_id GROUP BY 1, 2"
    ).fetchall())

    saved = conn.execute("SELECT * FROM dbsp_save()").fetchone()[0]
    assert "_dbsp_views" in saved, f"save failed: {saved}"
    n = conn.execute("SELECT COUNT(*) FROM _dbsp_views").fetchone()[0]
    assert n == 2, f"expected 2 persisted defs, got {n}"
    conn.close()

    # Session 2 (same process, D1 teardown ran): load from the table.
    conn = open_db(path)
    msg = conn.execute("SELECT * FROM dbsp_load()").fetchone()[0]
    assert "Loaded" in msg, f"load failed: {msg}"
    r = conn.execute("SELECT * FROM dbsp_query('rollup_sum')")
    cols_loaded = [d[0] for d in r.description]
    assert cols_loaded == ["parent_id", "dim_1", "value"], f"loaded names wrong: {cols_loaded}"
    got = sorted(conn.execute("SELECT parent_id, dim_1, value FROM dbsp_query('rollup_sum')").fetchall())
    assert got == expected_rollup, f"loaded values wrong: {got} != {expected_rollup}"

    # Loaded views are live: edits propagate.
    conn.execute("INSERT INTO li_1 VALUES (0, 0, 100.0)")
    truth = sorted(conn.execute(
        "SELECT c.parent_id, l.dim_1, SUM(l.value) FROM li_1 l "
        "JOIN closure c ON l.dim_0 = c.child_id GROUP BY 1, 2"
    ).fetchall())
    got = sorted(conn.execute("SELECT parent_id, dim_1, value FROM dbsp_query('rollup_sum')").fetchall())
    assert got == truth, f"post-load edit lost: {got} != {truth}"
    conn.close()

    # Backup scenario: file copy carries the views.
    copy_path = os.path.join(tmp, "backup.duckdb")
    shutil.copyfile(path, copy_path)
    conn = open_db(copy_path)
    conn.execute("SELECT * FROM dbsp_load()")
    views = [v[0] for v in conn.execute("SELECT * FROM dbsp_views()").fetchall()]
    assert sorted(views) == ["li_totals", "rollup_sum"], f"backup views wrong: {views}"
    conn.close()

signal.alarm(0)
print("PASS: definitions persist in _dbsp_views, travel with file copies, names intact", flush=True)

"""D1 regression test: two database instances in one process, each with its
own DBSP state (per-instance CDCManager registry).

Before D1 the CDCManager was one process-wide singleton keyed by bare table
names, so two instances with same-named tables would collide.

Run: python test_multi_instance.py <path-to-dbsp.duckdb_extension>
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


def open_db(path: str) -> duckdb.DuckDBPyConnection:
    conn = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    conn.execute(f"LOAD '{EXT}'")
    return conn


with tempfile.TemporaryDirectory() as tmp:
    a = open_db(os.path.join(tmp, "a.duckdb"))
    b = open_db(os.path.join(tmp, "b.duckdb"))

    # Same table name, same view name, different data in each instance.
    for conn, base in ((a, 100), (b, 5000)):
        conn.execute("CREATE TABLE li (k INTEGER, v DOUBLE)")
        conn.execute(f"INSERT INTO li SELECT i % 4, i + {base} FROM range(8) t(i)")
        conn.execute("CREATE MATERIALIZED VIEW li_sum AS SELECT k, SUM(v) AS s FROM li GROUP BY k")

    def totals(conn):
        return dict(conn.execute("SELECT k, s FROM dbsp_query('li_sum') ORDER BY k").fetchall())

    def expected(conn):
        return dict(conn.execute("SELECT k, SUM(v) FROM li GROUP BY k ORDER BY k").fetchall())

    assert totals(a) == expected(a), f"instance a wrong: {totals(a)} != {expected(a)}"
    assert totals(b) == expected(b), f"instance b wrong: {totals(b)} != {expected(b)}"
    assert totals(a) != totals(b), "instances should differ (different data)"

    # Interleaved edits must propagate only within their own instance.
    a.execute("INSERT INTO li VALUES (0, 1.5)")
    b.execute("INSERT INTO li VALUES (1, 2.5)")
    assert totals(a) == expected(a), f"a after edit: {totals(a)} != {expected(a)}"
    assert totals(b) == expected(b), f"b after edit: {totals(b)} != {expected(b)}"

    # Closing one instance must not disturb the other.
    a.close()
    b.execute("INSERT INTO li VALUES (2, 9.0)")
    assert totals(b) == expected(b), f"b after a closed: {totals(b)} != {expected(b)}"
    b.close()

    # Both paths must reopen without hanging (per-instance teardown ran).
    a2 = open_db(os.path.join(tmp, "a.duckdb"))
    assert a2.execute("SELECT COUNT(*) FROM li").fetchone()[0] == 9
    a2.close()
    b2 = open_db(os.path.join(tmp, "b.duckdb"))
    assert b2.execute("SELECT COUNT(*) FROM li").fetchone()[0] == 10
    b2.close()

signal.alarm(0)
print("PASS: two instances isolated; close/reopen per instance works", flush=True)

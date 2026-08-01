"""Regression test: dbsp_mv_tables(true) mirrors every MV's result into a
__mv_<view> table in the same database, kept in sync per commit.

Pins: enable-time backfill, create-while-enabled, post-edit/post-delete
parity across view shapes (aggregate, join, window, recursive, view-on-view,
duplicate-row results exercising the full-rewrite fallback), durability
across reopen, and that disabling stops mirroring.

Run: python test_mv_tables.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import sys
import tempfile

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 180


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)

DB = tempfile.mktemp(suffix=".duckdb")


def connect():
    c = duckdb.connect(DB, config={"allow_unsigned_extensions": "true"})
    c.execute(f"LOAD '{EXT}'")
    return c


conn = connect()
conn.execute("CREATE TABLE t (k INTEGER, g INTEGER, v DOUBLE)")
conn.execute("INSERT INTO t SELECT range, range % 5, range * 1.0 FROM range(1000)")
conn.execute("CREATE TABLE d (g INTEGER, label VARCHAR)")
conn.execute("INSERT INTO d SELECT range, 'g' || range FROM range(5)")

VIEWS = {
    "mv_agg": "SELECT g, SUM(v) AS s FROM t GROUP BY g",
    "mv_join": "SELECT t.k, d.label, t.v FROM t JOIN d ON d.g = t.g",
    "mv_win": (
        "SELECT k, SUM(v) OVER (PARTITION BY g ORDER BY k "
        "ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) AS r FROM t"
    ),
    "mv_rec": (
        "WITH RECURSIVE r AS (SELECT g AS n FROM d WHERE g = 0 "
        "UNION ALL SELECT n + 1 FROM r WHERE n < 20) SELECT n FROM r"
    ),
    "mv_chain": "SELECT g, s * 2 AS s2 FROM mv_agg",
    "mv_dups": "SELECT g FROM t",  # weight-per-row > 1: full-rewrite fallback
}
for name, body in VIEWS.items():
    conn.execute(f"CREATE MATERIALIZED VIEW {name} AS {body}")

assert conn.execute("SELECT * FROM dbsp_mv_tables(true)").fetchall()


def check_parity(c, msg):
    for name in VIEWS:
        mv = sorted(map(tuple, c.execute(f"SELECT * FROM dbsp_query('{name}')").fetchall()))
        tbl = sorted(map(tuple, c.execute(f'SELECT * FROM "__mv_{name}"').fetchall()))
        assert mv == tbl, f"{msg}: {name} diverged (mv {len(mv)} rows, table {len(tbl)})"


check_parity(conn, "after enable backfill")

conn.execute("UPDATE t SET v = v + 100.0 WHERE k = 7")
check_parity(conn, "after update")

conn.execute("DELETE FROM t WHERE k IN (3, 8, 13)")
check_parity(conn, "after delete")

conn.execute("INSERT INTO t VALUES (5000, 2, 9.5)")
check_parity(conn, "after insert")

# Create while enabled → backing table appears immediately.
conn.execute("CREATE MATERIALIZED VIEW mv_late AS SELECT g, COUNT(*) AS n FROM t GROUP BY g")
VIEWS["mv_late"] = ""
check_parity(conn, "after late create")

# Meta watermarks exist for every mirrored view.
meta = dict(conn.execute("SELECT view_name, commit_seq FROM __dbsp_mv_meta").fetchall())
assert set(meta) >= set(VIEWS), f"meta missing views: {set(VIEWS) - set(meta)}"

# Durability: backing tables are plain tables — they survive reopen and are
# readable WITHOUT any DBSP state.
conn.close()
plain = duckdb.connect(DB, config={"allow_unsigned_extensions": "true"})
rows = plain.execute('SELECT count(*) FROM "__mv_mv_agg"').fetchone()[0]
assert rows == 5, f"backing table lost across reopen: {rows}"
plain.close()

# Disable stops mirroring (table goes stale, loudly different).
conn = connect()
assert conn.execute("SELECT * FROM dbsp_mv_tables(false)").fetchall()
conn.execute("UPDATE t SET v = v + 777.0 WHERE k = 1")
stale = sorted(map(tuple, conn.execute('SELECT * FROM "__mv_mv_agg"').fetchall()))
live = sorted(map(tuple, conn.execute("SELECT * FROM dbsp_query('mv_agg')").fetchall()))
assert stale != live, "disable must stop mirroring"

print("PASS")

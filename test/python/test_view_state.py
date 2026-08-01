"""Regression test: dbsp_view_state() attributes resident circuit-state bytes
to the right classes (result / arrangement / window / recursion / other) and
emits the __shared_arrangements synthetic row.

Estimates are approximate by design — this test pins ATTRIBUTION (the right
class is nonzero for the right view shape) and monotonicity, not exact bytes.

Run: python test_view_state.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import sys

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 120


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)

conn = duckdb.connect(config={"allow_unsigned_extensions": "true"})
conn.execute(f"LOAD '{EXT}'")
conn.execute("CREATE TABLE t (k INTEGER, g INTEGER, v DOUBLE)")
conn.execute("INSERT INTO t SELECT range, range % 10, range * 1.0 FROM range(5000)")
conn.execute("CREATE TABLE d (g INTEGER, label VARCHAR)")
conn.execute("INSERT INTO d SELECT range, 'g' || range FROM range(10)")
conn.execute(
    "CREATE MATERIALIZED VIEW mv_join AS "
    "SELECT t.k, d.label, t.v FROM t JOIN d ON d.g = t.g"
)
conn.execute(
    "CREATE MATERIALIZED VIEW mv_win AS SELECT k, SUM(v) OVER "
    "(PARTITION BY g ORDER BY k ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) AS r FROM t"
)
conn.execute(
    "CREATE MATERIALIZED VIEW mv_rec AS WITH RECURSIVE r AS ("
    "SELECT g AS n FROM d WHERE g = 0 UNION ALL SELECT n + 1 FROM r WHERE n < 50"
    ") SELECT n FROM r"
)
for v in ("mv_join", "mv_win", "mv_rec"):
    conn.execute(f"SELECT count(*) FROM dbsp_query('{v}')").fetchone()

rows = {
    r[0]: dict(zip(("result", "arrangement", "window", "recursion", "other", "total"), r[1:]))
    for r in conn.execute("SELECT * FROM dbsp_view_state()").fetchall()
}

assert set(rows) >= {"mv_join", "mv_win", "mv_rec", "__shared_arrangements"}, rows.keys()
assert rows["mv_join"]["result"] > 0, rows["mv_join"]
assert rows["mv_win"]["window"] > 0, "window cache bytes must land in the window class"
assert rows["mv_rec"]["recursion"] > 0, "recursive state must land in the recursion class"
assert rows["__shared_arrangements"]["arrangement"] > 0, (
    "shared join arrangements must be reported"
)
for name, b in rows.items():
    assert b["total"] == sum(v for k, v in b.items() if k != "total"), (name, b)

# Growing the data grows the accounted state.
before = rows["mv_join"]["total"]
conn.execute("INSERT INTO t SELECT 10000 + range, range % 10, 1.0 FROM range(5000)")
conn.execute("SELECT count(*) FROM dbsp_query('mv_join')").fetchone()
after = {
    r[0]: r[6] for r in conn.execute("SELECT * FROM dbsp_view_state()").fetchall()
}["mv_join"]
assert after > before, f"state bytes must grow with data: {before} -> {after}"

print("PASS")

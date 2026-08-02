"""Regression test: per-table checkpoint validity (O(delta) recovery, inc A).

A crash that loses an edit to ONE table must no longer discard the whole
checkpoint: only views whose source closure touches the stale table
cold-replay; every other view restores from the checkpoint. Pins the
stale-closure walk (direct dependent + view-on-view transitive), the
restored views' parity, and post-recovery edit correctness on both sides.

Run: python test_partial_ckpt.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import subprocess
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


def close_clean(c):
    c.execute("SELECT * FROM dbsp_save()")
    c.close()
    d = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    d.execute(f"LOAD '{EXT}'")
    d.execute("SELECT * FROM dbsp_wait_teardown()")
    d.close()


conn = connect()
conn.execute("CREATE TABLE t1 (k INTEGER, v DOUBLE)")
conn.execute("INSERT INTO t1 SELECT range, range * 1.0 FROM range(50000)")
conn.execute("CREATE TABLE t2 (k INTEGER, v DOUBLE)")
conn.execute("INSERT INTO t2 SELECT range, range * 2.0 FROM range(50000)")
conn.execute("SELECT * FROM dbsp_mv_tables(true)")
conn.execute("CREATE MATERIALIZED VIEW v1 AS SELECT k % 10 AS g, SUM(v) AS s FROM t1 GROUP BY k % 10")
conn.execute("CREATE MATERIALIZED VIEW v2 AS SELECT k % 10 AS g, SUM(v) AS s FROM t2 GROUP BY k % 10")
conn.execute("CREATE MATERIALIZED VIEW v21 AS SELECT SUM(s) AS total FROM v2")
close_clean(conn)

# Crash child: edit ONLY t2, die without saving. t1's watermark stays
# clean; t2's goes stale.
child = f"""
import duckdb, os
c = duckdb.connect({DB!r}, config={{"allow_unsigned_extensions": "true"}})
c.execute("LOAD {EXT!r}")
c.execute("SELECT count(*) FROM dbsp_views()")
c.execute("UPDATE t2 SET v = v + 50.0 WHERE k = 1")
os._exit(9)
"""
r = subprocess.run([sys.executable, "-c", child])
assert r.returncode == 9

# Reopen with sync debug: the decline lines say who replayed.
probe = f"""
import duckdb, sys
c = duckdb.connect({DB!r}, config={{"allow_unsigned_extensions": "true"}})
c.execute("LOAD {EXT!r}")
c.execute("SELECT count(*) FROM dbsp_views()")
for view, table, expr in (
    ("v1", "t1", "SELECT k % 10 AS g, SUM(v) FROM t1 GROUP BY k % 10"),
    ("v2", "t2", "SELECT k % 10 AS g, SUM(v) FROM t2 GROUP BY k % 10"),
):
    got = sorted(c.execute(f"SELECT * FROM dbsp_query('{{view}}')").fetchall())
    want = sorted(c.execute(expr).fetchall())
    assert len(got) == len(want), (view, len(got), len(want))
    for (gg, gs), (wg, ws) in zip(got, want, strict=True):
        assert gg == wg and abs(gs - ws) < 1e-6 * max(abs(ws), 1.0), (view, gg, gs, ws)
got = c.execute("SELECT * FROM dbsp_query('v21')").fetchone()[0]
want = c.execute("SELECT SUM(v) FROM t2").fetchone()[0]
assert abs(got - want) < 1e-6 * abs(want), (got, want)
# post-recovery edits on BOTH sides stay correct (dup-row regression pin)
c.execute("UPDATE t1 SET v = v + 1.0 WHERE k = 2")
c.execute("UPDATE t2 SET v = v + 1.0 WHERE k = 2")
for view, expr, col in (
    ("v1", "SELECT k % 10 AS g, SUM(v) FROM t1 GROUP BY k % 10", None),
    ("v2", "SELECT k % 10 AS g, SUM(v) FROM t2 GROUP BY k % 10", None),
):
    got = sorted(c.execute(f"SELECT * FROM dbsp_query('{{view}}')").fetchall())
    want = sorted(c.execute(expr).fetchall())
    assert len(got) == len(want) == 10, (view, len(got))
    for (gg, gs), (wg, ws) in zip(got, want, strict=True):
        assert gg == wg and abs(gs - ws) < 1e-6 * max(abs(ws), 1.0), (view, gg, gs, ws)
c.execute("SELECT * FROM dbsp_save()")
c.close()
d = duckdb.connect(config={{"allow_unsigned_extensions": "true"}})
d.execute("LOAD {EXT!r}")
d.execute("SELECT * FROM dbsp_wait_teardown()")
d.close()
print("PROBE-OK", flush=True)
"""
env = dict(os.environ, DBSP_DEBUG_SYNC="1")
r = subprocess.run(
    [sys.executable, "-c", probe], capture_output=True, text=True, env=env
)
assert r.returncode == 0 and "PROBE-OK" in r.stdout, (
    f"rc={r.returncode}\nstdout={r.stdout}\nstderr={r.stderr}"
)
declined = [ln for ln in r.stderr.splitlines() if "checkpoint declined" in ln]
declined_names = {ln.split("'")[1] for ln in declined if "'" in ln}
# t2's closure replays; v1 must NOT be declined (it keeps its checkpoint).
assert "v2" in declined_names and "v21" in declined_names, (
    f"stale closure not replayed: {declined_names}"
)
assert "v1" not in declined_names, (
    f"clean view lost its checkpoint: {declined_names}"
)

os.remove(DB)
print("PASS")

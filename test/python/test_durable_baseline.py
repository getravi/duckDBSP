"""Regression test: durable spilled baselines (recovery inc B, first half).

For a file-backed database, spill files live in <dbfile>.dbsp_spill/ and
dbsp_save writes a digest-index sidecar under the checkpoint watermark. A
clean reopen's first table touch ADOPTS the log+index pair instead of
rescanning the table; any watermark/file-size mismatch (crash lost an
edit, out-of-band write) falls back to the ordinary rescan. Pins: sidecar
existence, adopt correctness (parity after adopt + subsequent edits),
and the crash-fallback path staying correct.

Run: python test_durable_baseline.py <path-to-dbsp.duckdb_extension>
"""

import glob
import os
import shutil
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

CHILD = r"""
import duckdb, os, sys
EXT, DB, step = sys.argv[1], sys.argv[2], sys.argv[3]
c = duckdb.connect(DB, config={"allow_unsigned_extensions": "true"})
c.execute(f"LOAD '{EXT}'")
if step == "create":
    c.execute("CREATE TABLE big (k INTEGER, v DOUBLE)")
    c.execute("INSERT INTO big SELECT range, range * 1.0 FROM range(200000)")
    c.execute("SELECT * FROM dbsp_mv_tables(true)")
    c.execute("CREATE MATERIALIZED VIEW mv AS SELECT k % 7 AS g, SUM(v) AS s FROM big GROUP BY k % 7")
else:
    c.execute("SELECT count(*) FROM dbsp_views()")
    c.execute("UPDATE big SET v = v + 3.0 WHERE k = 11")  # first touch adopts or rescans
    got = sorted(c.execute("SELECT * FROM dbsp_query('mv')").fetchall())
    want = sorted(c.execute("SELECT k % 7 AS g, SUM(v) FROM big GROUP BY k % 7").fetchall())
    assert len(got) == len(want) == 7, (len(got), len(want))
    for (gg, gs), (wg, ws) in zip(got, want, strict=True):
        assert gg == wg and abs(gs - ws) < 1e-6 * max(abs(ws), 1.0), (gg, gs, ws)
    mode = c.execute("SELECT mode FROM dbsp_table_state()").fetchone()[0]
    assert mode == "spilled", mode
    if step == "reopen-crash":
        os._exit(9)
c.execute("SELECT * FROM dbsp_save()")
c.close()
d = duckdb.connect(config={"allow_unsigned_extensions": "true"})
d.execute(f"LOAD '{EXT}'")
d.execute("SELECT * FROM dbsp_wait_teardown()")
d.close()
print("CHILD-OK", flush=True)
"""


def run(step, env_extra=None, expect_rc=0, expect_ok=True):
    env = dict(os.environ, DBSP_SPILL_THRESHOLD_ROWS="50000")
    if env_extra:
        env.update(env_extra)
    r = subprocess.run(
        [sys.executable, "-c", CHILD, EXT, DB, step],
        capture_output=True,
        text=True,
        env=env,
    )
    assert r.returncode == expect_rc, (
        f"{step}: rc={r.returncode}\nstdout={r.stdout}\nstderr={r.stderr}"
    )
    if expect_ok:
        assert "CHILD-OK" in r.stdout, f"{step}: {r.stdout}\n{r.stderr}"
    return r


# 1. Create above threshold in a file-backed DB → durable spill dir +
#    index sidecar after dbsp_save.
run("create")
spill_dir = DB + ".dbsp_spill"
logs = glob.glob(spill_dir + "/*.dbspill")
idxs = glob.glob(spill_dir + "/*.dbspill.idx")
assert logs and idxs, f"durable spill files missing: {os.listdir(spill_dir) if os.path.isdir(spill_dir) else 'no dir'}"

# 2. Clean reopen: first edit adopts the pair (mode spilled, parity holds
#    through the edit). Then it crashes WITHOUT saving.
run("reopen-crash", expect_rc=9, expect_ok=False)

# 3. Post-crash reopen: the table changed since the last save, so the
#    stale index must be REJECTED and the rescan path must produce
#    correct results again.
run("reopen", expect_rc=0)

# 4. And one more clean reopen adopts the fresh sidecar written by step
#    3's dbsp_save.
run("reopen", expect_rc=0)

shutil.rmtree(spill_dir, ignore_errors=True)
os.remove(DB)
print("PASS")

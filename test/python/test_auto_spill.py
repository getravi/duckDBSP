"""Regression test: baselines auto-spill above DBSP_SPILL_THRESHOLD_ROWS.

Bounded-RAM Phase 5: boxed baselines (~300 B/row) OOM big-model builds;
above the row threshold a baseline must migrate itself to the digest form
mid-scan, with no dbsp_spill(true) and no correctness change. Pins:
threshold crossing during initial scan, small tables staying boxed,
mixed-mode edits + parity, threshold=0 disabling auto-spill.

Run: python test_auto_spill.py <path-to-dbsp.duckdb_extension>
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

# Threshold is read once per process (static) — run each scenario in a
# child so the env var actually applies.
CHILD = r"""
import duckdb, os, sys
EXT, DB, mode = sys.argv[1], sys.argv[2], sys.argv[3]
c = duckdb.connect(DB, config={"allow_unsigned_extensions": "true"})
c.execute(f"LOAD '{EXT}'")
c.execute("CREATE TABLE big (k INTEGER, v DOUBLE)")
c.execute("INSERT INTO big SELECT range, range * 1.0 FROM range(100000)")
c.execute("CREATE TABLE small (k INTEGER, v DOUBLE)")
c.execute("INSERT INTO small SELECT range, range * 1.0 FROM range(100)")
c.execute("CREATE MATERIALIZED VIEW mv_big AS SELECT SUM(v) AS s FROM big")
c.execute("CREATE MATERIALIZED VIEW mv_small AS SELECT SUM(v) AS s FROM small")
st = {r[0].split(".")[-1]: r[1] for r in c.execute("SELECT * FROM dbsp_table_state()").fetchall()}
if mode == "auto":
    assert st["big"] == "spilled", f"big must auto-spill: {st}"
    assert st["small"] == "boxed", f"small must stay boxed: {st}"
else:
    assert st["big"] == "boxed", f"threshold=0 must disable auto-spill: {st}"
    assert st["small"] == "boxed", f"small must stay boxed: {st}"
# mixed-mode edits + parity
c.execute("UPDATE big SET v = v + 5 WHERE k = 3")
c.execute("UPDATE small SET v = v + 5 WHERE k = 3")
got_b = c.execute("SELECT * FROM dbsp_query('mv_big')").fetchone()[0]
want_b = c.execute("SELECT SUM(v) FROM big").fetchone()[0]
assert abs(got_b - want_b) < 1e-6, (got_b, want_b)
got_s = c.execute("SELECT * FROM dbsp_query('mv_small')").fetchone()[0]
want_s = c.execute("SELECT SUM(v) FROM small").fetchone()[0]
assert abs(got_s - want_s) < 1e-6, (got_s, want_s)
c.execute("SELECT * FROM dbsp_save()")
c.close()
d = duckdb.connect(config={"allow_unsigned_extensions": "true"})
d.execute(f"LOAD '{EXT}'")
d.execute("SELECT * FROM dbsp_wait_teardown()")
d.close()
print("CHILD-OK", flush=True)
"""

for mode, threshold in (("auto", "50000"), ("off", "0")):
    db = tempfile.mktemp(suffix=".duckdb")
    env = dict(os.environ, DBSP_SPILL_THRESHOLD_ROWS=threshold)
    r = subprocess.run(
        [sys.executable, "-c", CHILD, EXT, db, mode],
        capture_output=True,
        text=True,
        env=env,
    )
    assert r.returncode == 0 and "CHILD-OK" in r.stdout, (
        f"{mode}: rc={r.returncode}\nstdout={r.stdout}\nstderr={r.stderr}"
    )
    os.remove(db)

print("PASS")

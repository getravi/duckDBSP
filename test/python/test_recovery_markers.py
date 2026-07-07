"""Recovery-marker hygiene.

Three contracts (each in its own subprocess — recovery runs once per process):
1. In-memory instances create NO recovery dir (state dies with the process;
   there is nothing to recover) — previously they littered ./.dbsp_recovery
   into the process CWD (e.g. the API server's repo root).
2. File-backed: markers live next to the db file, and a clean close removes
   the session lock so the next open does NOT claim "previous session
   crashed" (previously the lock only died in a global static destructor
   that embedders' teardown never runs).
3. A real crash (SIGKILL) leaves the lock; the next open detects it, logs
   it, and clears the marker.

Run: python test_recovery_markers.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import subprocess
import sys
import tempfile

EXT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension")
TIMEOUT_S = 120


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)


def run_child(code: str, cwd: str, expect_kill: bool = False) -> None:
    proc = subprocess.run([sys.executable, "-c", code], cwd=cwd, capture_output=True, text=True)
    if expect_kill:
        assert proc.returncode != 0, "child was expected to die"
    elif proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr)
        raise AssertionError("child failed")


PRELUDE = f"""
import duckdb, os, signal
def conn_for(path=None):
    c = duckdb.connect(path or ":memory:", config={{"allow_unsigned_extensions": "true"}})
    c.execute("LOAD '{EXT}'")
    return c
"""

with tempfile.TemporaryDirectory() as tmp:
    workdir = os.path.join(tmp, "cwd")
    os.makedirs(workdir)
    db = os.path.join(tmp, "m.duckdb")
    rec_dir = os.path.join(tmp, ".dbsp_recovery")
    lock = os.path.join(rec_dir, ".dbsp.lock")
    crash_log = os.path.join(rec_dir, ".dbsp.crash")

    # ── 1. memory instance: no recovery dir in CWD ──────────────────────
    run_child(
        PRELUDE
        + """
c = conn_for()
c.execute("CREATE TABLE t (a INTEGER)")
c.execute("INSERT INTO t VALUES (1)")
c.execute("CREATE MATERIALIZED VIEW s AS SELECT SUM(a) AS s FROM t")
assert c.execute("SELECT s FROM dbsp_query('s')").fetchone() == (1,)
c.close()
""",
        cwd=workdir,
    )
    assert not os.path.exists(os.path.join(workdir, ".dbsp_recovery")), (
        "memory instance littered .dbsp_recovery into the CWD"
    )
    print("PASS: memory instance leaves no recovery dir")

    # ── 2. file-backed: lock lives next to db, clean close removes it ───
    run_child(
        PRELUDE
        + f"""
c = conn_for({db!r})
c.execute("CREATE TABLE t (a INTEGER)")
c.execute("INSERT INTO t VALUES (2)")
c.execute("CREATE MATERIALIZED VIEW s2 AS SELECT SUM(a) AS s FROM t")
c.execute("SELECT s FROM dbsp_query('s2')").fetchone()
assert os.path.exists({lock!r}), "session lock not created next to the db file"
c.close()
assert not os.path.exists({lock!r}), (
    "clean close left the session lock - next boot would claim a crash"
)
""",
        cwd=workdir,
    )
    assert not os.path.exists(os.path.join(workdir, ".dbsp_recovery")), (
        "file-backed markers must not land in the CWD"
    )
    print("PASS: file-backed lock lives next to db; clean close removes it")

    # ── 3. real crash still detected ─────────────────────────────────────
    run_child(
        PRELUDE
        + f"""
c = conn_for({db!r})
# recovery runs via dbsp's internal connections — touch the machinery
c.execute("CREATE MATERIALIZED VIEW crash_probe AS SELECT COUNT(*) AS n FROM t")
c.execute("SELECT n FROM dbsp_query('crash_probe')").fetchone()  # triggers recovery
assert os.path.exists({lock!r}), "lock missing before kill"
os.kill(os.getpid(), signal.SIGKILL)
""",
        cwd=workdir,
        expect_kill=True,
    )
    assert os.path.exists(lock), "SIGKILL'd session should leave the lock behind"
    run_child(
        PRELUDE
        + f"""
c = conn_for({db!r})
c.execute("CREATE MATERIALIZED VIEW crash_probe2 AS SELECT COUNT(*) AS n FROM t")
c.execute("SELECT n FROM dbsp_query('crash_probe2')").fetchone()
# recovery replaces the stale lock with THIS session's lock
assert os.path.exists({lock!r}), "recovered session should hold its own lock"
assert os.path.exists({crash_log!r}), "crash should be logged"
c.close()
assert not os.path.exists({lock!r}), "clean close after recovery should release the lock"
""",
        cwd=workdir,
    )
    print("PASS: real crash detected, logged, lock cleared")

print("ALL PASS")

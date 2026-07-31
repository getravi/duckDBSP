"""Regression test: dbsp_autopersist (Feature 1 of durability-ergonomics).

Before this feature, DBSP view state died on a clean connection reopen
unless a caller explicitly ran dbsp_save() before close() and dbsp_load()
after reopen — nobody in practice made both calls (see test_reopen_hang.py's
"DBSP state is in-memory: views from session 1 are gone by design").

dbsp_autopersist(enable) is ON by default:
  - Auto-save: a clean connection close (last user connection to the
    instance) saves view definitions (_dbsp_views) and a circuit-state
    checkpoint (_dbsp_ckpt) when autopersist is on and any view exists.
  - Auto-load: the first DBSP entry point with a ClientContext in a fresh
    session (dbsp_query, dbsp_views, dbsp_track, CREATE MATERIALIZED VIEW,
    dbsp_sync, dbsp_changes) loads persisted state if the registry is
    still empty — no explicit dbsp_load() call needed.

Proven here:
  1. views survive a same-process clean close + reopen with no explicit
     save/load calls, and stay incrementally correct afterward,
  2. dbsp_autopersist(false) is honored: nothing is saved on close, so a
     reopen finds no views (and no auto-load fires).

Run: python test_autopersist.py <path-to-dbsp.duckdb_extension>
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


def open_db(path):
    conn = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    conn.execute(f"LOAD '{EXT}'")
    return conn


with tempfile.TemporaryDirectory() as tmp:
    # ── 1. views survive a clean close + reopen, no explicit save/load ──
    path = os.path.join(tmp, "ap.duckdb")

    conn = open_db(path)
    conn.execute("CREATE TABLE t (k INTEGER, v DOUBLE)")
    conn.execute("INSERT INTO t VALUES (1, 10.0), (2, 20.0)")
    conn.execute("CREATE MATERIALIZED VIEW mv AS SELECT k, v * 2 AS d FROM t")
    got = sorted(conn.execute("SELECT * FROM dbsp_query('mv')").fetchall())
    assert got == [(1, 20.0), (2, 40.0)], f"initial view wrong: {got}"
    conn.close()  # clean close -> auto-save fires

    conn2 = open_db(path)
    # No dbsp_load() call — the first DBSP entry point auto-loads.
    got = sorted(conn2.execute("SELECT * FROM dbsp_query('mv')").fetchall())
    assert got == [(1, 20.0), (2, 40.0)], f"view did not survive reopen: {got}"

    # And it stays incremental after the reopen (not just a one-shot rebuild).
    conn2.execute("INSERT INTO t VALUES (3, 30.0)")
    got = sorted(conn2.execute("SELECT * FROM dbsp_query('mv')").fetchall())
    assert got == [(1, 20.0), (2, 40.0), (3, 60.0)], (
        f"post-reopen incremental edit diverged: {got}"
    )
    conn2.close()
    print("PASS: views survive a clean close + reopen and stay incremental")

    # ── 2. dbsp_autopersist(false) is honored: nothing saved, nothing loaded ──
    off_path = os.path.join(tmp, "off.duckdb")

    conn = open_db(off_path)
    conn.execute("SELECT * FROM dbsp_autopersist(false)")
    conn.execute("CREATE TABLE t (k INTEGER)")
    conn.execute("CREATE MATERIALIZED VIEW mv AS SELECT k FROM t")
    conn.close()

    conn2 = open_db(off_path)
    res = conn2.execute("SELECT * FROM dbsp_views()").fetchall()
    assert res == [], f"autopersist(false) should save nothing: {res}"
    conn2.close()
    print("PASS: dbsp_autopersist(false) is honored (nothing saved or loaded)")

    # ── 3. dbsp_load() on an already-populated registry is truthful ──
    # conn3 auto-loads mv from the checkpoint conn2's clean close saved (same
    # "first DBSP entry point" trigger as step 1). A subsequent EXPLICIT
    # dbsp_load() call has nothing left to do -- every row in _dbsp_views
    # already names a live view -- and must say so, not claim "0 from
    # checkpoint" (which reads as "the checkpoint was never used", when in
    # fact the auto-load moments earlier DID consume it).
    conn3 = open_db(path)
    got = sorted(conn3.execute("SELECT * FROM dbsp_query('mv')").fetchall())
    assert got == [(1, 20.0), (2, 40.0), (3, 60.0)], (
        f"pre-explicit-load state wrong: {got}"
    )

    msg = conn3.execute("SELECT * FROM dbsp_load()").fetchone()[0]
    assert "0 from checkpoint" not in msg, (
        f"dbsp_load() on an already-loaded registry must not claim the "
        f"checkpoint went unused: {msg!r}"
    )
    assert "already loaded" in msg, (
        f"dbsp_load() on an already-loaded registry must say so plainly: "
        f"{msg!r}"
    )
    conn3.close()
    print("PASS: dbsp_load() on an already-loaded registry is truthful")

signal.alarm(0)
print("PASS: autopersist saves on clean close and auto-loads on reopen", flush=True)

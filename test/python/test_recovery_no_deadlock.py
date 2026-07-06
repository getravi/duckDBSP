"""Regression test: opening a second connection to an instance that already
has the extension loaded must not deadlock.

Crash recovery used to run inside OnConnectionOpened, which executes under
ConnectionManager::connections_lock; recovery opens internal Connections
whose constructors re-enter AddConnection on the same mutex — self-deadlock.
The first connection per instance never hit it (the callback registers at
LOAD, after that connection's AddConnection already ran), so single-
connection scripts passed while embedded hosts opening a connection per
operation hung on connection #2. Recovery now runs at first QueryBegin.

Run: python test_recovery_no_deadlock.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import sys
import tempfile

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 30


def on_alarm(signum, frame):
    print("FAIL: timed out (recovery deadlock on second connection)", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)

with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "model.duckdb")

    # Connection 1: loads the extension (callback registers AFTER this
    # connection's AddConnection, so it never triggered the bug).
    c1 = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    c1.execute(f"LOAD '{EXT}'")
    c1.execute("CREATE TABLE li (a INTEGER, v DOUBLE)")
    c1.execute("INSERT INTO li VALUES (1, 2.0)")

    # Connection 2 to the same live instance: AddConnection now fires the
    # DBSP callback. Before the fix this deadlocked inside recovery.
    c2 = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    row = c2.execute("SELECT COUNT(*) FROM li").fetchone()
    assert row[0] == 1, f"expected 1 row, got {row[0]}"

    # Recovery (now at first QueryBegin) must leave the engine functional.
    c2.execute("CREATE MATERIALIZED VIEW li_sum AS SELECT a, SUM(v) AS s FROM li GROUP BY a")
    c2.execute("INSERT INTO li VALUES (1, 3.0)")
    got = c2.execute("SELECT s FROM dbsp_query('li_sum')").fetchone()
    assert got[0] == 5.0, f"expected 5.0, got {got[0]}"

    c2.close()
    c1.close()

signal.alarm(0)
print("PASS: second connection does not deadlock; recovery deferred to QueryBegin", flush=True)

"""Regression test: same-process close + reopen of a DB that had DBSP views.

Before the fix, DBSP's materialized views held internal Connections in the
leaked CDCManager singleton, pinning the DatabaseInstance forever. DuckDB's
DBInstanceCache busy-spins waiting for the old instance to die, so the second
duckdb.connect() to the same path never returned.

Run: python test_reopen_hang.py <path-to-dbsp.duckdb_extension>
Exits 0 on pass; exits 1 (after killing itself) on hang/failure.
"""

import os
import signal
import sys
import tempfile

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 30


def die(msg: str) -> None:
    print(f"FAIL: {msg}", flush=True)
    os._exit(1)


def on_alarm(signum, frame):
    die(f"timed out after {TIMEOUT_S}s (reopen hang)")


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)

with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "model.duckdb")

    # Session 1: create table + materialized view, then close.
    conn = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    conn.execute(f"LOAD '{EXT}'")
    conn.execute("CREATE TABLE li (a INTEGER, v DOUBLE)")
    conn.execute("INSERT INTO li SELECT i % 10, i * 0.5 FROM range(1000) t(i)")
    conn.execute("CREATE MATERIALIZED VIEW li_sum AS SELECT a, SUM(v) AS s FROM li GROUP BY a")
    n = conn.execute("SELECT COUNT(*) FROM dbsp_query('li_sum')").fetchone()[0]
    assert n == 10, f"expected 10 groups, got {n}"
    conn.close()

    # Session 2 (same process, same path): this hung forever before the fix.
    conn = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    conn.execute(f"LOAD '{EXT}'")
    row = conn.execute("SELECT COUNT(*) FROM li").fetchone()
    assert row[0] == 1000, f"expected 1000 rows, got {row[0]}"

    # DBSP state is in-memory: views from session 1 are gone by design.
    # Creating a fresh view in session 2 must work.
    conn.execute("CREATE MATERIALIZED VIEW li_sum2 AS SELECT a, SUM(v) AS s FROM li GROUP BY a")
    n = conn.execute("SELECT COUNT(*) FROM dbsp_query('li_sum2')").fetchone()[0]
    assert n == 10, f"expected 10 groups in session 2, got {n}"
    conn.close()

    # Session 3: repeat once more to catch teardown-ordering regressions.
    conn = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    conn.execute(f"LOAD '{EXT}'")
    conn.execute("SELECT 1").fetchone()
    conn.close()

signal.alarm(0)
print("PASS: close + reopen with DBSP views does not hang", flush=True)

"""Regression test: crash recovery runs once per DATABASE, not once per process.

Three defects lived here, each hiding the next:

1. maybe_run_recovery latched on a process-wide atomic, so only the FIRST
   database opened ever ran recovery. Every later model got no crash marker
   and registered no session -- a server opening many models in one process
   protected exactly one of them.
2. determine_recovery_path returned the already-set recovery_path_ member,
   which recover_from_crash itself assigns. So the second database inherited
   the first one's directory: no marker of its own, and the first one's LIVE
   lock read as a crash on a clean open.
3. The per-database "already recovered" set held raw DatabaseInstance
   pointers and was never pruned. DuckDB reuses freed addresses, so a new
   database landing on a dead one's address skipped recovery entirely.

Case D is the one that catches #3, and it only catches it because cases A-C
open and close databases FIRST. Do not reorder the cases.

Run: python test_recovery_per_database.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import sys
import tempfile

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 120


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)

failures = []


def check(name: str, actual, expected) -> None:
    if actual == expected:
        print(f"PASS {name}", flush=True)
    else:
        print(f"FAIL {name}: got {actual!r}, want {expected!r}", flush=True)
        failures.append(name)


def open_db(path: str) -> duckdb.DuckDBPyConnection:
    conn = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    conn.execute(f"LOAD '{EXT}'")
    return conn


def build(dir_path: str) -> duckdb.DuckDBPyConnection:
    """A database with one tracked table and one materialized view."""
    conn = open_db(os.path.join(dir_path, "m.duckdb"))
    conn.execute("CREATE TABLE t (a INT)")
    conn.execute("INSERT INTO t SELECT i FROM range(500) s(i)")
    conn.execute("CREATE MATERIALIZED VIEW v AS SELECT a*2 AS b FROM t")
    conn.sql("SELECT * FROM dbsp_mv_tables(true)").fetchone()
    conn.sql("SELECT * FROM dbsp_save()").fetchone()
    return conn


def marked(dir_path: str) -> bool:
    """Whether this database currently holds a crash marker."""
    return os.path.exists(os.path.join(dir_path, ".dbsp_recovery"))


with tempfile.TemporaryDirectory() as tmp:
    # A-C: three databases open at once, each with its own marker, each
    # released only by its own close. Under defect #1 only the first was
    # ever marked; under #2 the second wrote into the first's directory.
    dirs = []
    for name in ("one", "two", "three"):
        d = os.path.join(tmp, name)
        os.makedirs(d)
        dirs.append(d)
    conns = [build(d) for d in dirs]

    check("A three databases each marked", [marked(d) for d in dirs], [True] * 3)

    conns[0].close()
    check("B closing one spares the others", [marked(d) for d in dirs], [False, True, True])

    conns[1].close()
    conns[2].close()
    check("C every close releases its own", [marked(d) for d in dirs], [False] * 3)

    # D: a database opened AFTER those closures may land on a freed
    # DatabaseInstance address. It must still recover and mark.
    d4 = os.path.join(tmp, "four")
    os.makedirs(d4)
    a = build(d4)
    check("D database opened after closures is marked", marked(d4), True)

    # E-F: the marker is refcounted, so it survives until the LAST connection
    # on that database closes -- otherwise a crash after the first close
    # looks like a clean exit and the next open skips recovery.
    b = open_db(os.path.join(d4, "m.duckdb"))
    b.sql("SELECT count(*) FROM dbsp_views()").fetchone()
    a.close()
    check("E marker survives while a second connection is open", marked(d4), True)
    b.close()
    check("F last connection releases the marker", marked(d4), False)

    # G: a stale lock on a LATE-opened database is still detected as a crash
    # and its views still come back. Under defect #1 this database never ran
    # recovery at all, so nothing was restored.
    d5 = os.path.join(tmp, "five")
    os.makedirs(d5)
    build(d5).close()
    os.makedirs(os.path.join(d5, ".dbsp_recovery"), exist_ok=True)
    with open(os.path.join(d5, ".dbsp_recovery", ".dbsp.lock"), "w") as fh:
        fh.write("stale")
    c = open_db(os.path.join(d5, "m.duckdb"))
    check(
        "G late database recovers its views after a crash",
        c.sql("SELECT count(*) FROM dbsp_views()").fetchone()[0],
        1,
    )
    c.close()

if failures:
    print(f"FAILED: {', '.join(failures)}", flush=True)
    sys.exit(1)
print("OK", flush=True)

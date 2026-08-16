"""Differential test: the RowExprEval bound-reference fast path must be
value-for-value identical to the generic executor path.

RowExprEval::eval used to run every call through an ExpressionExecutor: a
DataChunk::Reset (a VectorCacheBuffer::ResetFromCache per column), a refill of
EVERY input column, and a fresh result Vector -- all to hand back one column
of the row it was given. Join pad reconciliation calls it once per affected
row per step, and it profiled as the hottest frame of a cold build.

A bare column reference now short-circuits all of that. The risk is that the
short-circuit disagrees with the executor on an edge: NULL keys, a key column
whose stored type differs from the referenced input type, or outer-join
padding driven by match counts. Each case below is built with the fast path ON
and OFF (DBSP_ROWEVAL_FASTPATH) and the results compared exactly.

Run: python test_roweval_fastpath.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import subprocess
import sys

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 300

# Each case: (name, setup statements, view SQL, probe SQL).
# Probes read the __mv_ backing table dbsp_mv_tables creates: a materialized
# view is not readable as a plain relation. Ordered so the comparison is on
# values, not row order.
CASES = [
    (
        "inner join on int key",
        [
            "CREATE TABLE l (k INT, v INT)",
            "CREATE TABLE r (k INT, w INT)",
            "INSERT INTO l SELECT i % 7, i FROM range(60) s(i)",
            "INSERT INTO r SELECT i % 5, i * 10 FROM range(40) s(i)",
        ],
        "SELECT l.k AS k, sum(l.v + r.w) AS t FROM l JOIN r ON l.k = r.k GROUP BY l.k",
        "SELECT * FROM \"__mv_v\" ORDER BY k",
    ),
    (
        # LEFT JOIN drives reconcile_pads/match_count -- the path the fast
        # path is actually hot in.
        "left join with unmatched rows (pads)",
        [
            "CREATE TABLE l (k INT, v INT)",
            "CREATE TABLE r (k INT, w INT)",
            "INSERT INTO l SELECT i, i FROM range(50) s(i)",
            "INSERT INTO r SELECT i * 3, i FROM range(10) s(i)",
        ],
        "SELECT l.k AS k, r.w AS w FROM l LEFT JOIN r ON l.k = r.k",
        "SELECT k, coalesce(w, -1) AS w FROM \"__mv_v\" ORDER BY k, w",
    ),
    (
        "null keys on both sides",
        [
            "CREATE TABLE l (k INT, v INT)",
            "CREATE TABLE r (k INT, w INT)",
            "INSERT INTO l VALUES (1, 1), (NULL, 2), (3, 3), (NULL, 4)",
            "INSERT INTO r VALUES (1, 10), (NULL, 20), (3, 30)",
        ],
        "SELECT l.k AS k, l.v AS v, r.w AS w FROM l LEFT JOIN r ON l.k = r.k",
        "SELECT coalesce(k, -1) AS k, v, coalesce(w, -1) AS w FROM \"__mv_v\" ORDER BY k, v, w",
    ),
    (
        # Key columns of DIFFERENT widths: the fast path must apply the same
        # input-type cast the generic chunk fill applied.
        "join across int types (cast on key)",
        [
            "CREATE TABLE l (k BIGINT, v INT)",
            "CREATE TABLE r (k SMALLINT, w INT)",
            "INSERT INTO l SELECT i % 9, i FROM range(45) s(i)",
            "INSERT INTO r SELECT (i % 6)::SMALLINT, i FROM range(30) s(i)",
        ],
        "SELECT l.k AS k, count(*) AS n FROM l LEFT JOIN r ON l.k = r.k GROUP BY l.k",
        "SELECT * FROM \"__mv_v\" ORDER BY k",
    ),
    (
        "varchar key",
        [
            "CREATE TABLE l (k VARCHAR, v INT)",
            "CREATE TABLE r (k VARCHAR, w INT)",
            "INSERT INTO l SELECT 'k' || (i % 8), i FROM range(40) s(i)",
            "INSERT INTO r SELECT 'k' || (i % 5), i FROM range(25) s(i)",
        ],
        "SELECT l.k AS k, sum(r.w) AS t FROM l LEFT JOIN r ON l.k = r.k GROUP BY l.k",
        "SELECT k, coalesce(t, -1) AS t FROM \"__mv_v\" ORDER BY k",
    ),
]

# Applied after the view is built, so the comparison covers incremental
# maintenance (reconcile_pads over deltas) and not just the initial replay.
EDITS = [
    "INSERT INTO l VALUES (2, 999)",
    "DELETE FROM r WHERE w % 7 = 0",
    "UPDATE l SET v = v + 1 WHERE v % 5 = 0",
]

# A file-backed database, not :memory: -- the extension's materialized views
# are not served from an in-memory catalog.
CHILD = """
import duckdb, sys, tempfile, os
setup, view_sql, edits, probe, ext = eval(sys.stdin.read())
tmp = tempfile.mkdtemp()
conn = duckdb.connect(os.path.join(tmp, "m.duckdb"),
                      config={"allow_unsigned_extensions": "true"})
conn.execute("LOAD '" + ext + "'")
for stmt in setup:
    conn.execute(stmt)
conn.execute("CREATE MATERIALIZED VIEW v AS " + view_sql)
conn.sql("SELECT * FROM dbsp_mv_tables(true)").fetchone()
for stmt in edits:
    conn.execute(stmt)
print(repr(conn.sql(probe).fetchall()))
"""


def run_case(setup, view_sql, probe, fastpath: bool) -> str:
    """Build the view in a fresh process, apply edits, return the probe rows."""
    env = dict(os.environ)
    env["DBSP_ROWEVAL_FASTPATH"] = "1" if fastpath else "0"
    out = subprocess.run(
        [sys.executable, "-c", CHILD],
        input=repr((setup, view_sql, EDITS, probe, EXT)),
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
    )
    if out.returncode != 0:
        tail = out.stderr.strip().splitlines()
        return "ERROR: " + (tail[-1] if tail else "no stderr")
    return out.stdout.strip()


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)

if os.environ.get("DBSP_ROWEVAL_FASTPATH") is not None:
    print("FAIL: DBSP_ROWEVAL_FASTPATH is set in the environment", flush=True)
    sys.exit(1)

failures = []
for name, setup, view_sql, probe in CASES:
    fast = run_case(setup, view_sql, probe, fastpath=True)
    slow = run_case(setup, view_sql, probe, fastpath=False)
    if fast.startswith("ERROR") or slow.startswith("ERROR"):
        print(f"FAIL {name}: fast={fast[:160]} slow={slow[:160]}", flush=True)
        failures.append(name)
    elif fast != slow:
        print(f"FAIL {name}: fast path diverged from the executor", flush=True)
        print(f"  fast: {fast[:300]}", flush=True)
        print(f"  slow: {slow[:300]}", flush=True)
        failures.append(name)
    else:
        print(f"PASS {name}", flush=True)

if failures:
    print(f"FAILED: {', '.join(failures)}", flush=True)
    sys.exit(1)
print("OK", flush=True)

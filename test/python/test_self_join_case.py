"""CASE + later column refs silently wrong (regression).

Found 2026-08-14 by NumPad's MV compiler as "CASE across a self-join
(same table LEFT-JOINed twice) is silently NULL/wrong". Root cause is NOT
self-join-specific: BatchEvaluator reused each expression's result vector
across execute() calls. DuckDB's CASE executor has all-one-branch fast
paths that make the result vector a REFERENCE to an input chunk column;
on a later batch with mixed branches, FillSwitch writes through that
stale pointer straight into the shared input chunk, corrupting the
branch's source column for every expression evaluated AFTER the CASE in
that batch. The self-join shape merely produced the required batch
sequence (an all-one-branch batch, then a mixed batch) during CREATE.

This test compares dbsp_query() output against stock DuckDB running the
same SELECT, covering the originally reported self-join shape plus the
staged-commit single-table and distinct-table shapes that trigger the
same mechanism. Silent divergence == FAIL.

Run: python test_self_join_case.py <path-to-dbsp.duckdb_extension>
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

SETUP = [
    "CREATE TABLE base (d0 INTEGER, v DOUBLE)",
    "INSERT INTO base VALUES (1, 10.0), (2, 20.0), (3, 30.0)",
    "CREATE TABLE tb (d0 INTEGER, s DOUBLE, e DOUBLE)",
    "INSERT INTO tb VALUES (1, 100.0, 1000.0), (2, -200.0, 2000.0)",
    "CREATE TABLE other (d0 INTEGER, s DOUBLE, e DOUBLE)",
    "INSERT INTO other VALUES (1, 100.0, 1000.0), (2, -200.0, 2000.0)",
]

# The defective shape: same table joined twice + CASE reading both aliases.
SELF_JOIN_CASE = (
    "SELECT base.d0, "
    "       CASE WHEN a1.s > 0 THEN a1.s ELSE a2.e END AS c, "
    "       a1.s AS p1, a2.e AS p2 "
    "FROM base "
    "LEFT JOIN tb a1 ON a1.d0 = base.d0 "
    "LEFT JOIN tb a2 ON a2.d0 = base.d0"
)

# Control 1: identical join tree, plain columns only (was always correct).
SELF_JOIN_PLAIN = (
    "SELECT base.d0, a1.s AS p1, a2.e AS p2 "
    "FROM base "
    "LEFT JOIN tb a1 ON a1.d0 = base.d0 "
    "LEFT JOIN tb a2 ON a2.d0 = base.d0"
)

# Control 2: same CASE but across two DISTINCT tables (was always correct).
DISTINCT_TABLES_CASE = (
    "SELECT base.d0, "
    "       CASE WHEN a1.s > 0 THEN a1.s ELSE a2.e END AS c "
    "FROM base "
    "LEFT JOIN tb a1 ON a1.d0 = base.d0 "
    "LEFT JOIN other a2 ON a2.d0 = base.d0"
)

stock = duckdb.connect()
for sql in SETUP:
    stock.execute(sql)

circ = duckdb.connect(config={"allow_unsigned_extensions": "true"})
circ.execute(f"LOAD '{EXT}'")
for sql in SETUP:
    circ.execute(sql)

CASES = {
    "self_join_case": SELF_JOIN_CASE,
    "self_join_plain": SELF_JOIN_PLAIN,
    "distinct_tables_case": DISTINCT_TABLES_CASE,
}

failed = False
for name, sql in CASES.items():
    expected = sorted(stock.execute(sql).fetchall())
    try:
        circ.execute(f"CREATE MATERIALIZED VIEW mv_{name} AS {sql}")
    except duckdb.Error as e:
        # Loud rejection is acceptable (never silent wrongness) — but the
        # shipped fix computes these shapes, so treat rejection as failure
        # to catch a regression from "fixed" back to "rejected".
        print(f"FAIL: {name} rejected at CREATE: {e}", flush=True)
        failed = True
        continue
    actual = sorted(circ.execute(f"SELECT * FROM dbsp_query('mv_{name}')").fetchall())
    if actual != expected:
        print(f"FAIL: {name} diverges from stock DuckDB", flush=True)
        print(f"  expected: {expected}", flush=True)
        print(f"  actual:   {actual}", flush=True)
        failed = True
    else:
        print(f"ok: {name} matches stock ({len(actual)} rows)", flush=True)

# Incremental follow-up on the defective shape: edit tb, both engines agree.
if not failed:
    stock.execute("INSERT INTO tb VALUES (3, 300.0, 3000.0)")
    circ.execute("INSERT INTO tb VALUES (3, 300.0, 3000.0)")
    expected = sorted(stock.execute(SELF_JOIN_CASE).fetchall())
    actual = sorted(
        circ.execute("SELECT * FROM dbsp_query('mv_self_join_case')").fetchall()
    )
    if actual != expected:
        print("FAIL: self_join_case diverges after incremental insert", flush=True)
        print(f"  expected: {expected}", flush=True)
        print(f"  actual:   {actual}", flush=True)
        failed = True
    else:
        print(f"ok: self_join_case incremental matches ({len(actual)} rows)", flush=True)

# Root-cause shapes: an all-one-branch commit primes the stale reference,
# the following mixed-branch commit writes through it. No self-join needed.
circ2 = duckdb.connect(config={"allow_unsigned_extensions": "true"})
circ2.execute(f"LOAD '{EXT}'")
circ2.execute("CREATE TABLE tb (d0 INTEGER, s DOUBLE, e DOUBLE)")
circ2.execute(
    "CREATE MATERIALIZED VIEW mv_staged AS "
    "SELECT d0, CASE WHEN s > 0 THEN s ELSE e END AS c, e AS p2 FROM tb"
)
circ2.execute("INSERT INTO tb VALUES (2, -200.0, 2000.0)")  # all-ELSE batch
circ2.execute(
    "INSERT INTO tb VALUES (1, 100.0, 1000.0), (4, -1.0, 4000.0)"
)  # mixed batch
actual = sorted(circ2.execute("SELECT * FROM dbsp_query('mv_staged')").fetchall())
expected = [(1, 100.0, 1000.0), (2, 2000.0, 2000.0), (4, 4000.0, 4000.0)]
if actual != expected:
    print("FAIL: staged single-table CASE diverges", flush=True)
    print(f"  expected: {expected}", flush=True)
    print(f"  actual:   {actual}", flush=True)
    failed = True
else:
    print("ok: staged single-table CASE matches", flush=True)
circ2.close()

circ3 = duckdb.connect(config={"allow_unsigned_extensions": "true"})
circ3.execute(f"LOAD '{EXT}'")
circ3.execute("CREATE TABLE base (d0 INTEGER, v DOUBLE)")
circ3.execute("CREATE TABLE tb (d0 INTEGER, s DOUBLE, e DOUBLE)")
circ3.execute("CREATE TABLE other (d0 INTEGER, s DOUBLE, e DOUBLE)")
sql_dt_p2 = (
    "SELECT base.d0, "
    "       CASE WHEN a1.s > 0 THEN a1.s ELSE a2.e END AS c, a2.e AS p2 "
    "FROM base "
    "LEFT JOIN tb a1 ON a1.d0 = base.d0 "
    "LEFT JOIN other a2 ON a2.d0 = base.d0"
)
# view first, THEN the commits: base alone makes every row an all-ELSE
# NULL pad (primes the stale reference); the tb/other fills are the
# mixed-branch batches that used to write through it
circ3.execute("CREATE MATERIALIZED VIEW mv_dt_p2 AS " + sql_dt_p2)
circ3.execute("INSERT INTO base VALUES (1, 10.0), (2, 20.0), (3, 30.0)")
circ3.execute("INSERT INTO tb VALUES (1, 100.0, 1000.0), (2, -200.0, 2000.0)")
circ3.execute("INSERT INTO other VALUES (1, 100.0, 1000.0), (2, -200.0, 2000.0)")
actual = sorted(
    circ3.execute("SELECT * FROM dbsp_query('mv_dt_p2')").fetchall(),
    key=lambda r: r[0],
)
expected = [(1, 100.0, 1000.0), (2, 2000.0, 2000.0), (3, None, None)]
if actual != expected:
    print("FAIL: staged distinct-tables CASE diverges", flush=True)
    print(f"  expected: {expected}", flush=True)
    print(f"  actual:   {actual}", flush=True)
    failed = True
else:
    print("ok: staged distinct-tables CASE matches", flush=True)
circ3.close()

if failed:
    print("FAIL", flush=True)
    sys.exit(1)
print("PASS", flush=True)

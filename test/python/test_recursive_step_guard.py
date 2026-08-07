"""Recursive-step shape guard (regression).

Row-collapsing operators inside a recursive STEP (DISTINCT / GROUP BY /
LIMIT) do not follow SQL's per-iteration working-table semantics on any
of the engine's fixpoint paths — such views used to be ACCEPTED and
silently wrong. They must now be REJECTED loudly at CREATE, while plain
linear recursion keeps working (and stays incremental).

Run: python test_recursive_step_guard.py <path-to-dbsp.duckdb_extension>
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

conn = duckdb.connect(config={"allow_unsigned_extensions": "true"})
conn.execute(f"LOAD '{EXT}'")
conn.execute("CREATE TABLE edges (src INTEGER, dst INTEGER)")
conn.execute("INSERT INTO edges VALUES (1, 2), (2, 3), (3, 4), (10, 11)")

# Sanity: LINEAR recursion still accepted and correct + incremental.
conn.execute(
    "CREATE MATERIALIZED VIEW reach AS "
    "WITH RECURSIVE r AS ("
    "  SELECT src, dst FROM edges WHERE src = 1 "
    "  UNION ALL "
    "  SELECT r.src, e.dst FROM r JOIN edges e ON e.src = r.dst"
    ") SELECT * FROM r"
)


def rows():
    return sorted(conn.execute("SELECT src, dst FROM dbsp_query('reach')").fetchall())


assert rows() == [(1, 2), (1, 3), (1, 4)], f"linear recursion wrong: {rows()}"
conn.execute("INSERT INTO edges VALUES (4, 5)")
assert rows() == [(1, 2), (1, 3), (1, 4), (1, 5)], (
    f"incremental linear recursion wrong: {rows()}"
)

# Row-collapsing steps: loud rejection, never silent acceptance.
BAD_STEPS = {
    "distinct": (
        "WITH RECURSIVE r AS (SELECT src, dst FROM edges WHERE src = 1 "
        "UNION ALL SELECT DISTINCT r.src, e.dst FROM r "
        "JOIN edges e ON e.src = r.dst) SELECT * FROM r"
    ),
    "group_by": (
        "WITH RECURSIVE r AS (SELECT src, dst FROM edges WHERE src = 1 "
        "UNION ALL SELECT r.src, MAX(e.dst) FROM r "
        "JOIN edges e ON e.src = r.dst GROUP BY r.src) SELECT * FROM r"
    ),
    "limit": (
        "WITH RECURSIVE r AS (SELECT src, dst FROM edges WHERE src = 1 "
        "UNION ALL SELECT r.src, e.dst FROM r "
        "JOIN edges e ON e.src = r.dst ORDER BY e.dst LIMIT 1) SELECT * FROM r"
    ),
}

for name, sql in BAD_STEPS.items():
    try:
        conn.execute(f"CREATE MATERIALIZED VIEW bad_{name} AS {sql}")
    except duckdb.Error as e:
        # Ours ("recursive step contains a row-collapsing operator") or
        # stock DuckDB's own refusal (LIMIT in recursion is a parse error
        # upstream) — either way it is LOUD, which is the contract.
        msg = str(e)
        assert "recursive" in msg or "DBSP" in msg, (
            f"{name}: rejected but with an unrelated error: {msg}"
        )
    else:
        got = sorted(
            conn.execute(f"SELECT * FROM dbsp_query('bad_{name}')").fetchall()
        )
        raise AssertionError(
            f"{name}: silently ACCEPTED a row-collapsing recursive step "
            f"(got {got}) — must reject at CREATE"
        )

print("PASS", flush=True)

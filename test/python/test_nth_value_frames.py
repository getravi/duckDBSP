"""NTH_VALUE frame semantics (regression).

NTH_VALUE(x, n) returns the nth row OF THE FRAME — default frame RANGE
UNBOUNDED PRECEDING..CURRENT ROW, so rows before the nth are NULL. The
old implementation indexed the whole partition (partition_rows[n-1]) for
every row: early rows wrongly saw the value, and explicit ROWS frames
were ignored entirely. Truth = DuckDB's own window executor on the same
data.

Run: python test_nth_value_frames.py <path-to-dbsp.duckdb_extension>
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
conn.execute("CREATE TABLE t (g INTEGER, o INTEGER, v DOUBLE)")
conn.execute(
    "INSERT INTO t SELECT i // 5, i % 5, (i * 7) % 13 + 0.5 FROM range(20) r(i)"
)

CASES = {
    # default frame: rows before the 2nd are NULL
    "nv_default": "NTH_VALUE(v, 2) OVER (PARTITION BY g ORDER BY o)",
    # explicit sliding frame: nth row OF THE FRAME, not the partition
    "nv_rows": (
        "NTH_VALUE(v, 2) OVER (PARTITION BY g ORDER BY o "
        "ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING)"
    ),
    # unbounded frame: whole partition for every row
    "nv_unbounded": (
        "NTH_VALUE(v, 3) OVER (PARTITION BY g ORDER BY o "
        "ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING)"
    ),
}


def truth(expr):
    return sorted(
        conn.execute(f"SELECT g, o, {expr} FROM t").fetchall(),
        key=lambda r: (r[0], r[1]),
    )


def view(name):
    return sorted(
        conn.execute(f"SELECT g, o, nv FROM dbsp_query('{name}')").fetchall(),
        key=lambda r: (r[0], r[1]),
    )


for name, expr in CASES.items():
    conn.execute(f"CREATE MATERIALIZED VIEW {name} AS SELECT g, o, {expr} AS nv FROM t")
    got, want = view(name), truth(expr)
    assert got == want, f"{name} initial: {got} != {want}"

# Incremental: inserts and a delete must keep every case exact.
conn.execute("INSERT INTO t VALUES (0, 5, 99.5), (4, 0, 1.5)")
for name, expr in CASES.items():
    got, want = view(name), truth(expr)
    assert got == want, f"{name} after insert: {got} != {want}"

conn.execute("DELETE FROM t WHERE g = 0 AND o = 1")
for name, expr in CASES.items():
    got, want = view(name), truth(expr)
    assert got == want, f"{name} after delete: {got} != {want}"

print("PASS", flush=True)

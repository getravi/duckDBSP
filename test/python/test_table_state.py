"""Regression test: dbsp_table_state() meters tracked-table baseline RAM.

Pins the bounded-RAM Phase 5 accounting gap: boxed baselines (~300 B/row)
are the dominant big-model RAM class and are NOT in dbsp_view_state — a
budget must sum both functions. Checks mode transitions
boxed -> spilled -> deferred and that reported bytes scale with rows.

Run: python test_table_state.py <path-to-dbsp.duckdb_extension>
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

DB = tempfile.mktemp(suffix=".duckdb")


def connect():
    c = duckdb.connect(DB, config={"allow_unsigned_extensions": "true"})
    c.execute(f"LOAD '{EXT}'")
    return c


conn = connect()
conn.execute("CREATE TABLE t (k INTEGER, g INTEGER, v DOUBLE)")
conn.execute("INSERT INTO t SELECT range, range % 5, range * 1.0 FROM range(100000)")
conn.execute("CREATE MATERIALIZED VIEW mv AS SELECT g, SUM(v) AS s FROM t GROUP BY g")

rows = conn.execute("SELECT * FROM dbsp_table_state()").fetchall()
assert len(rows) == 1, f"want 1 tracked table, got {rows}"
name, mode, n, b = rows[0]
assert mode == "boxed", f"fresh baseline must be boxed: {mode}"
assert n == 100000, f"distinct_rows wrong: {n}"
# Boxed baselines cost hundreds of bytes/row; anything under 100 B/row
# means the meter lost the class it exists to expose.
assert b > 100 * n, f"boxed bytes implausibly low: {b}"
boxed_bytes = b

# Spill migrates the baseline to a digest index (~72 B/row indexed).
conn.execute("SELECT * FROM dbsp_spill(true)")
name, mode, n, b = conn.execute("SELECT * FROM dbsp_table_state()").fetchall()[0]
assert mode == "spilled", f"after dbsp_spill(true): {mode}"
assert n == 100000, f"spilled distinct_rows wrong: {n}"
assert b < boxed_bytes // 3, f"spill must shrink the meter: {boxed_bytes} -> {b}"

# Bytes keep metering edits (baseline updated through the spill index).
conn.execute("UPDATE t SET v = v + 1 WHERE k = 7")
mode2, n2 = conn.execute("SELECT mode, distinct_rows FROM dbsp_table_state()").fetchall()[0]
assert (mode2, n2) == ("spilled", 100000), f"post-edit state wrong: {mode2}, {n2}"

# Deferred (lazy-restore fast path): reopen without touching table state.
conn.execute("SELECT * FROM dbsp_save()")
conn.close()
conn = connect()
conn.execute("SELECT count(*) FROM dbsp_views()")
name, mode, n, b = conn.execute("SELECT * FROM dbsp_table_state()").fetchall()[0]
assert mode == "deferred", f"lazy-restored baseline must be deferred: {mode}"
assert b < 1_000_000, f"deferred baseline must be near-zero resident: {b}"

# First edit materializes it; meter must follow.
conn.execute("UPDATE t SET v = v + 1 WHERE k = 8")
name, mode, n, b = conn.execute("SELECT * FROM dbsp_table_state()").fetchall()[0]
assert mode in ("boxed", "spilled"), f"post-edit baseline still deferred: {mode}"
assert n == 100000, f"materialized distinct_rows wrong: {n}"

conn.execute("SELECT * FROM dbsp_save()")
conn.close()

drain = duckdb.connect(config={"allow_unsigned_extensions": "true"})
drain.execute(f"LOAD '{EXT}'")
drain.execute("SELECT * FROM dbsp_wait_teardown()")
drain.close()

print("PASS")

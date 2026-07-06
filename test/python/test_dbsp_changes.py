"""D4 regression test: dbsp_changes(view) returns the last sync's output
delta as (view columns..., weight) with signed weights.

Run: python test_dbsp_changes.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import sys

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 60


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)

conn = duckdb.connect(config={"allow_unsigned_extensions": "true"})
conn.execute(f"LOAD '{EXT}'")
conn.execute("CREATE TABLE li (k INTEGER, v DOUBLE)")
conn.execute("CREATE MATERIALIZED VIEW li_sum AS SELECT k, SUM(v) AS s FROM li GROUP BY k")


def changes():
    return sorted(conn.execute("SELECT k, s, weight FROM dbsp_changes('li_sum')").fetchall())


# Initial insert: two groups appear (+1 each).
conn.execute("INSERT INTO li VALUES (1, 10.0), (1, 5.0), (2, 7.0)")
got = changes()
assert got == [(1, 15.0, 1), (2, 7.0, 1)], f"initial delta wrong: {got}"

# Reading again without a new sync returns the same buffered delta.
assert changes() == got, "delta must be stable between syncs"

# Update group 1: old aggregate retracted (-1), new one inserted (+1);
# group 2 untouched — absent from the delta.
conn.execute("INSERT INTO li VALUES (1, 1.0)")
got = changes()
assert got == [(1, 15.0, -1), (1, 16.0, 1)], f"update delta wrong: {got}"

# Delete all of group 2: pure retraction.
conn.execute("DELETE FROM li WHERE k = 2")
got = changes()
assert got == [(2, 7.0, -1)], f"delete delta wrong: {got}"

# Result itself stays correct.
res = dict(conn.execute("SELECT k, s FROM dbsp_query('li_sum')").fetchall())
assert res == {1: 16.0}, f"result wrong: {res}"

# Unknown view errors.
try:
    conn.execute("SELECT * FROM dbsp_changes('nope')")
    raise AssertionError("expected error for unknown view")
except duckdb.Error as e:
    assert "not found" in str(e).lower(), f"unexpected error: {e}"

# Notify path (fresh view): each dbsp_notify_* call is its own sync step.
# While a group survives a step, its aggregate update appears as a
# (old, -1), (new, +1) pair — hosts get old and new values in one read.
# A delete+insert pair of notifies is TWO steps, so the buffer holds only
# the second step's delta; hosts needing the first read between notifies.
conn.execute("CREATE TABLE li2 (k INTEGER, v DOUBLE)")
conn.execute("INSERT INTO li2 VALUES (1, 10.0), (1, 5.0)")
conn.execute("CREATE MATERIALIZED VIEW li2_sum AS SELECT k, SUM(v) AS s FROM li2 GROUP BY k")
conn.execute("SELECT * FROM dbsp_auto_sync(false)")

conn.execute("UPDATE li2 SET v = 12.0 WHERE k = 1 AND v = 10.0")
conn.execute("SELECT * FROM dbsp_notify_delete('li2', 1, 10.0)")
mid = sorted(conn.execute("SELECT k, s, weight FROM dbsp_changes('li2_sum')").fetchall())
assert mid == [(1, 5.0, 1), (1, 15.0, -1)], f"delete-step delta wrong: {mid}"

conn.execute("SELECT * FROM dbsp_notify_insert('li2', 1, 12.0)")
got = sorted(conn.execute("SELECT k, s, weight FROM dbsp_changes('li2_sum')").fetchall())
assert got == [(1, 5.0, -1), (1, 17.0, 1)], f"insert-step delta wrong: {got}"

res = dict(conn.execute("SELECT k, s FROM dbsp_query('li2_sum')").fetchall())
assert res == {1: 17.0}, f"li2 result wrong: {res}"

conn.close()
signal.alarm(0)
print("PASS: dbsp_changes returns signed last-sync deltas", flush=True)

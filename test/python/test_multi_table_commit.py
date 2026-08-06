"""Multi-table commit = ONE circuit pass (regression).

A transaction writing SEVERAL tracked tables used to propagate once per
table at COMMIT; each pass rewrote every downstream view's single-generation
dbsp_changes buffer, so a view reading BOTH tables kept only the last
table's effects, and a join both of whose sides changed missed its
both-shared correction. sync_tables now collects every table's delta and
runs one propagate_changes_multi pass.

Run: python test_multi_table_commit.py <path-to-dbsp.duckdb_extension>
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
conn.execute("CREATE TABLE ta (k INTEGER, v DOUBLE)")
conn.execute("CREATE TABLE tb (k INTEGER, w DOUBLE)")
# One view over each table, plus one reading BOTH (the shape that lost data).
conn.execute("CREATE MATERIALIZED VIEW va AS SELECT k, SUM(v) AS sv FROM ta GROUP BY k")
conn.execute("CREATE MATERIALIZED VIEW vb AS SELECT k, SUM(w) AS sw FROM tb GROUP BY k")
conn.execute(
    "CREATE MATERIALIZED VIEW vj AS "
    "SELECT a.k AS k, a.v * b.w AS p FROM ta a JOIN tb b ON a.k = b.k"
)

conn.execute("INSERT INTO ta VALUES (1, 10.0), (2, 20.0)")
conn.execute("INSERT INTO tb VALUES (1, 3.0), (2, 4.0)")


def changes(view):
    return sorted(conn.execute(f"SELECT * FROM dbsp_changes('{view}')").fetchall())


def gens():
    return dict(
        conn.execute("SELECT view_name, generation FROM dbsp_delta_generations()").fetchall()
    )


# ONE transaction touching BOTH tables.
conn.execute("BEGIN")
conn.execute("UPDATE ta SET v = 100.0 WHERE k = 1")  # drives va + vj
conn.execute("UPDATE tb SET w = 7.0 WHERE k = 2")  # drives vb + vj
conn.execute("COMMIT")

# Each single-source view sees its own table's effect.
got_va = changes("va")
assert got_va == [(1, 10.0, -1), (1, 100.0, 1)], f"va delta wrong: {got_va}"
got_vb = changes("vb")
assert got_vb == [(2, 4.0, -1), (2, 7.0, 1)], f"vb delta wrong: {got_vb}"

# The BOTH-tables view must carry BOTH effects in one delta — this is the
# buffer the per-table passes clobbered (only tb's effect survived).
got_vj = changes("vj")
assert got_vj == [
    (1, 30.0, -1),
    (1, 300.0, 1),
    (2, 80.0, -1),
    (2, 140.0, 1),
], f"vj delta wrong: {got_vj}"

# One pass ⇒ every stepped view carries the SAME delta generation.
g = gens()
assert g["va"] == g["vb"] == g["vj"], f"generations differ (multiple passes): {g}"

# Both-shared join correction: insert a NEW key on both sides in one txn.
# One pass yields exactly one +1 row for the new join match.
conn.execute("BEGIN")
conn.execute("INSERT INTO ta VALUES (3, 5.0)")
conn.execute("INSERT INTO tb VALUES (3, 6.0)")
conn.execute("COMMIT")
got_vj = changes("vj")
assert got_vj == [(3, 30.0, 1)], f"both-sides insert delta wrong: {got_vj}"

# Results themselves stay exact.
res = dict(conn.execute("SELECT k, p FROM dbsp_query('vj') ORDER BY k").fetchall())
assert res == {1: 300.0, 2: 140.0, 3: 30.0}, f"vj result wrong: {res}"

print("PASS", flush=True)

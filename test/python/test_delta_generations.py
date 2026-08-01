"""Regression test: dbsp_delta_generations() stamps each view's delta buffer
with the commit generation that wrote it.

dbsp_changes is a single-generation buffer that reading does NOT drain — an
untouched view keeps serving its last delta (its initial replay, at worst)
forever. A change-feed consumer must therefore compare each view's generation
against a baseline and skip views whose buffer predates the commits it cares
about. This test pins that contract.

Run: python test_delta_generations.py <path-to-dbsp.duckdb_extension>
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


def gens():
    return dict(conn.execute("SELECT view_name, generation FROM dbsp_delta_generations()").fetchall())


conn.execute("CREATE TABLE ta (k INTEGER, v DOUBLE)")
conn.execute("CREATE TABLE tb (k INTEGER, v DOUBLE)")
conn.execute("INSERT INTO ta VALUES (1, 10.0)")
conn.execute("INSERT INTO tb VALUES (1, 1.0)")
conn.execute("CREATE MATERIALIZED VIEW mva AS SELECT k, v * 2 AS v2 FROM ta")
conn.execute("CREATE MATERIALIZED VIEW mvb AS SELECT k, v + 1 AS v1 FROM tb")

g0 = gens()
assert set(g0) == {"mva", "mvb"}, f"views missing: {g0}"
baseline = max(g0.values())

# Edit ta only: mva's generation must advance past the baseline, mvb's must
# not — even though mvb's dbsp_changes buffer still holds its initial replay.
conn.execute("UPDATE ta SET v = 11.0 WHERE k = 1")
g1 = gens()
assert g1["mva"] > baseline, f"touched view did not advance: {g1}"
assert g1["mvb"] == g0["mvb"], f"untouched view advanced: {g1}"
stale = conn.execute("SELECT count(*) FROM dbsp_changes('mvb')").fetchone()[0]
assert stale > 0, "precondition lost: untouched view should still buffer its old delta"

# Edit tb: now mvb advances past mva.
conn.execute("UPDATE tb SET v = 2.0 WHERE k = 1")
g2 = gens()
assert g2["mvb"] > g1["mva"], f"second edit did not advance mvb: {g2}"
assert g2["mva"] == g1["mva"], f"mva advanced without an edit: {g2}"

# Reads never advance generations.
conn.execute("SELECT count(*) FROM dbsp_changes('mva')").fetchone()
conn.execute("SELECT count(*) FROM dbsp_query('mva')").fetchone()
assert gens() == g2, "reading changed a generation"

# Dropped views leave the listing.
conn.execute("SELECT dbsp_drop('mvb')")
assert set(gens()) == {"mva"}, f"dropped view still listed: {gens()}"

print("PASS")

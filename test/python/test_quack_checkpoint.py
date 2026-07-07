"""Quack-mode checkpoint: persist circuit state INTO the attached model DB so
a process restart RESTORES views instead of cold-rebuilding them.

Production (NuEPM quack mode) reaches model DBs by ATTACH on a shared server
connection. dbsp_save/dbsp_load historically wrote to the DEFAULT catalog (the
in-memory server) — which dies on restart. This test drives the attach-mode
round trip: save into the attached catalog, simulate a restart (new process /
fresh server connection), load from the attached catalog, and confirm the view
is restored correct and incrementally live WITHOUT replaying a full rebuild.

Run: python test_quack_checkpoint.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import sys
import tempfile

import duckdb

EXT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension")
TIMEOUT_S = 90


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)

CFG = {"allow_unsigned_extensions": "true"}
VIEW_SQL = (
    'SELECT c.ancestor_id AS dim_0, SUM(l.value) AS value '
    'FROM "m".li_0 l JOIN "m".dbsp_closure_0 c ON l.dim_0 = c.child_id '
    "WHERE l.dim_0 < 100 GROUP BY c.ancestor_id"
)
ROOT = 110


def leaf_sum():
    return sum(float(i) for i in range(100))


with tempfile.TemporaryDirectory() as tmp:
    db = os.path.join(tmp, "model.duckdb")
    setup = duckdb.connect(db, config=CFG)
    setup.execute(f"LOAD '{EXT}'")
    setup.execute("CREATE TABLE li_0 (dim_0 INTEGER, value DOUBLE)")
    setup.execute("INSERT INTO li_0 SELECT i, i * 1.0 FROM range(100) t(i)")
    setup.execute("CREATE TABLE dbsp_closure_0 (child_id INTEGER, ancestor_id INTEGER)")
    setup.executemany(
        "INSERT INTO dbsp_closure_0 VALUES (?, ?)",
        [(leaf, 100 + leaf // 10) for leaf in range(100)]
        + [(leaf, ROOT) for leaf in range(100)],
    )
    setup.close()

    # ── session 1: shared conn, attach, create view, edit, SAVE to attached ──
    c1 = duckdb.connect(config=CFG)
    c1.execute(f"LOAD '{EXT}'")
    c1.execute(f"ATTACH '{db}' AS m (READ_WRITE)")
    c1.execute("SELECT * FROM dbsp_auto_sync(false)")
    c1.execute(f"CREATE MATERIALIZED VIEW rollup_0 AS {VIEW_SQL}")
    root1 = c1.execute("SELECT value FROM dbsp_query('rollup_0') WHERE dim_0 = ?", [ROOT]).fetchone()[0]
    assert root1 == leaf_sum(), f"pre-save root wrong: {root1}"
    # persist checkpoint INTO the attached model DB (not the default catalog)
    c1.execute("SELECT * FROM dbsp_save(catalog := 'm')")
    c1.execute("DETACH m")
    c1.close()

    # checkpoint tables must be ON DISK in the model file
    chk = duckdb.connect(db, config=CFG)
    tables = {t[0] for t in chk.execute("SHOW TABLES").fetchall()}
    chk.close()
    assert "_dbsp_views" in tables, f"view defs not persisted to model file: {tables}"
    assert "_dbsp_ckpt" in tables, f"circuit checkpoint not persisted to model file: {tables}"
    print("PASS: checkpoint persisted into the attached model file")

    # ── session 2 (simulated restart): fresh server conn, attach, LOAD ──
    c2 = duckdb.connect(config=CFG)
    c2.execute(f"LOAD '{EXT}'")
    c2.execute(f"ATTACH '{db}' AS m (READ_WRITE)")
    c2.execute("SELECT * FROM dbsp_auto_sync(false)")
    c2.execute("SELECT * FROM dbsp_load(catalog := 'm')")

    root2 = c2.execute("SELECT value FROM dbsp_query('rollup_0') WHERE dim_0 = ?", [ROOT]).fetchone()[0]
    assert root2 == leaf_sum(), f"restored root wrong: {root2} != {leaf_sum()}"
    print("PASS: view restored correct after simulated restart")

    # ── post-restore incremental edit must be exact (proves circuit state,
    #    not just the sink value, was restored) ──
    c2.execute("UPDATE m.li_0 SET value = 1000.0 WHERE dim_0 = 0")
    c2.execute("SELECT * FROM dbsp_notify_delete('\"m\".li_0', 0, 0.0)")
    c2.execute("SELECT * FROM dbsp_notify_insert('\"m\".li_0', 0, 1000.0)")
    root3 = c2.execute("SELECT value FROM dbsp_query('rollup_0') WHERE dim_0 = ?", [ROOT]).fetchone()[0]
    expected = leaf_sum() - 0.0 + 1000.0
    assert root3 == expected, f"post-restore incremental wrong: {root3} != {expected}"
    print("PASS: post-restore incremental edit exact")
    c2.close()

print("ALL PASS")

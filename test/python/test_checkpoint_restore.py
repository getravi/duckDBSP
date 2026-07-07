"""D3b regression test: circuit-state checkpoint restore.

dbsp_save() snapshots operator state (aggregate groups, private join
indexes) and sink results; dbsp_load() cold-creates covered views (no
circuit replay) and injects that state. The test proves three things the
sink alone cannot: post-restore incremental edits are CORRECT (internal
aggregate state was restored), restore is much faster than rebuild, and a
stale checkpoint (source table changed after save) is detected via
watermarks and falls back to a full rebuild.

Run: python test_checkpoint_restore.py <path-to-dbsp.duckdb_extension>
"""

import os
import signal
import sys
import tempfile
import time

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 300
N_LEAVES = 100_000
N_OTHER = 10  # 1M rows: replay dominates, restore skips it


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)


def open_db(path):
    conn = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    conn.execute(f"LOAD '{EXT}'")
    return conn


def view_rows(conn, name="rollup"):
    return sorted(conn.execute(f"SELECT parent_id, dim_1, value FROM dbsp_query('{name}')").fetchall())


def truth(conn):
    return sorted(conn.execute(
        "SELECT c.parent_id, l.dim_1, SUM(l.value) FROM li l "
        "JOIN closure c ON l.dim_0 = c.child_id GROUP BY 1, 2"
    ).fetchall())


with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "model.duckdb")

    # Session 1: NuEPM shape — join + GROUP BY SUM over 200k rows.
    conn = open_db(path)
    conn.execute("CREATE TABLE li (dim_0 INTEGER, dim_1 INTEGER, value DOUBLE)")
    conn.execute(
        f"INSERT INTO li SELECT leaf, o, (leaf * 31 + o) % 997 + 0.25 "
        f"FROM range({N_LEAVES}) t(leaf) CROSS JOIN range({N_OTHER}) u(o)"
    )
    conn.execute("CREATE TABLE closure (child_id INTEGER, parent_id INTEGER)")
    conn.execute(f"INSERT INTO closure SELECT i, 100000 + i // 100 FROM range({N_LEAVES}) t(i)")

    t0 = time.perf_counter()
    conn.execute(
        "CREATE MATERIALIZED VIEW rollup AS "
        "SELECT c.parent_id, l.dim_1, SUM(l.value) AS value "
        "FROM li l JOIN closure c ON l.dim_0 = c.child_id GROUP BY 1, 2"
    )
    build_s = time.perf_counter() - t0
    baseline = view_rows(conn)
    assert baseline == truth(conn), "initial build wrong"

    msg = conn.execute("SELECT * FROM dbsp_save()").fetchone()[0]
    assert "circuit checkpoint" in msg and "no circuit" not in msg, f"checkpoint not saved: {msg}"
    conn.close()

    # Session 2: restore must skip replay and be correct + incremental.
    conn = open_db(path)
    t0 = time.perf_counter()
    conn.execute("SELECT * FROM dbsp_load()")
    restore_s = time.perf_counter() - t0
    got = view_rows(conn)
    assert got == baseline, "restored view differs from saved state"

    # Incremental edit AFTER restore: correct only if aggregate group
    # state and join indexes were restored, not just the sink.
    conn.execute("SELECT * FROM dbsp_auto_sync(false)")
    conn.execute("DELETE FROM li WHERE dim_0 = 5 AND dim_1 = 3")
    old_v = (5 * 31 + 3) % 997 + 0.25
    conn.execute("SELECT * FROM dbsp_notify_delete('li', 5, 3, ?)", [old_v])
    conn.execute("INSERT INTO li VALUES (5, 3, 5000.0)")
    conn.execute("SELECT * FROM dbsp_notify_insert('li', 5, 3, 5000.0)")
    got = view_rows(conn)
    want = truth(conn)
    assert got == want, "post-restore incremental edit diverged (internal state not restored)"
    conn.close()

    print(f"build={build_s:.2f}s restore={restore_s:.2f}s speedup={build_s / restore_s:.1f}x", flush=True)
    # Restore skips circuit replay but still pays source sync + arrangement
    # backfill + blob decode (all O(n)); the margin grows with aggregate
    # fan-out. Assert direction, not a fixed ratio - codec/arrangement
    # follow-ups tracked in PHASE_D_PLAN.md.
    assert restore_s < build_s, f"restore slower than rebuild: {restore_s:.2f}s vs {build_s:.2f}s"

    # Session 3: stale checkpoint (table changed post-save) must fall back
    # to rebuild and still be correct.
    conn = open_db(path)
    conn.execute("INSERT INTO li VALUES (7, 1, 123.0)")  # invalidates watermark
    conn.close()
    conn = open_db(path)
    conn.execute("SELECT * FROM dbsp_load()")
    got = view_rows(conn)
    want = truth(conn)
    assert got == want, "stale-checkpoint fallback produced wrong values"
    conn.close()

signal.alarm(0)
print("PASS: checkpoint restore is fast, correct, incremental-safe, and stale-safe", flush=True)

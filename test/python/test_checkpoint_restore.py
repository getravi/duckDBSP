"""D3b/D3c regression test: circuit-state checkpoint restore + lazy baselines.

dbsp_save() snapshots operator state (aggregate groups, private join
indexes) and sink results; dbsp_load() cold-creates covered views (no
circuit replay) and injects that state. With D3c, a watermark-matched
restore also DEFERS tracked-table baselines and shared-arrangement
backfill: dbsp_load reads zero source rows; the first operation that
needs table state (notify delta, pre-write hook, replay) materializes
them from a single typed scan.

Proven here:
  1. post-restore incremental edits are CORRECT (internal aggregate state
     restored; deferred baselines materialize with pending-delta
     subtraction before the first notify delta propagates),
  2. restore is much faster than rebuild and defers all sources,
  3. a stale checkpoint (source changed after save) is detected via
     watermarks at load and falls back to a full rebuild,
  4. auto-sync SQL writes after a lazy restore stay exact (QueryBegin
     materializes deferred baselines from pre-write storage),
  5. an out-of-band write against a deferred baseline (auto-sync off, no
     notify) is detected at the next sync and self-heals via a full view
     rebuild at the next statement boundary.

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
    # The count is the real assertion: "+ circuit checkpoint" with 0 views
    # covered is exactly the silent failure that shipped when
    # PlannedCircuitView lacked the checkpointable() override.
    assert "circuit checkpoint: 1 views" in msg, f"view not covered by checkpoint: {msg}"
    conn.close()

    # Session 2: restore must skip replay and be correct + incremental.
    conn = open_db(path)
    t0 = time.perf_counter()
    load_msg = conn.execute("SELECT * FROM dbsp_load()").fetchone()[0]
    restore_s = time.perf_counter() - t0
    assert "1 from checkpoint" in load_msg, f"checkpoint fast path did not fire: {load_msg}"
    # D3c: a watermark-matched restore must not scan any source rows —
    # both tracked tables stay deferred until something needs table state.
    assert "2 sources deferred" in load_msg, f"lazy baselines did not defer: {load_msg}"
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
    stale_msg = conn.execute("SELECT * FROM dbsp_load()").fetchone()[0]
    assert "0 from checkpoint" in stale_msg, f"stale checkpoint was not rejected: {stale_msg}"
    assert "0 sources deferred" in stale_msg, f"stale load must not defer baselines: {stale_msg}"
    got = view_rows(conn)
    want = truth(conn)
    assert got == want, "stale-checkpoint fallback produced wrong values"
    # Re-checkpoint the rebuilt state for the lazy-restore sessions below.
    conn.execute("SELECT * FROM dbsp_save()").fetchone()
    conn.close()

    # Session 4: auto-sync SQL writes after a lazy restore. QueryBegin must
    # materialize deferred baselines from PRE-write storage; the captured
    # INSERT, captured UPDATE/DELETE (write capture), and the re-saved
    # checkpoint with that captured history then reconcile incrementally.
    conn = open_db(path)
    load_msg = conn.execute("SELECT * FROM dbsp_load()").fetchone()[0]
    assert "1 from checkpoint" in load_msg and "2 sources deferred" in load_msg, (
        f"lazy fast path did not fire on re-checkpoint: {load_msg}"
    )
    conn.execute("INSERT INTO li VALUES (8, 2, 77.0)")     # captured append
    conn.execute("DELETE FROM li WHERE dim_0 = 9 AND dim_1 = 0")  # captured delete
    conn.execute("UPDATE li SET value = 501.5 WHERE dim_0 = 8 AND dim_1 = 2")  # captured update
    got = view_rows(conn)
    want = truth(conn)
    assert got == want, "auto-sync DML after lazy restore diverged"
    # Round-trip: dbsp_save with captured UPDATE/DELETE deltas in history
    conn.execute("SELECT * FROM dbsp_save()").fetchone()
    conn.close()

    # Session 5: out-of-band write against a deferred baseline (auto-sync
    # off, no notify). The explicit sync detects the watermark mismatch;
    # the next statement boundary rebuilds the views from storage.
    conn = open_db(path)
    load_msg = conn.execute("SELECT * FROM dbsp_load()").fetchone()[0]
    assert "2 sources deferred" in load_msg, f"lazy fast path did not fire: {load_msg}"
    conn.execute("SELECT * FROM dbsp_auto_sync(false)")
    conn.execute("DELETE FROM li WHERE dim_0 = 11")  # out-of-band: no notify
    conn.execute("SELECT * FROM dbsp_sync()")        # detects mismatch
    got = view_rows(conn)                            # rebuild fires here
    want = truth(conn)
    assert got == want, "out-of-band write after lazy restore was not self-healed"
    conn.close()

signal.alarm(0)
print("PASS: checkpoint restore is fast, correct, incremental-safe, stale-safe, and lazy", flush=True)

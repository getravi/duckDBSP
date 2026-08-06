"""Delta-append sidecars: a dirty save is O(changed rows), not O(baseline).

After a reopen ADOPTS the durable sidecars (baseline digest index +
shared-arrangement fingerprint files), a dirty save writes only small
delta files (.idx.d / <fp>.d) chained to the untouched bases; the next
reopen adopts base+delta and stays exact. A clean save (same watermark)
writes nothing. A big overlay compacts back into a fresh base and drops
the delta.

Run: python test_delta_append_sidecars.py <path-to-dbsp.duckdb_extension>
"""

import glob
import os
import signal
import sys
import tempfile

import duckdb

EXT = sys.argv[1] if len(sys.argv) > 1 else "build/dbsp.duckdb_extension"
TIMEOUT_S = 300
N_LEAVES = 20_000
N_OTHER = 5

os.environ["DBSP_SPILL_THRESHOLD_ROWS"] = "1000"  # force durable spilled baselines


def on_alarm(signum, frame):
    print("FAIL: timed out", flush=True)
    os._exit(1)


signal.signal(signal.SIGALRM, on_alarm)
signal.alarm(TIMEOUT_S)


def open_db(path):
    conn = duckdb.connect(path, config={"allow_unsigned_extensions": "true"})
    conn.execute(f"LOAD '{EXT}'")
    return conn


def drain_teardown():
    # close() returns while a DETACHED teardown thread may still be
    # saving; reopening the same db (same process) races it — file
    # mtimes/sizes move under the next session's adopt. Drain first.
    c = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    c.execute(f"LOAD '{EXT}'")
    c.execute("SELECT * FROM dbsp_wait_teardown()")
    c.close()


def view_rows(conn):
    return sorted(
        conn.execute("SELECT parent_id, dim_1, value FROM dbsp_query('rollup')").fetchall()
    )


def truth(conn):
    return sorted(
        conn.execute(
            "SELECT c.parent_id, l.dim_1, SUM(l.value) FROM li l "
            "JOIN closure c ON l.dim_0 = c.child_id GROUP BY 1, 2"
        ).fetchall()
    )


def mtimes(pats, spill_dir):
    out = {}
    for pat in pats:
        for p in glob.glob(os.path.join(spill_dir, pat)):
            out[p] = os.stat(p).st_mtime_ns
    return out


with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "model.duckdb")
    spill_dir = path + ".dbsp_spill"

    # Session 1: build, save (full bases), close.
    conn = open_db(path)
    conn.execute("CREATE TABLE li (dim_0 INTEGER, dim_1 INTEGER, value DOUBLE)")
    conn.execute(
        f"INSERT INTO li SELECT leaf, o, (leaf * 31 + o) % 997 + 0.25 "
        f"FROM range({N_LEAVES}) t(leaf) CROSS JOIN range({N_OTHER}) u(o)"
    )
    conn.execute("CREATE TABLE closure (child_id INTEGER, parent_id INTEGER)")
    conn.execute(f"INSERT INTO closure SELECT i, 100000 + i // 100 FROM range({N_LEAVES}) t(i)")
    conn.execute(
        "CREATE MATERIALIZED VIEW rollup AS "
        "SELECT c.parent_id, l.dim_1, SUM(l.value) AS value "
        "FROM li l JOIN closure c ON l.dim_0 = c.child_id GROUP BY 1, 2"
    )
    baseline = view_rows(conn)
    assert baseline == truth(conn), "initial build wrong"
    msg = conn.execute("SELECT * FROM dbsp_save()").fetchone()[0]
    assert "circuit checkpoint: 1 views" in msg, f"checkpoint missing: {msg}"
    conn.close()
    drain_teardown()

    idx_files = mtimes(["*.dbspill.idx"], spill_dir)
    assert idx_files, "expected baseline digest index sidecars"
    arr_files = mtimes(["sharr_*.flat"], spill_dir)
    assert arr_files, "expected shared-arrangement fingerprint sidecars"

    # Session 2: adopt, ONE edit, save -> delta files appear, bases untouched.
    conn = open_db(path)
    load_msg = conn.execute("SELECT * FROM dbsp_load()").fetchone()[0]
    print("load2:", load_msg, flush=True)
    assert "1 from checkpoint" in load_msg, f"fast path did not fire: {load_msg}"
    conn.execute("UPDATE li SET value = 5000.0 WHERE dim_0 = 5 AND dim_1 = 3")
    conn.execute("SELECT * FROM dbsp_save()")
    post_edit = view_rows(conn)
    assert post_edit == truth(conn), "post-edit view wrong"
    conn.close()
    drain_teardown()

    idx_after = mtimes(["*.dbspill.idx"], spill_dir)
    assert idx_after == idx_files, "dirty save must NOT rewrite the base digest index"
    deltas = mtimes(["*.dbspill.idx.d"], spill_dir)
    assert deltas, "dirty save must write a digest-index delta sidecar"
    arr_after = mtimes(["sharr_*.flat"], spill_dir)
    assert arr_after == arr_files, "dirty save must NOT rewrite arrangement bases"
    assert mtimes(["sharr_*.flat.d"], spill_dir), (
        "dirty save must write arrangement delta sidecars"
    )

    # Session 3: adopt base+delta chain; values exact; incremental edit OK.
    conn = open_db(path)
    load_msg = conn.execute("SELECT * FROM dbsp_load()").fetchone()[0]
    print("load3:", load_msg, flush=True)
    assert "1 from checkpoint" in load_msg, f"delta-chain reopen fell back: {load_msg}"
    assert view_rows(conn) == post_edit, "restored view differs after delta-chain adopt"
    conn.execute("UPDATE li SET value = 7000.0 WHERE dim_0 = 6 AND dim_1 = 2")
    assert view_rows(conn) == truth(conn), "incremental edit after delta adopt diverged"

    # Clean-save skip: saving twice at one watermark rewrites nothing.
    conn.execute("SELECT * FROM dbsp_save()")
    before = mtimes(["*.dbspill.idx", "*.dbspill.idx.d"], spill_dir)
    conn.execute("SELECT * FROM dbsp_save()")
    after = mtimes(["*.dbspill.idx", "*.dbspill.idx.d"], spill_dir)
    changed = {k for k in after if before.get(k) != after[k]} | (
        set(before) - set(after)
    )
    assert before == after, f"a clean save must not rewrite sidecars: {changed}"

    # Compaction: touch >10% of rows -> full fold, delta dropped for li.
    conn.execute("UPDATE li SET value = value + 1.0 WHERE dim_0 % 4 = 0")
    conn.execute("SELECT * FROM dbsp_save()")
    assert view_rows(conn) == truth(conn), "post-compaction view wrong"
    conn.close()
    drain_teardown()

    # Session 4: reopen after compaction stays exact.
    conn = open_db(path)
    load_msg = conn.execute("SELECT * FROM dbsp_load()").fetchone()[0]
    assert "1 from checkpoint" in load_msg, f"post-compaction reopen fell back: {load_msg}"
    assert view_rows(conn) == truth(conn), "post-compaction reopen differs"
    conn.close()
    drain_teardown()

print("PASS", flush=True)

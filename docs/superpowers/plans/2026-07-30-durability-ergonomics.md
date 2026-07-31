# Durability & View-Lifecycle Ergonomics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Auto-persist DBSP view state across connection reopens, add subtree-only view replacement, and extend circuit checkpointing to LEFT/RIGHT joins — spec: `docs/superpowers/specs/2026-07-30-durability-ergonomics-design.md`.

**Architecture:** All three features live in the duckDBSP extension (repo `/Users/ravi/Documents/Dev/duckDBSP`, header-heavy C++: `include/dbsp_cdc.hpp` CDCManager, `src/dbsp_extension.cpp` table functions + lifecycle, `include/dbsp_plan_translator.hpp` circuit nodes, `include/dbsp_parser_extension.hpp` CREATE MATERIALIZED VIEW parsing).

**Tech Stack:** C++17, DuckDB extension API (forked duckdb in-tree), Catch2 (`test/integration`, `test/unit`), python tests (`test/python`), CMake.

## Global Constraints

- Repo rules (`CLAUDE.md`): docs updated in the SAME commit as each feature (CHANGELOG.md + API.md + ARCHITECTURE.md + README.md as applicable); scratch files go to the session scratchpad, never the repo; `ctest` ONLY from `test/build_test`; never bare `make -j`/`cmake -j` — always `-j 4`.
- Build/test cycle: `cd test/build_test && make <target> -j 4` then run the binary, or `ctest` for the full suite (42 tests, ~55s). Loadable extension rebuild: `cd build && make -j 4`.
- Test DBs in tests use the existing harness (`test/test_helpers.hpp` `DuckDBTestHarness`; python tests follow `test/python/test_checkpoint_restore.py` patterns — they locate the extension via env/az fixed paths already in those files).
- Full ctest suite must be green at every task's commit. Each commit message ends with:
  Claude-Session: https://claude.ai/code/session_01TGZr1ZRcKV3jmWqoHMck8s
- Error identifiers follow `docs/ERROR_HANDLING.md` conventions (DBSP-Exxx codes); new user-facing errors get codes there.
- Checkpoint blob format change (Task 3) MUST bump the checkpoint format version so pre-change `_dbsp_ckpt` blobs are treated as absent (rebuild-by-replay), never misparsed. Find the version constant in `include/dbsp_checkpoint.hpp` / `_dbsp_ckpt_meta` handling; if none exists, add one to the meta table contract.

---

### Task 1: Auto-persist (`dbsp_autopersist`)

**Files:**
- Modify: `src/dbsp_extension.cpp` (new table function + auto-load trigger + shutdown hook; function registration block at ~line 1560; clean-shutdown crash-marker hook at ~line 1531)
- Modify: `include/dbsp_cdc.hpp` (autopersist flag + auto-load-once state on CDCManager; entry-point trigger helper)
- Modify: `include/dbsp_context_state.hpp` ONLY if the flag naturally belongs with other per-instance state — follow wherever `use_parallel_sync_` / auto-sync flags live today
- Test: `test/python/test_autopersist.py` (new)
- Docs: `CHANGELOG.md`, `docs/API.md`, `README.md` (same commit)

**Interfaces:**
- Consumes: existing `CDCManager::load_from_duck_table(context)` (`dbsp_cdc.hpp:771`), `save_to_duck_table` + `save_checkpoint` (`dbsp_cdc.hpp:455`), auto-sync flag pattern (`dbsp_auto_sync`, `src/dbsp_extension.cpp:992`).
- Produces: `dbsp_autopersist([BOOLEAN])` table function (set/query, same shape as `dbsp_auto_sync`); `CDCManager::maybe_autoload(context)` idempotent trigger; `CDCManager::autopersist_enabled()`.

- [ ] **Step 1: Write the failing python test**

`test/python/test_autopersist.py`, following the connect/load pattern of the existing python tests in that directory (copy their extension-path resolution verbatim):

```python
def test_views_survive_clean_reopen(tmp_path):
    db = str(tmp_path / "ap.duckdb")
    con = connect(db)  # helper from existing test file pattern
    con.execute("CREATE TABLE t (k INT, v DOUBLE)")
    con.execute("INSERT INTO t VALUES (1, 10.0), (2, 20.0)")
    con.execute("CREATE MATERIALIZED VIEW mv AS SELECT k, v * 2 AS d FROM t")
    assert con.sql("SELECT * FROM dbsp_query('mv') ORDER BY k").fetchall() == [(1, 20.0), (2, 40.0)]
    con.close()  # clean close -> auto-save fires

    con2 = connect(db)
    # No dbsp_load() call — first DBSP entry point auto-loads
    rows = con2.sql("SELECT * FROM dbsp_query('mv') ORDER BY k").fetchall()
    assert rows == [(1, 20.0), (2, 40.0)]
    # And it stays incremental after the reopen
    con2.execute("INSERT INTO t VALUES (3, 30.0)")
    rows = con2.sql("SELECT * FROM dbsp_query('mv') ORDER BY k").fetchall()
    assert rows == [(1, 20.0), (2, 40.0), (3, 60.0)]

def test_autopersist_off_is_honored(tmp_path):
    db = str(tmp_path / "off.duckdb")
    con = connect(db)
    con.execute("SELECT * FROM dbsp_autopersist(false)")
    con.execute("CREATE TABLE t (k INT)")
    con.execute("CREATE MATERIALIZED VIEW mv AS SELECT k FROM t")
    con.close()
    con2 = connect(db)
    res = con2.sql("SELECT * FROM dbsp_views()").fetchall()
    assert res == []  # nothing auto-loaded, nothing was saved
```

- [ ] **Step 2: Run it, watch both fail**

Run: `cd test/build_test && python3 -m pytest ../python/test_autopersist.py -x -q` (match how existing python tests are invoked — check `docs/TESTING.md`; if they run via ctest, register the same way).
Expected: FAIL — `dbsp_autopersist` unknown function / `dbsp_query('mv')` unknown view after reopen.

- [ ] **Step 3: Implement**

1. Flag on CDCManager next to the auto-sync flag: `std::atomic<bool> autopersist_{true};` + `bool autoload_attempted_ = false;`.
2. `dbsp_autopersist` table function: clone the `AutoSyncFunc`/bind at `src/dbsp_extension.cpp:992-1632` (set/query semantics, returns status string). Register alongside.
3. **Auto-load trigger:** add `void maybe_autoload(ClientContext&)` on CDCManager: if `autopersist_ && !autoload_attempted_ && views_.empty()`, set `autoload_attempted_ = true` and call the `dbsp_load()` internal path (`load_from_duck_table(context)`), swallowing an absent `_dbsp_views` silently (that path already returns success). Call `maybe_autoload` at the top of every user-facing entry that both has a ClientContext and reads/writes DBSP state — minimally: `dbsp_query`, `dbsp_views`, `dbsp_track`, `create_view` (both function and parser paths), `dbsp_sync`, `dbsp_changes`. Creating a view sets `autoload_attempted_ = true` too (never auto-load over a registry the user has started populating).
4. **Auto-save:** in the clean-shutdown hook (`src/dbsp_extension.cpp:~1531`, where the crash-marker lock is released), when `autopersist_` and the registry is non-empty: call `save_to_duck_table` + `save_checkpoint`. Mind what's safe at shutdown: the hook context there — mirror how the crash-marker code obtains its connection/instance; if no ClientContext is safely available at that point, document it and hook instead into the LAST connection close (wherever the crash-marker release found its hook — read that code first and follow it exactly).
5. `dbsp_autopersist_interval(N)` per spec: store N, count commits in `propagate_changes`, trigger `save_checkpoint` every N. Default 0 = off. (Small; same commit.)

- [ ] **Step 4: Run tests to green, run full ctest**

Run: the python test file, then `cd test/build_test && ctest` — all 42 (+ new) green.

- [ ] **Step 5: Docs + commit**

CHANGELOG entry; API.md sections for `dbsp_autopersist` / `dbsp_autopersist_interval` (note the two-call `dbsp_save`/`dbsp_load` contract is now optional, kept for compat); README feature bullet. Commit: `feat: autopersist — views survive clean reopen without explicit save/load`.

---

### Task 2: `dbsp_replace_view` + CREATE OR REPLACE

**Files:**
- Modify: `include/dbsp_cdc.hpp` (new `replace_view` on CDCManager; uses `DependencyGraph::get_all_dependents` at `dbsp_cdc.hpp:243`, existing drop-cascade machinery, `views_[...]->sql()` for DDL snapshot)
- Modify: `src/dbsp_extension.cpp` (table function `dbsp_replace_view(name, sql)`; registration)
- Modify: `include/dbsp_parser_extension.hpp` (accept `CREATE OR REPLACE MATERIALIZED VIEW` — parse at ~line 95-122 where `CREATE MATERIALIZED VIEW` is matched; route to replace path when the view exists)
- Test: `test/integration/test_cascading_views.cpp` (extend)
- Docs: `CHANGELOG.md`, `docs/API.md`, `docs/ERROR_HANDLING.md` (new error code), `README.md`

**Interfaces:**
- Consumes: Task-independent (registry + dep graph as of today).
- Produces: `CDCManager::replace_view(context, name, sql) -> bool` (+ `last_error_` report); SQL surface `SELECT * FROM dbsp_replace_view('name', 'SELECT ...')` and `CREATE OR REPLACE MATERIALIZED VIEW name AS <query>`.

- [ ] **Step 1: Write the failing tests**

Append to `test/integration/test_cascading_views.cpp`:

```cpp
TEST_CASE("Replace a mid-chain view rebuilds only its subtree",
          "[integration][cascade][replace]") {
    DuckDBTestHarness db;
    db.createTable("base", "k INT, v DOUBLE", {"(1, 1.0)", "(2, 2.0)"});
    db.exec("SELECT * FROM dbsp_track('base')");
    db.exec("SELECT * FROM dbsp_sync('base')");
    db.exec("CREATE MATERIALIZED VIEW lvl1 AS SELECT k, v * 2 AS d FROM base");
    db.exec("CREATE MATERIALIZED VIEW lvl2 AS SELECT k, d + 1 AS e FROM lvl1");

    // Replace lvl1: doubles become triples; lvl2's own DDL must survive
    db.exec("SELECT * FROM dbsp_replace_view('lvl1', "
            "'SELECT k, v * 3 AS d FROM base')");
    auto rows = db.query("SELECT e FROM dbsp_query('lvl2') WHERE k = 2");
    REQUIRE(rows->GetValue(0, 0).GetValue<double>() == 7.0); // 2*3+1

    // Still incremental afterwards
    db.exec("INSERT INTO base VALUES (3, 3.0)");
    db.exec("SELECT * FROM dbsp_sync('base')");
    rows = db.query("SELECT e FROM dbsp_query('lvl2') WHERE k = 3");
    REQUIRE(rows->GetValue(0, 0).GetValue<double>() == 10.0); // 3*3+1
}

TEST_CASE("CREATE OR REPLACE MATERIALIZED VIEW maps to replace",
          "[integration][cascade][replace]") {
    DuckDBTestHarness db;
    db.createTable("base", "k INT", {"(1)"});
    db.exec("CREATE OR REPLACE MATERIALIZED VIEW mv AS SELECT k FROM base");     // create
    db.exec("CREATE OR REPLACE MATERIALIZED VIEW mv AS SELECT k + 1 AS k FROM base"); // replace
    auto rows = db.query("SELECT k FROM dbsp_query('mv')");
    REQUIRE(rows->GetValue(0, 0).GetValue<int32_t>() == 2);
}

TEST_CASE("Replace with bad SQL reports and leaves registry queryable",
          "[integration][cascade][replace]") {
    DuckDBTestHarness db;
    db.createTable("base", "k INT", {"(1)"});
    db.exec("CREATE MATERIALIZED VIEW good AS SELECT k FROM base");
    auto result = db.query("SELECT * FROM dbsp_replace_view('good', "
                           "'SELECT nonexistent_col FROM base')");
    // The call surfaces an error (either result error or error-status row)
    // and the registry remains usable afterwards:
    auto views = db.query("SELECT * FROM dbsp_views()");
    REQUIRE_FALSE(views->HasError());
}
```

- [ ] **Step 2: Run to watch fail** (unknown function / parser error on OR REPLACE)

Run: `cd test/build_test && make test_cascading_views -j 4 && ./test_cascading_views "[replace]"`

- [ ] **Step 3: Implement `CDCManager::replace_view`**

Algorithm (spec §2): verify exists → snapshot `(name, sql)` of `get_all_dependents(name)` in topological order (use the dep graph's existing topo helper `topological_order`, filter to dependents) → drop subtree via existing cascade path → `create_view(name, new_sql)` → recreate dependents in saved order. On mid-sequence failure: continue best-effort with remaining old DDL, accumulate a report into `last_error_`, return false. Registry lock discipline: `create_view` takes `struct_mutex_` itself (see `load_from_duck_table`'s comment `dbsp_cdc.hpp:766` — do NOT hold it across the whole replace; snapshot under shared lock, then drop/create unlocked exactly as `dbsp_load` does).

- [ ] **Step 4: Parser extension OR REPLACE**

At the `CREATE MATERIALIZED VIEW` prefix match (`dbsp_parser_extension.hpp:~95`), also accept `CREATE OR REPLACE MATERIALIZED VIEW`; thread a `replace` flag through the parse data to wherever the create executes, and route to `replace_view` when the flag is set AND the name exists.

- [ ] **Step 5: Green + full ctest + docs + commit**

`ctest` all green. API.md (`dbsp_replace_view`, OR REPLACE), ERROR_HANDLING.md (new code for unknown-view + partial-failure report), CHANGELOG, README. Commit: `feat: dbsp_replace_view + CREATE OR REPLACE — subtree-only repopulation`.

---

### Task 3: LEFT/RIGHT join checkpoint state

**Files:**
- Modify: `include/dbsp_plan_translator.hpp` — `PlanJoinNode::state_kind()` (~line 2561), `serialize_state` (~2569), `restore_state` (~2592)
- Modify: checkpoint format version (find in `include/dbsp_checkpoint.hpp` / `_dbsp_ckpt_meta` writer in `dbsp_cdc.hpp::save_checkpoint`; add one if absent per Global Constraints)
- Test: `test/unit/` new file `test_join_checkpoint.cpp` (or extend the existing checkpoint test file if one covers PlanJoinNode — check `test/python/test_checkpoint_restore.py` and unit tests first; put C++ round-trip tests wherever D3b's join-state tests live today)
- Docs: `CHANGELOG.md`, `docs/ARCHITECTURE.md` (state-kind table), `docs/API.md` if it documents checkpoint coverage

**Interfaces:**
- Consumes: existing BlobWriter/BlobReader (`w.u64/i64/row`, `r.u64/i64/row`), existing serialize shape (left_index_, right_index_, left_weights_, right_weights_ already written — see `plan_translator.hpp:2569`).
- Produces: LEFT/RIGHT (non-spilled, non-mark) joins report `SERIALIZABLE`; blob format extends with pad state + totals; format version bump invalidates old checkpoints cleanly.

- [ ] **Step 1: Write the failing round-trip test**

Test principle (exact harness per whatever D3b tests use — read them first): **checkpointed-and-restored view must behave byte-identically to a continuously-live twin under post-restore deltas**, exercising both pad transitions:

```cpp
TEST_CASE("LEFT join view checkpoint round-trips pad state",
          "[unit][checkpoint][join]") {
    // twin A: live throughout; twin B: save -> teardown -> restore
    // Schema: left(k INT, a INT), right(k INT, b INT)
    // View: SELECT l.k, l.a, r.b FROM left l LEFT JOIN right r ON l.k = r.k
    // Seed: left {(1,10),(2,20)}, right {(1,100)}   -- row 2 is NULL-padded
    // Checkpoint B. Restore B.
    // Post-restore delta 1: INSERT right (2,200)  -- row 2 gains first match
    //   (pad must retract)
    // Post-restore delta 2: DELETE right k=1      -- row 1 loses last match
    //   (pad must appear)
    // After each delta: REQUIRE(B result == A result), exact rows+weights.
}
```

State_kind gate test in the same file: a LEFT-join view reports checkpointable; a FULL-join view still does not; a spilled LEFT join still does not.

- [ ] **Step 2: Watch it fail** (LEFT join view currently `checkpointable() == false`, so the save path skips it / restore rebuilds — the twin-equality assertions never even exercise restore; assert on `checkpointable()` first so the failure is crisp)

- [ ] **Step 3: Implement**

1. `state_kind()`: permit `JoinType::LEFT` and `JoinType::RIGHT` when `!marks_ && !local_spill_left_ && !local_spill_right_` (FULL stays out).
2. `serialize_state`: append `left_pad_`, `right_pad_` (same RowWeights writer), `right_total_`, `right_null_` (and their left-side analogs if the shared-right bookkeeping fields at `plan_translator.hpp:1768-1784` need them — read `reconcile_pads` and serialize EXACTLY the fields it consumes; nothing more).
3. `restore_state`: mirror. Reject blobs that end early (BlobReader throws → return false → rebuild).
4. Bump checkpoint format version so pre-change blobs invalidate (Global Constraints).

- [ ] **Step 4: Green + full ctest**

New tests pass; `ctest` 42+ green (crash-recovery and persistence suites are the regression risk — they exercise checkpoint blobs end-to-end).

- [ ] **Step 5: Docs + commit**

ARCHITECTURE state-kind table gains LEFT/RIGHT; CHANGELOG notes the format bump ("old checkpoints rebuild once"). Commit: `feat: checkpoint LEFT/RIGHT join state — pad bookkeeping serialized`.

---

### Task 4: End-to-end validation against the NumPad spike

**Files:** none in this repo committed (validation only; scratchpad artifacts). NumPad-side memory/doc updates happen OUTSIDE this plan.

- [ ] **Step 1: Rebuild loadable extension** — `cd build && make -j 4`.
- [ ] **Step 2: wfp cold-vs-restore measurement**

Scratchpad script (pattern: NumPad `scripts/spike_full_sql` + the timing harness from this session): fresh `spike50.duckdb` copy → create 43 views (cold ≈ 6.2s) → clean close (auto-save fires) → reopen → first `dbsp_query` triggers auto-load → measure wall-clock to views-ready and verify parity (`parity.check(con, 'oracle') == []` with the NumPad venv). Record: restore time, which views restored from checkpoint vs rebuilt (the restore path exposes counts — `dbsp_cdc.hpp:888` region tracks restored-vs-replayed; surface via existing stats/log). Expected shape: LEFT-join-only views restore; recursive/window views still rebuild (~1.9s of the head remains). Numbers go in the final report, not committed anywhere in duckDBSP.
- [ ] **Step 3: Full ctest one last time; confirm clean `git status`.**

---

## Self-review notes

- Spec coverage: F1 → Task 1 (incl. interval setting), F2 → Task 2 (incl. OR REPLACE + failure semantics), F3 → Task 3 (LEFT/RIGHT only, format bump), spec Testing section → per-task Step 1s + Task 4; spec Documentation section → folded into each task's final step per repo CLAUDE.md.
- Placeholders: Task 1 Step 3.4 intentionally defers ONE decision to code-reading ("mirror how the crash-marker hook obtains its instance") because the answer lives in code the implementer must read anyway; the fallback instruction (hook last-connection-close instead) is concrete.
- Type consistency: `maybe_autoload(ClientContext&)` / `autopersist_enabled()` / `replace_view(context, name, sql) -> bool` used consistently; serialize field list defined by `reconcile_pads`'s actual reads, stated as the authority.

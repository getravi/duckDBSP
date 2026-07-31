# Cold/Restore Attack Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut wfp cold populate (15.5s) and restore (~7.0s) — spec: `docs/superpowers/specs/2026-07-31-cold-restore-attack-design.md`.

**Architecture:** duckDBSP repo (`include/dbsp_plan_translator.hpp`, `include/dbsp_cdc.hpp`, `include/dbsp_checkpoint.hpp`, spill codec in whatever header the CHANGELOG's "spill row codec" lives) + a NumPad-side view revert and final measurement.

## Global Constraints

- Repo rules as always: docs same commit; `ctest` only from `test/build_test`; `make <target> -j 4` never bare; scratch to session scratchpad; full ctest green per commit; commit trailer:
  Claude-Session: https://claude.ai/code/session_01TGZr1ZRcKV3jmWqoHMck8s
- Any checkpoint blob layout change bumps `kDbspCkptFormatVersion` (currently 3) so old checkpoints rebuild-by-replay, never misparse.
- Differential correctness is the bar everywhere: restored/rewritten views must match a continuously-live twin / stock DuckDB exactly.

---

### Task 1: Window/LAG constant folding + truthful load report

**Files:** `include/dbsp_plan_translator.hpp` (`constant_int` ~line 5290 region; used at LAG offset ~5321 and frame bounds ~5377/5383); `include/dbsp_cdc.hpp` (`load_from_duck_table` report string); tests in `test/integration/test_advanced_window.cpp` (follow its harness) + a load-report assertion where the autopersist python test lives; docs: CHANGELOG, API.md window paragraph.

- [ ] **Step 1 (RED):** tests: (a) `CREATE MATERIALIZED VIEW ... AVG(...) OVER (... ROWS BETWEEN 11 PRECEDING AND CURRENT ROW)` creates as a true MV (no DBSP-E110) and matches stock DuckDB before AND after an UPDATE that changes a mid-window value (exact values); (b) same for `LAG(v, 12)`; (c) `LAG(v)` still works; (d) python: second `dbsp_load()` on a loaded registry does not claim "0 from checkpoint" (assert on the new wording).
- [ ] **Step 2:** watch (a)/(b)/(d) fail.
- [ ] **Step 3:** fix `constant_int`: unwrap `BOUND_CAST` chains (loop on `ExpressionClass::BOUND_CAST` → child) before the `BOUND_CONSTANT` check; if DuckDB offers a cheap foldable-scalar helper usable without a ClientContext, prefer it — otherwise cast-unwrap is enough (verify empirically which shell the binder produces for `11 PRECEDING`; print `ExpressionClassToString` in a scratch probe first so the fix targets the real shape, not a guess). Fix the load report: when the registry is already populated, skip reload and report "already loaded (N views live)" — or if reload-over-populated is load-bearing behavior somewhere (check callers/tests), keep behavior and fix only the wording to state what actually happened.
- [ ] **Step 4:** green + full ctest + docs + commit (`feat: accept constant window frames and LAG/LEAD offsets; truthful dbsp_load report`).

### Task 2: MIN/MAX checkpoint state

**Files:** `include/dbsp_plan_translator.hpp` `PlanAggregateNode` (`state_kind()` ~1324 — scalar_fn gate; serialize/restore ~1337+); `include/dbsp_checkpoint.hpp` (version 3→4); tests in the file holding aggregate/checkpoint round-trips (find D3b's; else extend `test/integration/test_join_checkpoint.cpp` — it already has the twin-round-trip harness); docs: CHANGELOG, ARCHITECTURE state-kind table.

- [ ] **Step 1 (RED):** state_kind gate test (MIN/MAX non-DISTINCT view reports checkpointable; DISTINCT/MEDIAN still not) + twin round-trip: group's MAX retreats after restore (delete current max), advances (insert new max), exact equality vs live twin.
- [ ] **Step 2:** watch fail. **Step 3:** serialize the per-group value-collecting structure MIN/MAX keeps for retractions (read the operator's actual state fields first — serialize exactly what the maintenance path reads, mirroring how join pads were done in `7512fd4`). Version bump 3→4. **Step 4:** green + full ctest (persistence/crash suites are the net) + docs + commit.

### Task 3: Fast typed codec for checkpoint blobs

**Files:** wherever the spill fast codec lives (grep CHANGELOG "spill row codec" → the header with the tag-byte codec) + `include/dbsp_cdc.hpp` checkpoint blob write/read (`BlobWriter`/`BlobReader` row paths used by `save_checkpoint`/`restore_view_state` and node serialize helpers); `include/dbsp_checkpoint.hpp` (version 4→5); docs: CHANGELOG.

- [ ] **Step 1 (RED):** a round-trip test over a view whose rows cover the codec's value shapes (INTEGER/BIGINT/DOUBLE/VARCHAR/BOOLEAN/typed NULLs + one nested/fallback type) asserting exact restore equality — plus a timing probe (scratchpad, not asserted in ctest) recording decode ms on a 58k-row sink blob before/after.
- [ ] **Step 2-3:** wire the typed codec into the checkpoint row read/write paths (shared helper, no duplication — if the spill codec is file-local, lift it to a shared header). Version bump. Fallback to the generic path for shapes the codec doesn't cover (same rule the spill path uses).
- [ ] **Step 4:** green + full ctest + docs + commit; report the measured decode speedup.

### Task 4: NumPad revert + full measurement (no duckDBSP commits)

- [ ] Rebuild loadable extension (`build/ && make -j 4`).
- [ ] NumPad `scripts/spike_full_sql/views.py`: revert `v_rolling12mavgheadcount` and `v_companyannualizedattrition` to the native bounded-window forms (git history of the file has the originals; keep the LNB join rewrites — MAX now checkpoints, windows don't, that trade was accepted in the spec). Run: create views → DECLINED must stay 0 → parity clean.
- [ ] Measure and record (scratchpad + report): cold populate (target: well under 15.5s — the 3.8s range-join hotspot should collapse), clean-close → reopen restore time and restored-view count (target: materially under 7.0s, restored > 29), 8-edit probe wall times, full 300-employee driver run (p25 both scenarios both arms + parity all clean).
- [ ] NumPad commit: views.py revert + spike-spec addendum with the final table (controller reviews before commit).

## Self-review notes

- Version bumps: two (v4 aggregates, v5 codec) — both invalidate-clean by the existing version gate; acceptable double rebuild for pre-existing checkpoints.
- Task 1's load-report fix intentionally offers two shapes (skip vs reword) gated on reading the callers — not a placeholder, a documented decision point with the default named (skip+report if nothing depends on reload-over-populated).
- Task 4 depends on 1-3 only through the rebuilt extension; ordering is strict.

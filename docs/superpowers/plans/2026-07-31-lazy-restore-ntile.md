# Lazy Restore + NTILE Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ms-scale reopen via lazy per-view restore; NTILE correct + ungated — spec: `docs/superpowers/specs/2026-07-31-lazy-restore-ntile-design.md`.

## Global Constraints

- **Build protocol (binding, per user):** Tasks 1-2 NO builds/tests; ONE batched cycle in Task 3 (user already authorized "a build only once at the end" — no further ask needed unless a second iteration is required, which DOES need an ask).
- No checkpoint format change (stash/re-save = existing v5 bytes).
- Docs same commit; trailer: Claude-Session: https://claude.ai/code/session_01TGZr1ZRcKV3jmWqoHMck8s
- Commit bodies note "Untested-by-build: verification deferred to the batched cycle."

### Task 1: Lazy per-view restore

Files: include/dbsp_cdc.hpp (load_from_duck_table checkpoint fast path; new pending_restore_ map + realize_pending_view; call sites: QueryFunc/dbsp_query path in src/dbsp_extension.cpp, propagate_changes/step_view, save_checkpoint verbatim re-save, replace_view/drop paths, arrangement initial fill for view-named sources — AUDIT get_result/get_delta consumers on freshly-loaded views and list each in the report with its realize-or-safe verdict), src/dbsp_extension.cpp (dbsp_lazy_restore setting fn, load report wording), tests in test/integration or test/python following the autopersist patterns (lazy: reopen + single-view touch decodes one chain [assert via a decode counter or the report string]; delta-before-query realizes first, differential vs an eager twin; partial-realize then save then reopen round-trips; lazy_restore(false) = eager behavior), CHANGELOG/API/ARCHITECTURE.

Mirror materialize_deferred_locked's locking discipline per call site. Realization must be idempotent + thread-safe (two connections racing first touch).

### Task 2: NTILE fix + ungate

Files: include/dbsp_window_view.hpp (NTILE bucket math — stock semantics: first p%n buckets get p/n+1 rows), include/dbsp_plan_translator.hpp (NTILE from bare_constant_int to constant_int; NTH_VALUE only if differentially verified — else document staying gated), test/integration/test_advanced_window.cpp (differential vs stock: p%n==0, p%n!=0, n>p, single-row partitions, post-edit re-render), API.md/TODO.md/CHANGELOG.

### Task 3: Batched cycle (controller)

Full build + ctest + extension rebuild + wfp probes: reopen wall (expect ms-scale), single-view first-touch, full-touch parity, NTILE spot checks. Fixes need a fresh user ask if a second build is required. Then spec addendum (NumPad), memory, push on request.

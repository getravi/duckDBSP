# Restore Tail Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 43/43 wfp views restore from checkpoint — spec: `docs/superpowers/specs/2026-07-31-restore-tail-design.md`.

## Global Constraints

- **Build protocol (binding, per user):** Tasks 1-2 run NO builds at all — code + tests written and statically reviewed only. ALL building (single-target, full suite, extension) happens in Task 3 as one batched cycle after user sign-off. Expect the first build to surface issues from both features; budget one fix iteration.
- Format version 4 → 5 exactly once (Task 1 bumps it; Task 2 does NOT bump again).
- Docs in the same commit as each feature; commit trailer:
  Claude-Session: https://claude.ai/code/session_01TGZr1ZRcKV3jmWqoHMck8s
- Serialize-what-the-resume-path-reads discipline, justified per field in the report (precedents: join pads 7512fd4, MIN/MAX e6a6b57).

### Task 1: PlanRecursiveNode checkpoint state (+ v5 bump)

Files: `include/dbsp_plan_translator.hpp` (PlanRecursiveNode ~3133+: fields `accumulated_`, `anchor_total_`, `base_totals_`, nested `step_view_` [a PlannedCircuitView with existing `serialize_circuit_state`/`restore_circuit_state`/`checkpointable`]), `include/dbsp_checkpoint.hpp` (v5), `test/integration/test_join_checkpoint.cpp`, CHANGELOG, ARCHITECTURE.

Steps: RED twin round-trip tests (post-restore mid-chain retraction resumes the linear signed-delta path from restored state; post-restore fallback-to-recompute also correct; UNION recursion still not checkpointable) → implement (no build; note in the report which failures you EXPECT the deferred RED run to show) → `state_kind` (SERIALIZABLE iff `union_all_ && step_view_->checkpointable()`), serialize/restore incl. nested circuit sub-blob → docs → commit (untested-by-build, stated in the commit body). 

### Task 2: NativeWindowView checkpoint state via EmbeddedViewNode

Files: `include/dbsp_plan_translator.hpp` (EmbeddedViewNode ~2820+), `include/dbsp_window_view.hpp` (state fields — read `apply_changes` incremental + structural paths to derive the list), `test/integration/test_join_checkpoint.cpp`, CHANGELOG, ARCHITECTURE.

Steps: RED twin round-trips (post-restore fast-path edit; post-restore structural delta; sort/limit embedded views still not checkpointable) → implement `state_kind` delegation + serialize/restore on NativeWindowView only (no build) → docs → commit (untested-by-build, stated in the commit body). NO version bump (v5 already covers this plan's layout changes — Task 1 landed it).

### Task 3: Batched verification + wfp measurement (USER SIGN-OFF REQUIRED before builds)

Controller confirms with the user, then: full-suite rebuild + ctest (43 green), loadable extension rebuild, wfp probe (fresh spike300 copy → create → save → reopen: expect 43/43 blob_decode, reopen time recorded vs 3.2s baseline), parity clean, spec-doc addendum (controller), memory, push on user request.

## Self-review notes

Version-bump-once split across tasks is explicit; build protocol overrides the usual per-commit full-ctest rule by design (user constraint) — the final batched ctest is the gate for the whole range.

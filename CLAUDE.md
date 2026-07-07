# duckDBSP Project Rules

## Documentation Discipline

After completing each major task (feature, phase milestone, architectural change,
significant bug fix), update documentation in the same commit or an immediate
follow-up commit:

1. **Docs** — relevant files in `docs/` (`ARCHITECTURE.md`, `API.md`, `THEORY.md`,
   `ERROR_HANDLING.md`, `TESTING.md`). Remove or correct anything the change
   made stale.
2. **Code comments** — file/class header comments in touched files must match
   new behavior. Delete comments the change invalidated.
3. **README.md** — feature list, version badges, usage examples, build
   instructions (including pinned DuckDB version).
4. **Architecture diagram** — the diagram in `docs/ARCHITECTURE.md` must reflect
   current components and data flow. If a component was added, removed, or
   rewired (e.g. a view type migrated to the circuit IR), redraw it.

"Major task" = anything you'd mention in a changelog entry. Skip for typos,
comment-only edits, or test-only fixes that don't change behavior.

Do not mark a major task complete until this checklist is done.

## Scratch & Test Artifact Hygiene

Never accumulate throwaway files. Rules:

1. **Fixed names, overwrite in place** — scratch scripts, profiler
   samples, probe outputs reuse the same filename every time
   (`bench_sample.txt`, not `bench_sample_<date>.txt`).
2. **Scratch goes in the session scratchpad dir**, never `/tmp` or the
   repo. Anything that must live in the repo temporarily is deleted in
   the same session.
3. **ctest only from `test/build_test`** — running it elsewhere plants a
   junk `Testing/` directory.
4. **Sanitizer build dirs (`test/build_asan`, `test/build_tsan`) are
   disposable** — recreate on demand, delete when disk is tight; never
   treat their absence as breakage.
5. Runtime spill directories are self-cleaning (dbsp_spill(true) sweeps
   directories left by dead processes) — don't add manual cleanup steps
   for them.

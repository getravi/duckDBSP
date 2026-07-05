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

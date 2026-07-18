# Internal note: DuckDB storage-layer modification hooks

Status: LOCAL ONLY. Nothing here has been (or should be) posted to the
DuckDB project without the repo owner doing so personally. This records
the hook design validated against v1.5.4 sources so it is not lost —
usable as a future fork patch spec or, if the owner ever chooses,
upstream proposal material. Companion: `docs/DESIGN_ENGINE_HOOK.md`.

---

## Proposal: extension-registerable modification hooks on DataTable

**Ask**: a small, optional callback surface that lets extensions observe
table mutations exactly, at the storage layer, before they are applied.

**Motivation**: incremental computation over DuckDB tables — materialized
view maintenance, CDC export, replication, cache invalidation — needs the
per-statement delta: which rows were appended, deleted, or updated, with
old images for the latter two. Today no extension surface provides this.
DuckDB's own roadmap lists Materialized Views as future work; this
primitive is exactly what an eager incremental implementation needs, and
an extension ecosystem could build on it immediately (we have a working
DBSP-based incremental view extension that currently reconstructs deltas
through pre-image SELECT probes and optimizer-injected plan tees — it
works, but it re-derives inside the extension what the storage layer
already knows).

**Design sketch** (validated against v1.5.4 sources):

```cpp
// duckdb/storage/table/modification_hook.hpp
struct ModificationHook {
  // rows about to be appended (table column order)
  std::function<void(ClientContext &, DataTable &, DataChunk &)> on_append;
  // rows about to be deleted; old_rows holds their current images
  std::function<void(ClientContext &, DataTable &, Vector &row_ids,
                     idx_t count, DataChunk &old_rows)> on_delete;
  // rows about to be updated in place; updates holds new values for
  // `columns`, old_rows their current images
  std::function<void(ClientContext &, DataTable &, Vector &row_ids,
                     idx_t count, DataChunk &old_rows,
                     const vector<PhysicalIndex> &columns,
                     DataChunk &updates)> on_update;
  // a mutation the surface cannot describe (e.g. struct-field
  // UpdateColumn) — consumers must degrade for this table
  std::function<void(ClientContext &, DataTable &)> on_unknown;
  static void Register(DBConfig &config, shared_ptr<ModificationHook>);
};
```

Call sites (four, all in `DataTable`):

- `DataTable::LocalAppend(LocalAppendState&, ...)` — the single append
  chokepoint (INSERT, Appender, all overloads funnel here) — fire
  `on_append` with the verified chunk before `LocalStorage::Append`.
- `DataTable::Delete` — inside the existing local/global batching loop,
  fetch the batch's current images (`LocalStorage::FetchChunk` /
  `DataTable::Fetch` — rows are still visible at this point) and fire
  `on_delete` before performing the batch.
- `DataTable::Update` — in both the local and global slices, fetch old
  images and fire `on_update` before applying.
- `DataTable::UpdateColumn` — fire `on_unknown`.

With no hooks registered, each site is one empty-vector check. Old-image
fetches happen only when a hook is present. `update_is_del_and_insert`
plans need no special handling: they decompose into Delete + LocalAppend
calls the hooks see individually, and the composition is the correct
delta.

**Semantics**:

- Hooks fire during statement execution inside the owning transaction;
  consumers see per-transaction deltas and handle commit/rollback via
  the existing `ClientContextState` transaction callbacks.
- Repeated updates of one row in a transaction telescope naturally
  (each on_update's old image is the previous new image).
- No WAL/replay invocations: replay paths do not traverse these methods
  with a registered-config context in a way consumers observe before
  extension load.

**What this replaces for us** (evidence the primitive is the right one):
our extension currently achieves exact O(Δ) view maintenance for every
plain-SQL DML shape via ~1,500 lines of pre-image SELECT probes with
commit guards plus an OptimizerExtension that widens DML child plans and
injects a pass-through operator recording processed rows. All of it is a
reconstruction of information these four call sites hold natively, and
all of it depends on undocumented plan-shape internals that shift across
releases. The hook surface above collapses that to a ~150-line consumer.

**Costs**: ~300 lines, one new header, one modified file, zero overhead
when unregistered, no behavior change for existing users.


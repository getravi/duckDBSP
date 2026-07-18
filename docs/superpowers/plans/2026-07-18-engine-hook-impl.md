# Engine-hook implementation plan (SaaS-only fork)

Status: WIP 2026-07-18. Implements `docs/DESIGN_ENGINE_HOOK.md`
(`TransactionModificationCallback`) for the **NumPad SaaS backend only** —
not distributed, not self-hosted, so the engine build is a single controlled
target (one Docker image). Companion perf project: `docs/DESIGN_DATA_PLANE.md`.

## Why this is cheap here

The extension already builds DuckDB from source (`duckdb/` submodule) and pins
v1.5.4. Because the runtime is a single SaaS image we control:
- No wheel matrix / PyPI / customer supply-chain — **one** patched wheel for
  the server platform, baked into the backend image.
- Dual-mode is only for local dev + a CI stock-mode differential; **prod always
  runs the fork**, so the ~1,500-line capture stack can eventually be DELETED
  (not just demoted).
- Rebases are self-scheduled (bump DuckDB when we choose).

## The patch (branch `v1.5.4-dbsp`, one commit → `patches/v1.5.4-dbsp-txn-callback.patch`)

Generate the patch by applying A1–A3 to the submodule and
`git -C duckdb diff > patches/v1.5.4-dbsp-txn-callback.patch`. Keep it ONE
commit; any second engine change is a rebase liability.

### A1. Surface — `src/include/duckdb/main/config.hpp` (CONCRETE)

> **Signature change during A3 (2026-07-18):** the callback receives
> `DataTableInfo &` (schema/table names), NOT `TableCatalogEntry &`. There is
> no `DataTable → TableCatalogEntry` pointer in the engine; resolving the
> entry by catalog lookup inside `Commit` is fragile (a table CREATEd in the
> same txn is invisible post-stamp because its catalog entry is stamped with
> `commit_id > start_time`; dropped entries throw; extra locking). The
> consumer only needs the table name; column types travel in
> `TransactionModifications::types`.

```cpp
struct TransactionModifications;  // defined in undo_buffer.hpp
struct TransactionModificationCallback {
  // Fired in DuckTransaction::Commit after undo_buffer.Commit succeeds and
  // before FlushCommit — snapshot + version info still valid. One call per
  // modified table; `mods` streams old images (delete/update) and new images
  // (insert/update), full-width, in table column order.
  std::function<void(ClientContext &, DataTableInfo &,
                     TransactionModifications &)> on_commit;
};
// in struct DBConfig (near the other registries):
vector<TransactionModificationCallback> transaction_modification_callbacks;
void RegisterTxnModificationCallback(TransactionModificationCallback cb) {
  transaction_modification_callbacks.push_back(std::move(cb));
}
```

### A2. Call site — `src/transaction/duck_transaction.cpp` `Commit` (~L267) (CONCRETE)

```cpp
   storage->Commit(commit_state.get());
   undo_buffer.Commit(iterator_state, commit_info);
+  auto &cbs =
+      DBConfig::GetConfig(db.GetDatabase()).transaction_modification_callbacks;
+  if (!cbs.empty()) {
+    undo_buffer.StreamModifications(cbs);   // new — see A3
+  }
   if (commit_state) {
     commit_state->FlushCommit();
   }
```

**Open compile detail:** `StreamModifications` needs a `ClientContext` for
`DataTable::Fetch` on the untouched columns of an UPDATE. `DuckTransaction`
already holds the context it was constructed with (`undo_buffer(transaction,
context)`); thread that through `StreamModifications` (add a `context_` member
to `UndoBuffer` or pass `DuckTransaction`'s context). Resolve when writing A3.

### A3. Walk — `UndoBuffer::StreamModifications` — **DONE 2026-07-18** (differential tests: `test/unit/test_engine_hook.cpp`, 11 cases)

Implementation notes (how it actually works — supersedes the sketch below):
- **Timing is the trick.** The hook runs *after* `undo_buffer.Commit` has stamped
  every undo entry with `commit_id`. For the committing transaction's own
  snapshot (`start_time S`, `transaction_id T`): `commit_id > S && commit_id != T`,
  so its stamped deletes read as *another, future* transaction's deletes (row
  still visible) and stamped update chains resolve to the *old* versions.
  Hence **old images = plain transactional `DataTable::Fetch`** with the
  committing transaction — no undo-payload reassembly, no per-column merging.
  Conversely its own inserts read as future-committed (invisible) on that path,
  so **new images = `DataTable::FetchCommitted`** (committed reader,
  `TransactionData(MAX_TRANSACTION_ID, TRANSACTION_ID_START-1)`), which sees
  stamped inserts + base (new) update values and drops same-txn-deleted rows.
- Pass 1 walks the entries (same `IterateEntries` as `WriteToWAL`) collecting
  per-`DataTable*` row ids: `AppendInfo` ranges; `DeleteInfo` ids
  (`base_row + i` if `is_consecutive` else `base_row + rows[i]`); `UpdateInfo`
  ids (`row_group_start + vector_index*STANDARD_VECTOR_SIZE + tuples[i]`).
- Net-effect rules applied in code: update ids deduped (one `UpdateInfo` per
  column per vector); updated∩deleted removed (delete pre-image is the whole
  story — the transactional fetch would return a pre-image for these too, so
  fetch-time filtering can NOT handle it); updated∩own-insert-range removed
  (insert-range committed fetch already carries final values). insert∩delete
  nets to zero automatically (both fetch paths drop the row).
- The context comes from `transaction.context.lock()` (the pattern
  `CommitState` already uses); bail out if expired. Temporary tables skipped,
  same as the WAL. Tables with no surviving images fire no callback.
- Callback payload: `DataTableInfo&` (names) + `TransactionModifications`
  {types, old_rows CDC (weight −1), new_rows CDC (weight +1)} defined in
  `undo_buffer.hpp`.

Original scaffold sketch (historical):

```cpp
// Route per-table modifications to callbacks. This is a PORT of the existing
// WriteToWAL() traversal in this file — same UndoFlags entries, same
// DataTable / UpdateInfo / DeleteInfo extraction — differing only in the sink
// (callbacks instead of the WAL). Do NOT invent a new undo walk.
void UndoBuffer::StreamModifications(
    const vector<TransactionModificationCallback> &cbs) {
  // Accumulate per TableCatalogEntry* a TransactionModifications:
  //   INSERT_TUPLE -> new-image rows            (LocalStorage appended rows)
  //   DELETE_TUPLE -> old-image rows by rowid   (DataTable::Fetch, pre-image)
  //   UPDATE_TUPLE -> old image (undo stores changed cols only; Fetch the
  //                   untouched cols) + new image (current in-txn versions)
  // Assemble DataChunks full-width in table column order.
  // TODO(dbsp): copy the per-entry body from WriteToWAL() (same file) and
  //   swap the WAL serializer for `for (auto &cb : cbs) cb.on_commit(ctx,
  //   table_entry, mods);`. ~1-2 days; the extraction already exists.
}
```

A compilable no-op body (empty) is a SAFE intermediate: the engine builds, the
hook exists but emits nothing, the extension consumer registers but receives
nothing and stays on the fallback — so the patch can land before A3 is filled
in without breaking anything.

## Build glue

### CI — single-target patched wheel — `.github/workflows/engine-wheel.yml` (COMPLETE)

See the committed workflow. Manual-dispatch / `engine-*` tag only, so normal
pushes never trigger it. Applies the patch to the pinned submodule and builds
one wheel for the server platform; uploads it as an artifact.

### Backend image (in the NumPad backend repo, NOT here)

```dockerfile
# was: RUN pip install duckdb==1.5.4
COPY wheels/duckdb-1.5.4+dbsp-*.whl /tmp/
RUN pip install /tmp/duckdb-1.5.4+dbsp-*.whl && \
    python -c "import duckdb; assert 'dbsp' in duckdb.__version__"
```

Local dev / CI stock mode keeps stock `duckdb==1.5.4`; the extension's
dual-mode (below) runs the capture path there.

## Dual-mode consumer (extension) — CONCRETE shape

```cpp
inline bool register_engine_hook(duckdb::DatabaseInstance &db) {
#ifdef DBSP_ENGINE_HOOK               // defined only by the patched-engine build
  auto &cfg = duckdb::DBConfig::GetConfig(db);
  cfg.RegisterTxnModificationCallback({[](duckdb::ClientContext &ctx,
        duckdb::TableCatalogEntry &tbl,
        duckdb::TransactionModifications &m) {
    // chunks -> per-txn signed TxnCapture (old -1 / new +1 / append +1),
    // then apply_captured_delta(tbl.name, capture) — the existing ingestion
    // path every tier already uses. No guards: the engine reported facts.
    ingest_engine_modifications(ctx, tbl, m);
  }});
  return true;
#else
  (void)db;
  return false;                       // fall back to design-1/2 capture stack
#endif
}
```

Also gate on a runtime version tag so a mismatched engine fails to the fallback
rather than mis-binding.

## Work items (tracked)

1. **A3 `StreamModifications`** — port `WriteToWAL` extraction (~1-2 days). The
   only real engineering left.
2. `ingest_engine_modifications` — chunks → `TxnCapture` → `apply_captured_delta`
   (reuses existing ingestion; small).
3. Generate + commit `patches/v1.5.4-dbsp-txn-callback.patch` once A3 compiles.
4. Wire `DBSP_ENGINE_HOOK` define into the patched-engine build only.
5. Once prod is on the fork: delete the capture stack (`dbsp_write_capture.hpp`,
   `dbsp_plan_tee.hpp`, guard machinery) — ~1,500 lines.
6. Optional: file the upstream PR (MV primitive) to shrink rebases to zero.

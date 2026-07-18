# Proposal: forked engine hook for exact write capture

Status: PROPOSAL (2026-07-17). Not scheduled. Companion to
`docs/DESIGN_WRITE_CAPTURE.md`, which documents the shipped extension-only
capture stack this proposal would supersede on patched engines.

## Where we are

Delta ingestion today is a three-tier stack built entirely on extension
surfaces of pinned DuckDB v1.5.4:

1. **Notify API** — host tells us the delta. Exact, fastest, invasive to
   the host.
2. **Design 1: pre-image capture** — one internal SELECT per whitelisted
   statement predicts the delta before the statement runs; a three-part
   commit guard (commit-seq conflict, signed COUNT(*), rowid
   re-verification) validates the prediction against committed storage.
3. **Design 2: plan tee** — an OptimizerExtension widens DML child plans
   and injects a pass-through operator that records the rows the plan
   actually processed.

Together these make every plain-SQL DML statement O(Δ) except four
residual shapes: multi-statement query strings, Appender-API writes (no
per-statement hook fires), multi-match `UPDATE ... FROM` (ambiguous by
SQL semantics), and non-repeatable `INSERT ... SELECT` sources
(LIMIT/SAMPLE/table functions).

The stack works — measured ~1.0–1.5 ms per statement at 1M rows, suite,
soak, ASAN and TSAN clean — but it rests on **empirically probed engine
behavior**, none of it contractual:

- autocommit statements have an active transaction at QueryBegin AND
  QueryEnd, yet their commit hook fires mid-statement with an unusable
  catalog view;
- `PhysicalUpdate` reads the rowid from the LAST child column by
  position;
- a projection-pushdown `LogicalGet` exposes only `projection_ids`
  entries as bindings;
- `update_is_del_and_insert` conditions (indexed/LIST SET columns);
- optimizer-time expressions are `BOUND_COLUMN_REF`, resolver-time are
  `BOUND_REF`.

Every DuckDB upgrade re-litigates this list. Upstream offers no exit:
the June 2026 roadmap has no CDC/hook item, and discussion #12408 has
sat unanswered since 2024.

## The proposal

Fork the pinned engine with **one surgical patch** — a commit-time
modification callback — and make the extension consume it when present,
falling back to the shipped stack when not.

### Engine patch (branch `v1.5.4-dbsp`, single commit, ~300 lines)

**New surface** (mirrors the existing `OptimizerExtension` registration
pattern):

```cpp
// duckdb/main/config.hpp
struct TransactionModificationCallback {
  // Fired inside DuckTransaction::Commit, BEFORE the commit finalizes,
  // while the transaction's snapshot and version info are still valid.
  // One invocation per modified table. Chunks stream old images for
  // deletes/updates and new images for appends/updates, full-width, in
  // table column order.
  std::function<void(ClientContext &, TableCatalogEntry &,
                     TransactionModifications &)> on_commit;
};
// DBConfig::transaction_modification_callbacks (vector, Register())
```

**Implementation site**: `DuckTransaction::Commit` already owns
everything the extension spends 1,500 lines approximating:

- the **UndoBuffer** holds every modification this transaction made:
  `DELETE_TUPLE` entries carry the deleted rowids, `UPDATE_TUPLE`
  entries carry per-column old values, `INSERT_TUPLE`/LocalStorage
  carries appended rows;
- undo `UPDATE_TUPLE` entries store only the *updated* columns — the
  callback reconstructs full old rows by fetching the untouched columns
  via `DataTable::Fetch` with the still-live transaction (engine-side
  this is a cheap versioned point-read; extension-side it is
  impossible, which is the whole reason design 1 predicts and design 2
  tees);
- new images for updates are the current in-transaction row versions —
  fetchable the same way;
- Appender and multi-statement writes flow through the same undo buffer
  as everything else. There is no shape distinction at this layer.

The patch touches: one new header, `DBConfig` (callback registry), one
call site in `DuckTransaction::Commit`, and a chunk-assembly helper that
walks undo entries per table. Nothing else. No behavior change when no
callback is registered.

### Extension changes

- **Runtime detection**: at load, probe for the callback registry
  (compile-time `#ifdef` against the patched header + runtime version
  tag). Present → register the hook consumer; absent → today's stack,
  unchanged.
- **Hook consumer** (~150 lines): converts streamed modification chunks
  into the existing per-transaction `TxnCapture` signed delta (old −1 /
  new +1 / append +1) and applies through `apply_captured_delta` at
  commit — the same ingestion path every tier already uses. No guards:
  the engine reported what happened, prediction error is impossible by
  construction.
- **Demote, don't delete** (phase 1): design 1 and the tee stay compiled
  in as the unpatched-engine path. If the fork becomes the only
  supported deployment, ~1,500 lines become deletable:
  `dbsp_write_capture.hpp` (~700: shape vetting, capture-SQL builders,
  upsert LEFT-JOIN probe), `dbsp_plan_tee.hpp` (~400: plan widening,
  binding surgery), guard machinery in `dbsp_context_state.hpp` (~400:
  seq/count/rowid verification, volatility vetting).

### Upstream play

Submit the patch as a DuckDB PR referencing discussion #12408. The
strongest acceptance argument: DuckDB's own roadmap lists **Materialized
Views** as unfunded future work, and a commit-time modification stream is
precisely the primitive that feature needs. Best case the fork lives for
one release cycle; worst case we carry one rebaseable commit.

## Expected improvements

| Dimension | Today (extension-only) | With hook |
|---|---|---|
| Write coverage | all DML except 4 residual shapes | 100% — incl. Appender, multi-statement, multi-match `UPDATE...FROM`, any INSERT source |
| Per-statement latency @1M rows | ~1.0–1.5 ms (capture SELECT + guards + apply) | ~0.3 ms (apply only — G2 floor for every shape) |
| Predicate-heavy multi-row UPDATE | pays the statement's scan twice | statement cost only (~2×) |
| Guard fallbacks under concurrent writers | any interleaved commit poisons in-flight captures → scan | structurally zero — deltas are per-transaction facts |
| Capture-layer code | ~1,500 lines, six empirical engine assumptions | ~150 lines, one owned patch |
| Engine upgrade cost | re-verify hook ordering + plan shapes + binding semantics | rebase one ~300-line commit |
| View/table consistency | small post-commit apply window | commit-atomic possible (apply inside commit path) |
| DDL awareness | untracked hazard | same surface can report catalog changes |
| Durability foundation | watermark checkpoints | exact per-commit delta stream → WAL-style view persistence, exactly-once restore |

**Explicit non-goals** — the hook fixes ingestion only. Propagation
throughput (1–2M rows/s aggregates, ~460k rows/s joins) is bounded by
the `DuckDBRow`/`Value` data plane; spill coverage gaps, E110 SQL gaps,
and ORDER BY/window view memory are untouched. Those are separate
(columnar data plane) projects — see `DESIGN_DATA_PLANE.md` (DP1 vectorized
row hashing shipped; DP2–DP4 workload-gated).

## Costs and risks

- **Distribution**: the loadable `dbsp.duckdb_extension` currently LOADs
  into official DuckDB builds (pip `duckdb==1.5.4` — the python test
  suite does exactly this). Against a forked engine that mode requires
  shipping a forked engine binary or wheel. The dual-mode design keeps
  official-build hosts on the shipped stack, so nothing regresses — but
  the hook's benefits only reach hosts that take our engine build.
- **Ownership**: CVE response and bugfix backports for the forked engine
  are ours for as long as the patch lives out of tree.
- **Divergence discipline**: the patch stays at ONE commit. Any second
  "while we're in here" engine change is a design smell and a rebase
  liability.

## Effort

- Engine patch + chunk assembly: 2–3 days (undo-buffer walk + fetch
  reconstruction is the substance; registration is boilerplate).
- Extension consumer + runtime detection + differential tests (hook
  path vs shipped path over the full matrix): 1–2 days.
- Forked python wheel / distribution automation, if needed: the real
  multi-week item; defer until a host actually needs it.
- Upstream PR write-up: half a day, high option value.

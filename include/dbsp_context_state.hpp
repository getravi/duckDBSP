// Per-connection hook state: auto-CDC transaction hooks + the O(Δ)
// captured-delta fast paths (G2 inserts, write-capture updates/deletes).
//
// INSERT (G2): explicit transactions expose their appended rows through
// the transaction's LocalStorage while the transaction is alive (QueryEnd
// fires with the transaction still open); pure-INSERT transactions on
// tracked tables are captured row-by-row as they execute.
//
// UPDATE/DELETE (write capture, docs/DESIGN_WRITE_CAPTURE.md): at
// QueryBegin a whitelisted statement on a tracked table runs one
// internal-connection SELECT that reads the old images and computes the
// new ones (SET expressions projected, cast to column types), yielding a
// signed delta — works for explicit transactions AND autocommit (the
// capture needs no transaction internals; autocommit applies from the
// mid-statement commit hook, where the fallback sync already runs).
//
// Autocommit INSERT ... VALUES (write capture too): the statement's own
// VALUES list is evaluated via one internal SELECT with the INSERT's
// to-column-type casts (full-cover column lists only — partial lists
// involve defaults). Explicit-txn INSERTs stay on G2, which is exact.
//
// At commit a guard validates every captured table — commit-sequence
// conflict check, signed COUNT(*), and rowid re-verification of written
// rows — and the merged delta is applied in O(delta) via
// apply_captured_delta. Anything else — autocommit INSERTs (LocalStorage
// is gone at every hook), non-whitelisted writes (txn marked dirty),
// guard mismatches (counted loudly) — falls back to the scan-and-diff
// sync. Captured and scanned deltas are never mixed for one commit.
// Correctness never depends on capture.

#pragma once

#include "dbsp_cdc.hpp"
#include "dbsp_recovery.hpp"
#include "dbsp_write_capture.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/transaction/transaction_context.hpp"
#include "duckdb/storage/storage_manager.hpp"

#include <atomic>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

namespace dbsp_native {

class DBSPContextState : public duckdb::ClientContextState {
public:
  void QueryBegin(duckdb::ClientContext &context) override {
    if (internal_query_depth > 0) {
      return;
    }
    maybe_run_recovery(context);
    auto &manager = get_cdc_manager(context);
    // D3c: an out-of-band change invalidated a lazily-restored baseline —
    // reconciliation is impossible incrementally, so views rebuild from
    // committed storage at the next statement boundary (here). Runs even
    // with auto-sync off (the notify path can schedule it too).
    if (manager.rebuild_pending()) {
      try {
        manager.rebuild_all_views(context);
      } catch (const std::exception &e) {
        std::cerr << "DBSP: view rebuild failed: " << e.what() << "\n";
      }
    }
    const bool auto_sync = manager.is_auto_sync_enabled();
    if (!auto_sync && !manager.has_deferred()) {
      stmt_ = {};
      return; // no auto-sync: don't pay the parse on every statement
    }
    // Classify here: GetCurrentQuery() is already cleared by QueryEnd, and
    // for autocommit statements the commit hook fires mid-query — before
    // QueryEnd — so the commit-time sync scoping needs the classification
    // of the in-flight statement (stmt_), not just per-txn accumulation.
    // With auto-sync OFF this parse runs only while baselines are deferred
    // (D3c) — the write check below is the sole consumer.
    stmt_ = classify(context.GetCurrentQuery());
    // D3c: a write is about to execute. Deferred (lazy-restored) baselines
    // must materialize from PRE-write storage: this is the only moment the
    // scan is guaranteed to equal the restore-time content exactly. (The
    // notify path can materialize later with pending-delta subtraction,
    // but only the first fragment of a compound edit is known then —
    // materializing here keeps SQL-write-then-notify hosts exact.)
    if ((stmt_.kind == StmtClass::INSERT_OK ||
         stmt_.kind == StmtClass::WRITE_KNOWN ||
         stmt_.kind == StmtClass::WRITE_UNKNOWN) &&
        manager.has_deferred()) {
      manager.materialize_all_deferred(context);
      if (manager.rebuild_pending()) {
        try {
          manager.rebuild_all_views(context);
        } catch (const std::exception &e) {
          std::cerr << "DBSP: view rebuild failed: " << e.what() << "\n";
        }
      }
    }
    if (!auto_sync) {
      stmt_ = {}; // hooks are inert without auto-sync
      return;
    }
    // Write capture must run BEFORE the statement executes: the capture
    // SELECT reads committed (pre-statement) state. It also must precede
    // fold_into_txn, which otherwise poisons WRITE_KNOWN statements.
    // Autocommit INSERT_OK: the transaction dies before QueryEnd can read
    // LocalStorage (G2 needs an explicit txn), but a plain VALUES list is
    // right there in the statement — capture it the same way. Explicit-txn
    // INSERTs stay on G2 (exact, and capturing both would double-count).
    // Autocommit statements already run inside their own transaction here
    // (probed empirically), so every capture buffers in capture_ and
    // applies from the TransactionCommit hook.
    if (stmt_.kind == StmtClass::WRITE_KNOWN ||
        (stmt_.kind == StmtClass::INSERT_OK &&
         context.transaction.IsAutoCommit())) {
      try {
        try_write_capture(context, manager);
      } catch (...) {
        // capture is an optimization only; fold below poisons as needed
      }
    }
    if (context.transaction.HasActiveTransaction()) {
      fold_into_txn(context, stmt_);
    }
  }

  void TransactionBegin(duckdb::MetaTransaction &transaction,
                        duckdb::ClientContext &context) override {
    if (internal_query_depth > 0) {
      return;
    }
    capture_ = {};
    // Write-capture conflict guard: another connection's delta landing
    // after this moves the seq and poisons this transaction's captures.
    capture_.seq_snapshot = get_cdc_manager(context).commit_seq();
  }

  void QueryEnd(duckdb::ClientContext &context,
                duckdb::optional_ptr<duckdb::ErrorData> error) override {
    if (internal_query_depth > 0) {
      return;
    }
    auto &manager = get_cdc_manager(context);
    if (!manager.is_auto_sync_enabled()) {
      return;
    }
    if (error && error->HasError()) {
      return; // failed statement changed nothing
    }
    // Autocommit: the transaction is finished before any hook fires —
    // nothing to capture; the commit hook already ran sync_all
    if (!context.transaction.HasActiveTransaction()) {
      return;
    }

    try {
      classify_and_capture(context);
    } catch (const std::exception &) {
      // Capture is an optimization only: on any surprise, poison the
      // transaction so commit falls back to scan-and-diff
      capture_.dirty = true;
    } catch (...) {
      capture_.dirty = true;
    }
  }

  void TransactionCommit(duckdb::MetaTransaction &transaction,
                         duckdb::ClientContext &context) override {
    // Skip commits from DBSP's own internal helper connections - recursing
    // into CDCManager here deadlocks (issuing thread may hold struct_mutex_)
    if (internal_query_depth > 0) {
      return;
    }

    auto &manager = get_cdc_manager(context);
    if (!manager.is_auto_sync_enabled()) {
      capture_ = {};
      return;
    }

    try {
      if (capture_.active && !capture_.dirty && !capture_.unknown_writes &&
          apply_captured(context, manager)) {
        capture_ = {};
        return; // O(delta) fast path served this commit
      }

      // H1: scope the fallback sync to the tables this transaction wrote.
      // Autocommit commits fire mid-statement, before QueryEnd folded the
      // in-flight classification — fold it now.
      if (!capture_.saw_statements && stmt_.kind != StmtClass::NONE) {
        fold_into_txn(context, stmt_);
      }
      const bool know_all_writes =
          capture_.saw_statements && !capture_.unknown_writes;
      std::vector<std::string> touched(capture_.touched.begin(),
                                       capture_.touched.end());
      capture_ = {};

      if (know_all_writes && touched.empty()) {
        return; // read-only commit: nothing can have changed
      }
      // The transaction has already committed successfully at this point.
      // We can safely read the post-commit state and pass the transaction
      // for proper catalog access.
      if (know_all_writes) {
        manager.sync_tables(context, touched,
                            manager.parallel_sync_enabled() &&
                                touched.size() > 1,
                            &transaction);
      } else {
        // Writes we could not attribute (Appender API, multi-statement,
        // unparseable SQL): scan everything, as before H1
        manager.sync_all(context, &transaction);
      }
    } catch (const std::exception &ex) {
      std::cerr << "DBSP Auto-CDC error: " << ex.what() << "\n";
    } catch (...) {
      std::cerr << "DBSP Auto-CDC unknown error\n";
    }
  }

  void TransactionRollback(duckdb::MetaTransaction &transaction,
                           duckdb::ClientContext &context) override {
    if (internal_query_depth > 0) {
      return;
    }
    capture_ = {}; // rolled back: captured rows never happened
  }

  // One-time crash recovery, moved here from OnConnectionOpened: that
  // callback runs under ConnectionManager::connections_lock, and recovery
  // opens internal Connections whose constructors re-enter AddConnection —
  // a self-deadlock. QueryBegin runs without that lock. Guarded so
  // recovery's own internal connections (internal_query_depth > 0) and
  // re-entrant queries never recurse.
  static void maybe_run_recovery(duckdb::ClientContext &context) {
    static std::atomic<bool> recovery_started{false};
    bool expected = false;
    if (!recovery_started.compare_exchange_strong(expected, true)) {
      return;
    }
    auto &recovery_manager = get_recovery_manager();
    std::string db_path;
    try {
      auto &db_manager = duckdb::DatabaseManager::Get(context);
      // The default catalog's NAME is not its path ("m" for m.duckdb) and
      // the old lookup by DEFAULT_SCHEMA ("main") never matched anything —
      // which parked recovery markers in the process CWD for every mode.
      auto default_db = db_manager.GetDatabase(
          context, duckdb::DatabaseManager::GetDefaultDatabase(context));
      if (default_db) {
        auto &storage = default_db->GetStorageManager();
        if (!storage.InMemory()) {
          db_path = storage.GetDBPath();
        }
      }
    } catch (...) {
      // no durable default catalog: markers stay disabled
    }
    recovery_manager.recover_from_crash(context, db_path);
  }

private:
  struct TxnCapture {
    bool active = false;
    bool dirty = false;
    // H1 sync scoping: which tables this transaction wrote, and whether we
    // saw/classified every statement. Writes we could not attribute
    // (unparseable, multi-statement, unknown statement kinds) force a full
    // sync; a transaction with zero seen statements (e.g. the Appender API
    // bypasses query hooks entirely) also forces a full sync.
    bool saw_statements = false;
    bool unknown_writes = false;
    std::unordered_set<std::string> touched;
    // per tracked table: captured append delta + local rows consumed so far
    std::unordered_map<std::string, std::pair<DuckDBZSet, duckdb::idx_t>>
        appends;
    // Write capture (UPDATE/DELETE): signed deltas + the rowids the commit
    // guard re-verifies against committed storage
    struct WriteVerify {
      int64_t rowid;
      bool is_delete;         // guard expects the rowid to be gone
      DuckDBRow expected_new; // UPDATE: guard expects exactly this row
    };
    std::unordered_map<std::string, DuckDBZSet> write_deltas;
    std::unordered_map<std::string, std::vector<WriteVerify>> verifies;
    uint64_t seq_snapshot = 0;
    bool wrote_capture = false;
  };
  TxnCapture capture_;

  // Classification of one SQL statement (computed at QueryBegin)
  struct StmtClass {
    enum Kind {
      NONE,        // empty / not seen
      READ,        // changes nothing
      INSERT_OK,   // plain INSERT: capturable append
      WRITE_KNOWN, // write with a known target table (DELETE/UPDATE/upsert)
      WRITE_UNKNOWN // anything else that might write anywhere
    };
    Kind kind = NONE;
    std::string table;   // INSERT_OK / WRITE_KNOWN target
    std::string text;    // original statement (INSERT capture needs it)
    bool captured = false; // WRITE_KNOWN served by write capture: not dirty
  };
  StmtClass stmt_;

  // H2 guard cache: one internal connection + a prepared COUNT(*) per table
  std::unique_ptr<duckdb::Connection> guard_con_;
  std::unordered_map<std::string, duckdb::unique_ptr<duckdb::PreparedStatement>>
      count_stmts_;

  static std::string base_table_name(const duckdb::TableRef *ref) {
    if (ref && ref->type == duckdb::TableReferenceType::BASE_TABLE) {
      auto &base = ref->Cast<duckdb::BaseTableRef>();
      return dotted_ref(base.catalog_name, base.schema_name, base.table_name);
    }
    return {};
  }

  // Textual dotted reference from parsed statement parts (either or both
  // qualifiers may be absent). Canonicalization happens at fold/capture
  // time via resolve_table_entry — parse-time text is never used as a key.
  static std::string dotted_ref(const std::string &catalog,
                                const std::string &schema,
                                const std::string &table) {
    std::string out;
    if (!catalog.empty()) {
      out += catalog + ".";
    }
    if (!schema.empty()) {
      out += schema + ".";
    }
    return out + table;
  }

  StmtClass classify(const std::string &query) {
    StmtClass out;
    if (query.empty()) {
      return out;
    }
    out.text = query;
    duckdb::Parser parser;
    try {
      parser.ParseQuery(query);
    } catch (...) {
      out.kind = StmtClass::WRITE_UNKNOWN; // unparseable: assume the worst
      return out;
    }
    if (parser.statements.size() != 1) {
      out.kind = parser.statements.empty() ? StmtClass::NONE
                                           : StmtClass::WRITE_UNKNOWN;
      return out;
    }
    auto &stmt = *parser.statements[0];
    switch (stmt.type) {
    case duckdb::StatementType::SELECT_STATEMENT:
    case duckdb::StatementType::EXPLAIN_STATEMENT:
    case duckdb::StatementType::PREPARE_STATEMENT:
    case duckdb::StatementType::TRANSACTION_STATEMENT:
      out.kind = StmtClass::READ;
      return out;
    case duckdb::StatementType::INSERT_STATEMENT: {
      auto &insert = stmt.Cast<duckdb::InsertStatement>();
      out.table = dotted_ref(insert.catalog, insert.schema, insert.table);
      out.kind = insert.on_conflict_info ? StmtClass::WRITE_KNOWN
                                         : StmtClass::INSERT_OK;
      return out;
    }
    case duckdb::StatementType::DELETE_STATEMENT: {
      auto &del = stmt.Cast<duckdb::DeleteStatement>();
      out.table = base_table_name(del.table.get());
      out.kind =
          out.table.empty() ? StmtClass::WRITE_UNKNOWN : StmtClass::WRITE_KNOWN;
      return out;
    }
    case duckdb::StatementType::UPDATE_STATEMENT: {
      auto &upd = stmt.Cast<duckdb::UpdateStatement>();
      out.table = base_table_name(upd.table.get());
      out.kind =
          out.table.empty() ? StmtClass::WRITE_UNKNOWN : StmtClass::WRITE_KNOWN;
      return out;
    }
    default:
      out.kind = StmtClass::WRITE_UNKNOWN;
      return out;
    }
  }

  // Merge one statement's classification into the transaction's sync scope
  void fold_into_txn(duckdb::ClientContext &context, const StmtClass &c) {
    if (c.kind == StmtClass::NONE) {
      return;
    }
    capture_.saw_statements = true;
    switch (c.kind) {
    case StmtClass::READ:
      break;
    case StmtClass::INSERT_OK:
    case StmtClass::WRITE_KNOWN: {
      // Canonicalize the parsed reference; a target that does not resolve
      // cannot be a tracked table (tracked keys always resolve), so skip.
      auto entry = resolve_table_entry(context, c.table);
      if (entry &&
          get_cdc_manager(context).is_table_tracked(
              canonical_table_key(*entry))) {
        capture_.touched.insert(canonical_table_key(*entry));
      }
    }
      if (c.kind == StmtClass::WRITE_KNOWN && !c.captured) {
        capture_.dirty = true; // not capturable, but the target is known
      }
      break;
    default:
      capture_.dirty = true;
      capture_.unknown_writes = true;
      break;
    }
  }

  void classify_and_capture(duckdb::ClientContext &context) {
    const StmtClass c = std::move(stmt_);
    stmt_ = {};
    if (c.kind != StmtClass::INSERT_OK) {
      return; // scoping already folded at QueryBegin; only capture remains
    }
    auto &manager = get_cdc_manager(context);
    auto entry = resolve_table_entry(context, c.table);
    if (!entry) {
      return; // unresolvable target cannot be tracked
    }
    const std::string key = canonical_table_key(*entry);
    if (!manager.is_table_tracked(key)) {
      return; // untracked target: irrelevant to views
    }
    capture_appended_rows(context, key);
    capture_.active = true;
  }

  // Read the transaction-local rows appended to `table_name` since the
  // last capture (LocalStorage keeps them until commit)
  void capture_appended_rows(duckdb::ClientContext &context,
                             const std::string &table_name) {
    auto &meta = context.transaction.ActiveTransaction();
    // table_name is a canonical key: resolve it to its entry and use the
    // transaction of the table's own attached database (D2).
    auto entry = resolve_table_entry(context, table_name);
    if (!entry) {
      return;
    }
    {
      auto &attached = entry->ParentCatalog().GetAttached();
      auto txn = meta.TryGetTransaction(attached);
      if (!txn) {
        return;
      }
      auto &dtxn = txn->Cast<duckdb::DuckTransaction>();
      auto &ls = dtxn.GetLocalStorage();
      auto &table = entry->GetStorage();
      if (!ls.Find(table)) {
        return; // no transaction-local rows for this table
      }

      auto &slot = capture_.appends[table_name];
      const duckdb::idx_t total = ls.AddedRows(table);
      if (total <= slot.second) {
        return; // nothing new (e.g. INSERT ... SELECT with zero rows)
      }

      duckdb::vector<duckdb::StorageIndex> column_ids;
      duckdb::vector<duckdb::LogicalType> types;
      for (auto &col : entry->GetColumns().Physical()) {
        column_ids.emplace_back(col.Physical().index);
        types.push_back(col.Type());
      }

      duckdb::TableScanState scan_state;
      scan_state.Initialize(column_ids);
      ls.InitializeScan(table, scan_state.local_state, nullptr);
      duckdb::DataChunk chunk;
      chunk.Initialize(duckdb::Allocator::Get(context), types);

      duckdb::idx_t seen = 0;
      while (true) {
        chunk.Reset();
        ls.Scan(scan_state.local_state, column_ids, chunk);
        const duckdb::idx_t n = chunk.size();
        if (n == 0) {
          break;
        }
        chunk.Flatten();
        for (duckdb::idx_t i = 0; i < n; i++, seen++) {
          if (seen < slot.second) {
            continue; // already captured by an earlier statement
          }
          DuckDBRow row;
          row.columns.reserve(types.size());
          for (duckdb::idx_t c = 0; c < types.size(); c++) {
            row.columns.push_back(chunk.GetValue(c, i));
          }
          slot.first.insert(std::move(row), 1);
        }
      }
      slot.second = total;
      return;
    }
  }

  duckdb::Connection &guard_connection(duckdb::ClientContext &context) {
    if (!guard_con_) {
      guard_con_ = std::make_unique<duckdb::Connection>(
          duckdb::DatabaseInstance::GetDatabase(context));
    }
    return *guard_con_;
  }

  // O(Δ) write capture (docs/DESIGN_WRITE_CAPTURE.md): runs BEFORE the
  // UPDATE/DELETE executes. One internal SELECT captures the old images
  // and, for UPDATE, computes the new images by projecting the SET
  // expressions cast to their column types. Declining is always safe —
  // the statement stays on the scan-and-diff path.
  void try_write_capture(duckdb::ClientContext &context, CDCManager &manager) {
    if (!manager.write_capture_enabled()) {
      return;
    }
    // Captures buffer per-transaction and apply at commit; without a
    // transaction there is no commit hook to apply from (never observed —
    // autocommit statements have their transaction by QueryBegin)
    if (!context.transaction.HasActiveTransaction()) {
      return;
    }
    if (capture_.dirty || capture_.unknown_writes) {
      return; // this transaction already fell off the fast path
    }
    duckdb::Parser parser;
    parser.ParseQuery(stmt_.text);
    if (parser.statements.size() != 1) {
      return;
    }
    auto &parsed = *parser.statements[0];
    const bool is_insert =
        parsed.type == duckdb::StatementType::INSERT_STATEMENT;
    const bool is_upsert =
        is_insert && parsed.Cast<duckdb::InsertStatement>().on_conflict_info;
    if (parsed.type != duckdb::StatementType::UPDATE_STATEMENT &&
        parsed.type != duckdb::StatementType::DELETE_STATEMENT && !is_insert) {
      return;
    }
    if (is_insert && !is_upsert && !context.transaction.IsAutoCommit()) {
      return; // explicit-txn INSERTs use the exact G2 LocalStorage scan
    }

    InternalQueryGuard guard;
    auto &con = guard_connection(context);
    auto &ictx = *con.context;
    std::string key;
    std::unique_ptr<WriteCapturePlan> plan;
    ictx.RunFunctionInTransaction([&] {
      auto entry = resolve_table_entry(ictx, stmt_.table);
      if (!entry) {
        return;
      }
      key = canonical_table_key(*entry);
      if (!manager.is_table_tracked(key)) {
        key.clear();
        return;
      }
      plan = is_upsert ? plan_upsert_capture(ictx, parsed, *entry, key)
             : is_insert ? plan_insert_capture(parsed, *entry)
                         : plan_write_capture(ictx, parsed, *entry, key);
    });
    if (key.empty() || !plan) {
      return;
    }
    // The capture SELECT reads committed state: a target this transaction
    // already wrote would be read stale — decline.
    if (capture_.touched.count(key)) {
      return;
    }
    // Volatile (or per-query-constant) functions would re-evaluate
    // differently in the statement itself
    auto logical = con.ExtractPlan(plan->capture_sql);
    if (!logical || !bound_plan_consistent(*logical)) {
      return;
    }
    auto res = con.Query(plan->capture_sql);
    if (!res || res->HasError()) {
      return;
    }

    const bool is_update = plan->kind == WriteCapturePlan::Kind::Update;
    // physical column -> projection slot holding its new value
    std::unordered_map<size_t, size_t> new_slots(plan->set_cols.begin(),
                                                 plan->set_cols.end());
    DuckDBZSet delta;
    std::vector<TxnCapture::WriteVerify> verifies;
    while (auto chunk = res->Fetch()) {
      const duckdb::idx_t n = chunk->size();
      if (n == 0) {
        break;
      }
      for (duckdb::idx_t i = 0; i < n; i++) {
        if (plan->kind == WriteCapturePlan::Kind::Insert) {
          // projection IS the new row (no rowid — guard is seq + count)
          DuckDBRow row;
          row.columns.reserve(plan->n_cols);
          for (size_t c = 0; c < plan->n_cols; c++) {
            row.columns.push_back(chunk->GetValue(c, i));
          }
          delta.insert(std::move(row), 1);
          continue;
        }
        if (plan->kind == WriteCapturePlan::Kind::Upsert) {
          if (chunk->GetValue(0, i).IsNull()) {
            // no conflict: insert-part row from the padded source image
            DuckDBRow row;
            row.columns.reserve(plan->n_cols);
            for (size_t c = 0; c < plan->n_cols; c++) {
              row.columns.push_back(
                  chunk->GetValue(plan->insert_slot_base + c, i));
            }
            delta.insert(std::move(row), 1);
            continue;
          }
          if (plan->set_cols.empty()) {
            continue; // DO NOTHING: matched rows change nothing
          }
          TxnCapture::WriteVerify v;
          v.rowid = chunk->GetValue(0, i).GetValue<int64_t>();
          v.is_delete = false;
          DuckDBRow old_row, new_row;
          old_row.columns.reserve(plan->n_cols);
          new_row.columns.reserve(plan->n_cols);
          for (size_t c = 0; c < plan->n_cols; c++) {
            auto slot = new_slots.find(c);
            old_row.columns.push_back(chunk->GetValue(c + 1, i));
            new_row.columns.push_back(chunk->GetValue(
                slot == new_slots.end() ? c + 1 : slot->second, i));
          }
          v.expected_new = new_row;
          delta.insert(std::move(new_row), 1);
          delta.insert(std::move(old_row), -1);
          verifies.push_back(std::move(v));
          continue;
        }
        TxnCapture::WriteVerify v;
        v.rowid = chunk->GetValue(0, i).GetValue<int64_t>();
        v.is_delete = !is_update;
        DuckDBRow old_row;
        old_row.columns.reserve(plan->n_cols);
        for (size_t c = 0; c < plan->n_cols; c++) {
          old_row.columns.push_back(chunk->GetValue(c + 1, i));
        }
        if (is_update) {
          DuckDBRow new_row;
          new_row.columns.reserve(plan->n_cols);
          for (size_t c = 0; c < plan->n_cols; c++) {
            auto slot = new_slots.find(c);
            new_row.columns.push_back(chunk->GetValue(
                slot == new_slots.end() ? c + 1 : slot->second, i));
          }
          v.expected_new = new_row;
          delta.insert(std::move(new_row), 1);
        }
        delta.insert(std::move(old_row), -1);
        verifies.push_back(std::move(v));
      }
    }

    auto &slot = capture_.write_deltas[key];
    for (const auto &[row, w] : delta) {
      slot.insert(row, w);
    }
    auto &vv = capture_.verifies[key];
    vv.insert(vv.end(), std::make_move_iterator(verifies.begin()),
              std::make_move_iterator(verifies.end()));
    capture_.wrote_capture = true;
    capture_.active = true;
    stmt_.captured = true;
  }

  // Commit guard part 3: re-read the captured rowids from committed
  // storage — deleted rowids must be gone, updated rowids must hold
  // exactly the predicted post-image. Rowid IN-lists prune, so this is
  // O(Δ) regardless of table size.
  bool verify_write_rows(duckdb::ClientContext &context,
                         const std::string &table,
                         const std::vector<TxnCapture::WriteVerify> &rows) {
    if (rows.empty()) {
      return true;
    }
    InternalQueryGuard guard;
    auto &con = guard_connection(context);
    const auto *schema = get_cdc_manager(context).get_table_schema(table);
    if (!schema) {
      return false;
    }
    std::string col_list;
    for (const auto &col : schema->columns) {
      col_list += ", " + quote_ident(col.name);
    }
    constexpr size_t kBatch = 512;
    for (size_t start = 0; start < rows.size();) {
      std::string del_ids, upd_ids;
      std::unordered_map<int64_t, const DuckDBRow *> expected;
      const size_t end = std::min(rows.size(), start + kBatch);
      for (size_t i = start; i < end; i++) {
        const auto &v = rows[i];
        std::string &ids = v.is_delete ? del_ids : upd_ids;
        if (!ids.empty()) {
          ids += ",";
        }
        ids += std::to_string(v.rowid);
        if (!v.is_delete) {
          expected[v.rowid] = &v.expected_new;
        }
      }
      start = end;
      if (!del_ids.empty()) {
        auto res = con.Query("SELECT COUNT(*) FROM " + quote_table_key(table) +
                             " WHERE rowid IN (" + del_ids + ")");
        if (!res || res->HasError()) {
          return false;
        }
        auto chunk = res->Fetch();
        if (!chunk || chunk->size() == 0 ||
            chunk->GetValue(0, 0).GetValue<int64_t>() != 0) {
          return false;
        }
      }
      if (!upd_ids.empty()) {
        auto res = con.Query("SELECT rowid" + col_list + " FROM " +
                             quote_table_key(table) + " WHERE rowid IN (" +
                             upd_ids + ")");
        if (!res || res->HasError()) {
          return false;
        }
        size_t seen = 0;
        while (auto chunk = res->Fetch()) {
          const duckdb::idx_t n = chunk->size();
          if (n == 0) {
            break;
          }
          for (duckdb::idx_t i = 0; i < n; i++, seen++) {
            const int64_t rowid = chunk->GetValue(0, i).GetValue<int64_t>();
            auto it = expected.find(rowid);
            if (it == expected.end()) {
              return false;
            }
            DuckDBRow got;
            got.columns.reserve(it->second->columns.size());
            for (size_t c = 0; c < it->second->columns.size(); c++) {
              got.columns.push_back(chunk->GetValue(c + 1, i));
            }
            if (!(got == *it->second)) {
              return false;
            }
          }
        }
        if (seen != expected.size()) {
          return false;
        }
      }
    }
    return true;
  }

  // Validate every captured table against the committed COUNT(*) and, if
  // all match, feed the captured deltas straight into propagation
  // Guard COUNT(*) through a cached connection + prepared statements:
  // parsing/binding/planning the count per commit was most of the fast
  // path's cost (H2)
  int64_t committed_count(duckdb::ClientContext &context,
                          const std::string &table) {
    InternalQueryGuard guard;
    guard_connection(context);
    auto it = count_stmts_.find(table);
    if (it == count_stmts_.end()) {
      auto prep =
          guard_con_->Prepare("SELECT COUNT(*) FROM " + quote_table_key(table));
      if (!prep || prep->HasError()) {
        return -1;
      }
      it = count_stmts_.emplace(table, std::move(prep)).first;
    }
    auto res = it->second->Execute();
    if (!res || res->HasError()) {
      count_stmts_.erase(it); // schema may have changed: re-prepare next time
      return -1;
    }
    auto chunk = res->Fetch();
    if (!chunk || chunk->size() == 0) {
      return -1;
    }
    return chunk->GetValue(0, 0).GetValue<int64_t>();
  }

  bool apply_captured(duckdb::ClientContext &context, CDCManager &manager) {
    if (capture_.appends.empty() && capture_.write_deltas.empty()) {
      return true; // clean read-only transaction: nothing to sync
    }
    const bool wrote = capture_.wrote_capture;
    auto fail = [&]() {
      if (wrote) {
        manager.note_capture_guard_fallback();
      }
      return false;
    };
    // Guard 1: an interleaved commit invalidates committed-state captures
    if (wrote && manager.commit_seq() != capture_.seq_snapshot) {
      return fail();
    }
    // Merge appends + write deltas per table: one apply (and one
    // propagation) per table keeps the commit a single consistent step
    std::unordered_map<std::string, DuckDBZSet> merged;
    for (const auto &[table, slot] : capture_.appends) {
      auto &dst = merged[table];
      for (const auto &[row, w] : slot.first) {
        dst.insert(row, w);
      }
    }
    for (const auto &[table, delta] : capture_.write_deltas) {
      auto &dst = merged[table];
      for (const auto &[row, w] : delta) {
        dst.insert(row, w);
      }
    }
    // Guard 2: committed COUNT(*) must equal baseline + captured weight
    for (const auto &[table, delta] : merged) {
      const int64_t actual = committed_count(context, table);
      if (actual < 0) {
        return fail();
      }
      int64_t captured = 0;
      for (const auto &[row, w] : delta) {
        captured += w;
      }
      const int64_t baseline = manager.tracked_total_weight(table);
      if (baseline < 0 || baseline + captured != actual) {
        return fail(); // something we did not see changed the table
      }
    }
    // Guard 3: rowid re-verification of updated/deleted rows
    for (const auto &[table, rows] : capture_.verifies) {
      if (!verify_write_rows(context, table, rows)) {
        return fail();
      }
    }
    for (const auto &[table, delta] : merged) {
      if (!manager.apply_captured_delta(table, delta, &context)) {
        return false;
      }
    }
    return true;
  }
};

} // namespace dbsp_native

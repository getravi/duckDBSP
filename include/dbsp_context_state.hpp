// Per-connection hook state: auto-CDC transaction hooks + the G2
// captured-delta fast path.
//
// Explicit transactions expose their appended rows through the
// transaction's LocalStorage while the transaction is alive (QueryEnd
// fires with the transaction still open). Pure-INSERT transactions on
// tracked tables are therefore captured row-by-row as they execute; at
// commit, a COUNT(*) guard validates the captured delta against the
// committed table and it is applied in O(delta) — no scan, no diff.
// Anything else — autocommit statements (the transaction is already gone
// at every hook), DELETE/UPDATE/unclassifiable statements (txn marked
// dirty), guard mismatches — falls back to the scan-and-diff sync_all.
// Correctness never depends on capture.

#pragma once

#include "dbsp_cdc.hpp"
#include "dbsp_recovery.hpp"
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
    if (!get_cdc_manager(context).is_auto_sync_enabled()) {
      stmt_ = {};
      return; // no auto-sync: don't pay the parse on every statement
    }
    // Classify here: GetCurrentQuery() is already cleared by QueryEnd, and
    // for autocommit statements the commit hook fires mid-query — before
    // QueryEnd — so the commit-time sync scoping needs the classification
    // of the in-flight statement (stmt_), not just per-txn accumulation
    stmt_ = classify(context.GetCurrentQuery());
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
      auto default_db =
          db_manager.GetDatabase(context, DEFAULT_SCHEMA);
      if (default_db) {
        db_path = default_db->GetName();
      }
    } catch (...) {
      // fall back to the default recovery path
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
      if (c.kind == StmtClass::WRITE_KNOWN) {
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

  // Validate every captured table against the committed COUNT(*) and, if
  // all match, feed the captured deltas straight into propagation
  // Guard COUNT(*) through a cached connection + prepared statements:
  // parsing/binding/planning the count per commit was most of the fast
  // path's cost (H2)
  int64_t committed_count(duckdb::ClientContext &context,
                          const std::string &table) {
    InternalQueryGuard guard;
    if (!guard_con_) {
      guard_con_ = std::make_unique<duckdb::Connection>(
          duckdb::DatabaseInstance::GetDatabase(context));
    }
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
    if (capture_.appends.empty()) {
      return true; // clean read-only transaction: nothing to sync
    }
    for (const auto &[table, slot] : capture_.appends) {
      const int64_t actual = committed_count(context, table);
      if (actual < 0) {
        return false;
      }
      int64_t captured = 0;
      for (const auto &[row, w] : slot.first) {
        captured += w;
      }
      const int64_t baseline = manager.tracked_total_weight(table);
      if (baseline < 0 || baseline + captured != actual) {
        return false; // something we did not see changed the table
      }
    }
    for (const auto &[table, slot] : capture_.appends) {
      if (!manager.apply_captured_delta(table, slot.first)) {
        return false;
      }
    }
    return true;
  }
};

} // namespace dbsp_native

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
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/transaction/transaction_context.hpp"

#include <iostream>
#include <unordered_map>

namespace dbsp_native {

class DBSPContextState : public duckdb::ClientContextState {
public:
  void QueryBegin(duckdb::ClientContext &context) override {
    if (internal_query_depth > 0) {
      return;
    }
    // GetCurrentQuery() is already cleared by the time QueryEnd fires:
    // remember the text here for end-of-query classification
    last_query_ = context.GetCurrentQuery();
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
    auto &manager = get_cdc_manager();
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

    auto &manager = get_cdc_manager();
    if (!manager.is_auto_sync_enabled()) {
      capture_ = {};
      return;
    }

    try {
      if (capture_.active && !capture_.dirty &&
          apply_captured(context, manager)) {
        capture_ = {};
        return; // O(delta) fast path served this commit
      }
      capture_ = {};
      // The transaction has already committed successfully at this point.
      // We can safely read the post-commit state and pass the transaction
      // to sync_all for proper catalog access.
      manager.sync_all(context, &transaction);
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

private:
  struct TxnCapture {
    bool active = false;
    bool dirty = false;
    // per tracked table: captured append delta + local rows consumed so far
    std::unordered_map<std::string, std::pair<DuckDBZSet, duckdb::idx_t>>
        appends;
  };
  TxnCapture capture_;
  std::string last_query_; // captured at QueryBegin

  void classify_and_capture(duckdb::ClientContext &context) {
    const std::string query = std::move(last_query_);
    last_query_.clear();
    if (query.empty()) {
      return;
    }
    duckdb::Parser parser;
    try {
      parser.ParseQuery(query);
    } catch (...) {
      capture_.dirty = true; // unparseable: assume the worst
      return;
    }
    if (parser.statements.size() != 1) {
      if (!parser.statements.empty()) {
        capture_.dirty = true; // multi-statement: classify conservatively
      }
      return;
    }
    auto &stmt = *parser.statements[0];
    switch (stmt.type) {
    case duckdb::StatementType::SELECT_STATEMENT:
    case duckdb::StatementType::EXPLAIN_STATEMENT:
    case duckdb::StatementType::PREPARE_STATEMENT:
    case duckdb::StatementType::TRANSACTION_STATEMENT:
      return; // reads change nothing
    case duckdb::StatementType::INSERT_STATEMENT:
      break; // candidate for capture
    default:
      capture_.dirty = true; // any other write: scan-diff at commit
      return;
    }

    auto &insert = stmt.Cast<duckdb::InsertStatement>();
    if (insert.on_conflict_info) {
      capture_.dirty = true; // upsert can modify existing rows
      return;
    }
    auto &manager = get_cdc_manager();
    if (!manager.is_table_tracked(insert.table)) {
      return; // untracked target: irrelevant to views
    }

    capture_appended_rows(context, insert.table);
    capture_.active = true;
  }

  // Read the transaction-local rows appended to `table_name` since the
  // last capture (LocalStorage keeps them until commit)
  void capture_appended_rows(duckdb::ClientContext &context,
                             const std::string &table_name) {
    auto &meta = context.transaction.ActiveTransaction();
    auto &db_manager = duckdb::DatabaseManager::Get(context);
    for (auto &db : db_manager.GetDatabases()) {
      if (db->IsSystem() || db->IsTemporary()) {
        continue;
      }
      auto txn = meta.TryGetTransaction(*db);
      if (!txn) {
        continue;
      }
      auto &dtxn = txn->Cast<duckdb::DuckTransaction>();
      auto &ls = dtxn.GetLocalStorage();
      auto &catalog = db->GetCatalog();
      auto entry = catalog.GetEntry<duckdb::TableCatalogEntry>(
          context, DEFAULT_SCHEMA, table_name,
          duckdb::OnEntryNotFound::RETURN_NULL);
      if (!entry) {
        continue;
      }
      auto &table = entry->GetStorage();
      if (!ls.Find(table)) {
        continue;
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
  bool apply_captured(duckdb::ClientContext &context, CDCManager &manager) {
    if (capture_.appends.empty()) {
      return true; // clean read-only transaction: nothing to sync
    }
    InternalQueryGuard guard;
    auto &db = duckdb::DatabaseInstance::GetDatabase(context);
    duckdb::Connection con(db);
    for (const auto &[table, slot] : capture_.appends) {
      auto res = con.Query("SELECT COUNT(*) FROM \"" + table + "\"");
      if (!res || res->HasError()) {
        return false;
      }
      const int64_t actual = res->GetValue(0, 0).GetValue<int64_t>();
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

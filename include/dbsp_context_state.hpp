#pragma once

#include "dbsp_cdc.hpp"
#include "duckdb.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace dbsp_native {

class DBSPContextState : public duckdb::ClientContextState {
public:
  void TransactionCommit(duckdb::MetaTransaction &transaction,
                         duckdb::ClientContext &context) override {
    // This is called just before the transaction is committed
    // We can inspect the local storage to see what changed
    try {
      auto &manager = get_cdc_manager();

      // Get the DuckDB transaction (assuming single database for now)
      // Note: In a multi-database setup, we might need to iterate over
      // databases But usually extensions operate on the attached database

      // We need to find the DuckTransaction.
      // MetaTransaction holds a map of AttachedDatabase -> Transaction*
      // We can iterate over tracked tables and check if they are modified in
      // this transaction

      // Optimization: If we can get the active DuckTransaction easily, that's
      // better. But MetaTransaction abstracts it. Let's iterate over tracked
      // tables and check their storage.

      auto tracked = manager.list_tracked_tables();
      if (tracked.empty()) {
        return;
      }

      // We need access to the Catalog to look up tables
      // Using the INVALID_CATALOG allows searching in the default
      // catalog/schema path
      auto &catalog = duckdb::Catalog::GetCatalog(context, INVALID_CATALOG);

      // Get the transaction for this catalog
      // This might return null if the transaction hasn't touched this catalog?
      // Or we can try to get it from the context
      auto &duck_txn = duckdb::DuckTransaction::Get(context, catalog);
      auto &storage = duckdb::LocalStorage::Get(duck_txn);

      for (const auto &table_name : tracked) {
        // Find the table entry
        auto table_entry = catalog.GetEntry(
            context, duckdb::CatalogType::TABLE_ENTRY, DEFAULT_SCHEMA,
            table_name, duckdb::OnEntryNotFound::RETURN_NULL);

        if (!table_entry)
          continue; // Table might have been dropped

        auto &table = table_entry->Cast<duckdb::TableCatalogEntry>();
        auto &data_table = table.GetStorage();

        // Check if this table has changes in local storage
        // LocalStorage::Find(DataTable&) returns true if there are changes
        if (storage.Find(data_table)) {
          // Flush changes to our CDC manager
          // We need a way to extract the changes.
          // LocalStorage doesn't expose a clean "Give me the delta" iterator
          // for external use easily without scanning. However, we can scan the
          // LocalStorage for this table.

          // Create a scan state for local storage
          duckdb::TableScanState table_scan_state;
          // We don't need to Initialize table_scan_state fully if we only use
          // local_state But CollectionScanState needs a parent in its
          // constructor.

          storage.InitializeScan(data_table, table_scan_state.local_state,
                                 nullptr);

          // Get column types - fix: don't use reference for temporary vector
          auto types = data_table.GetTypes();
          duckdb::DataChunk chunk;
          chunk.Initialize(context, types);

          duckdb::vector<duckdb::StorageIndex> column_ids;
          for (idx_t i = 0; i < types.size(); i++) {
            column_ids.push_back(duckdb::StorageIndex(i));
          }

          while (true) {
            chunk.Reset();
            storage.Scan(table_scan_state.local_state, column_ids, chunk);
            if (chunk.size() == 0)
              break;

            // Convert chunk to DuckDBRows and notify manager
            // Note: LocalStorage Scan returns the *new* rows (Inserts)
            // It doesn't easily distinguish inserts from updates (which are
            // delete+insert) But for now we treat everything as
            // inserts/upserts. Deletes are harder to track via pure Scan().
            // Ideally we'd access LocalStorage::GetDeleteIndex or similar.

            // For now, assume INSERTs only for O(delta) optimization.
            // If proper update/delete support is needed in O(delta), we need
            // deeper hooks or friend access.

            for (idx_t row_idx = 0; row_idx < chunk.size(); row_idx++) {
              DuckDBRow row;
              for (idx_t col_idx = 0; col_idx < chunk.ColumnCount();
                   col_idx++) {
                row.columns.push_back(chunk.GetValue(col_idx, row_idx));
              }
              manager.on_insert(table_name, row);
            }
          }
          // detect them? Verify: storage.delete_indexes has entries? if
          // (storage.GetDeleteCount(data_table) > 0) { ... } (Not exposed
          // directly)

          // Accessing delete indexes requires Friend access or custom method.
          // For now, we stick to Appends.
        }
      }

    } catch (std::exception &ex) {
      // Log error but don't crash transaction commit?
      duckdb::Printer::Print("Error in DBSP Auto-CDC: " +
                             std::string(ex.what()));
    }
  }

  // We also need to implement QueryEnd to clean up if needed, but ContextState
  // is tied to context
};

} // namespace dbsp_native

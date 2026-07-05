#pragma once

#include "dbsp_cdc.hpp"
#include "dbsp_wal_manager.hpp"
#include "duckdb.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace dbsp_native {

class DBSPContextState : public duckdb::ClientContextState {
public:
  void TransactionBegin(duckdb::MetaTransaction &transaction,
                        duckdb::ClientContext &context) override {
    if (internal_query_depth > 0) {
      return;
    }
    std::cerr << "DBSP: TransactionBegin hook called (context: " << &context
              << ")\n";
  }

  void TransactionCommit(duckdb::MetaTransaction &transaction,
                         duckdb::ClientContext &context) override {
    // Skip commits from DBSP's own internal helper connections - recursing
    // into CDCManager here deadlocks (issuing thread may hold struct_mutex_)
    if (internal_query_depth > 0) {
      return;
    }
    std::cerr << "DBSP: TransactionCommit hook called (context: " << &context
              << ")\n";

    // Only run auto-sync if enabled
    auto &manager = get_cdc_manager();
    if (!manager.is_auto_sync_enabled()) {
      std::cerr << "DBSP: Auto-sync disabled, skipping\n";
      return;
    }

    std::cerr << "DBSP: Auto-sync enabled, running sync_all\n";
    try {
      // The transaction has already committed successfully at this point.
      // We can safely read the post-commit state and pass the transaction
      // to sync_all for proper catalog access.
      manager.sync_all(context, &transaction);
      std::cerr << "DBSP: sync_all completed successfully\n";

      // Flush WAL to ensure durability
      auto &wal_manager = get_wal_manager();
      if (wal_manager.is_enabled()) {
        wal_manager.flush();
        std::cerr << "DBSP: WAL flushed\n";
      }

    } catch (const std::exception &ex) {
      std::cerr << "DBSP Auto-CDC error: " << ex.what() << "\n";
    } catch (...) {
      std::cerr << "DBSP Auto-CDC unknown error\n";
    }
  }
};

} // namespace dbsp_native

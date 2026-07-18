#pragma once
// Engine-hook consumer (SaaS fork, work item 2 of the engine-hook plan).
//
// When compiled with DBSP_ENGINE_HOOK against the patched engine
// (patches/v1.5.4-dbsp-txn-callback.patch), register_engine_hook subscribes
// to DBConfig::transaction_modification_callbacks. The engine then hands each
// committing transaction's exact per-table old/new images (full-width,
// weight -1/+1) to the callback, which converts them into a signed DuckDBZSet
// and BUFFERS it in the committing connection's DBSPContextState.
//
// Application deliberately stays out of the engine callback: the callback
// fires inside DuckTransaction::Commit (transaction-manager locks held) where
// re-entrant SQL is unsafe, and applying a delta can run SQL (deferred-
// baseline prep). The existing ClientContextState::TransactionCommit hook —
// the proven-safe apply point — drains the buffer guard-free: the engine
// reported facts, so the capture stack's prediction guards are unnecessary.
// Rollback after buffering is handled for free (TransactionRollback clears
// the capture).
//
// Without the define, register_engine_hook is a no-op returning false and the
// design-1/plan-tee capture stack keeps doing the work.

#include "dbsp_context_state.hpp"

#include <atomic>
#include <cstdint>
#include <string>

#ifdef DBSP_ENGINE_HOOK
#include "duckdb/main/attached_database.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/transaction/undo_buffer.hpp"
#endif

namespace dbsp_native {

struct EngineHookStats {
  std::atomic<uint64_t> tables_ingested{0};
  std::atomic<uint64_t> rows_ingested{0};
};

inline EngineHookStats &engine_hook_stats() {
  static EngineHookStats stats;
  return stats;
}

// engine_hook_flag()/engine_hook_active() live in dbsp_context_state.hpp —
// the capture stack consults them, and this header includes that one.

#ifdef DBSP_ENGINE_HOOK

inline void engine_cdc_to_zset(duckdb::ColumnDataCollection &cdc, int64_t weight, DuckDBZSet &out) {
  for (auto &chunk : cdc.Chunks()) {
    const duckdb::idx_t n = chunk.size();
    const duckdb::idx_t cols = chunk.ColumnCount();
    for (duckdb::idx_t i = 0; i < n; i++) {
      DuckDBRow row;
      row.columns.reserve(cols);
      for (duckdb::idx_t c = 0; c < cols; c++) {
        row.columns.push_back(chunk.GetValue(c, i));
      }
      out.insert(std::move(row), weight);
    }
  }
}

inline void ingest_engine_modifications(duckdb::ClientContext &context, duckdb::DataTableInfo &info,
                                        duckdb::TransactionModifications &mods) {
  if (internal_query_depth > 0) {
    return; // DBSP's own helper connections: never self-ingest
  }
  auto state = context.registered_state->Get<DBSPContextState>("dbsp_cdc_state");
  if (!state) {
    return;
  }
  // catalog.schema.table — must match canonical_table_key() exactly. Built
  // from DataTableInfo directly: catalog lookups inside Commit are unsafe.
  const std::string key =
      info.GetDB().GetName() + "." + info.GetSchemaName() + "." + info.GetTableName();
  if (!get_cdc_manager(context).is_table_tracked(key)) {
    return; // no view reads this table
  }
  try {
    DuckDBZSet delta;
    if (mods.old_rows) {
      engine_cdc_to_zset(*mods.old_rows, -1, delta);
    }
    if (mods.new_rows) {
      engine_cdc_to_zset(*mods.new_rows, +1, delta);
    }
    engine_hook_stats().tables_ingested.fetch_add(1, std::memory_order_relaxed);
    engine_hook_stats().rows_ingested.fetch_add(delta.size(), std::memory_order_relaxed);
    state->engine_buffer_delta(key, std::move(delta));
  } catch (...) {
    // Conversion failed: the buffered picture is incomplete. Poison the
    // transaction's capture so commit reconciles by scan instead of applying
    // a partial delta. Never let an exception escape into the engine commit.
    state->engine_mark_unknown();
  }
}

inline bool register_engine_hook(duckdb::DatabaseInstance &db) {
  duckdb::TransactionModificationCallback cb;
  cb.on_commit = [](duckdb::ClientContext &context, duckdb::DataTableInfo &info,
                    duckdb::TransactionModifications &mods) {
    ingest_engine_modifications(context, info, mods);
  };
  // Side registry keyed by instance (ABI-neutral engine surface); the engine
  // erases the entry in ~DatabaseInstance.
  duckdb::RegisterTxnModificationCallback(db, std::move(cb));
  engine_hook_flag().store(true, std::memory_order_relaxed);
  return true;
}

#else // !DBSP_ENGINE_HOOK — stock engine: capture stack stays in charge

inline bool register_engine_hook(duckdb::DatabaseInstance &) {
  return false;
}

#endif

} // namespace dbsp_native

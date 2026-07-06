#pragma once

// Tracks which ClientContexts belong to DBSP-internal Connections (view
// PlanKeepAlive connections) so the extension's OnConnectionClosed callback
// can tell user connections apart from our own, and release all DBSP state
// for a DatabaseInstance once the last *user* connection to it closes.
//
// Why this exists: materialized views hold internal Connections whose
// ClientContexts own a shared_ptr<DatabaseInstance>. The CDCManager singleton
// is deliberately leaked (see get_cdc_manager), so without an explicit
// release those Connections pin the instance forever. DuckDB's
// DBInstanceCache busy-spins waiting for a dying instance's cache entry to
// expire, so a same-process close + reopen of the same database file would
// never return.

#include <mutex>
#include <unordered_map>

namespace duckdb {
class ClientContext;
class DatabaseInstance;
} // namespace duckdb

namespace dbsp_native {

class InstanceRegistry {
public:
  void register_internal(duckdb::ClientContext *ctx,
                         duckdb::DatabaseInstance *db) {
    std::lock_guard<std::mutex> lock(mutex_);
    internal_ctxs_[ctx] = db;
  }

  void unregister_internal(duckdb::ClientContext *ctx) {
    std::lock_guard<std::mutex> lock(mutex_);
    internal_ctxs_.erase(ctx);
  }

  bool is_internal(duckdb::ClientContext *ctx) {
    std::lock_guard<std::mutex> lock(mutex_);
    return internal_ctxs_.count(ctx) > 0;
  }

  size_t internal_count(duckdb::DatabaseInstance *db) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (auto &entry : internal_ctxs_) {
      if (entry.second == db) {
        n++;
      }
    }
    return n;
  }

private:
  std::mutex mutex_;
  std::unordered_map<duckdb::ClientContext *, duckdb::DatabaseInstance *>
      internal_ctxs_;
};

// Holds only raw pointers and a mutex (no DuckDB objects), so a plain
// function-local static is safe here — nothing DuckDB-owned runs at static
// teardown, unlike CDCManager.
inline InstanceRegistry &get_instance_registry() {
  static InstanceRegistry registry;
  return registry;
}

} // namespace dbsp_native

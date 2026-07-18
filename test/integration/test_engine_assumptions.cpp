// Engine-assumption canaries. Every capture tier stands on empirically
// probed DuckDB v1.5.4 behavior that no API contract guarantees. Each
// TEST_CASE here pins ONE assumption by name; on an engine upgrade these
// fail FIRST, with a readable message, instead of surfacing as mystery
// view corruption in the differential suites.
//
// Canary tables are deliberately UNTRACKED: the plan tee only arms on
// tracked tables, so ExtractPlan returns raw engine shapes here.

#include "../test_helpers.hpp"
#include "catch.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_update.hpp"

using namespace dbsp_test;
using namespace duckdb;

namespace {

// Observations taken INSIDE an optimizer extension — the exact pipeline
// point where the production tee runs (post-optimizers, pre-resolver).
// ExtractPlan cannot stand in for this: it returns the RESOLVED plan,
// where these expressions have already become BOUND_REF.
struct CanaryObservations {
  bool saw_delete = false;
  bool delete_ref_is_bcr = false;
  bool saw_update = false;
  bool update_child_is_proj = false;
  bool update_refs_are_bcr = false;
  void reset() { *this = CanaryObservations(); }
};
CanaryObservations g_canary_obs;

void canary_observe(OptimizerExtensionInput &,
                    duckdb::unique_ptr<LogicalOperator> &plan) {
  std::function<void(LogicalOperator &)> walk = [&](LogicalOperator &op) {
    if (op.type == LogicalOperatorType::LOGICAL_DELETE) {
      g_canary_obs.saw_delete = true;
      g_canary_obs.delete_ref_is_bcr =
          !op.expressions.empty() &&
          op.expressions[0]->GetExpressionClass() ==
              ExpressionClass::BOUND_COLUMN_REF;
    }
    if (op.type == LogicalOperatorType::LOGICAL_UPDATE) {
      g_canary_obs.saw_update = true;
      g_canary_obs.update_child_is_proj =
          !op.children.empty() &&
          op.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION;
      g_canary_obs.update_refs_are_bcr = !op.expressions.empty();
      for (auto &e : op.expressions) {
        if (e->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
          g_canary_obs.update_refs_are_bcr = false;
        }
      }
    }
    for (auto &c : op.children) {
      walk(*c);
    }
  };
  walk(*plan);
}

struct CanaryFixture {
  DuckDBTestHarness db;
  CanaryFixture() {
    OptimizerExtension probe;
    probe.optimize_function = canary_observe;
    OptimizerExtension::Register(DBConfig::GetConfig(db.instance()),
                                 std::move(probe));
    g_canary_obs.reset();
    db.exec("CREATE TABLE ca (id INTEGER, val INTEGER, name VARCHAR)");
    db.exec("CREATE TABLE cpk (id INTEGER PRIMARY KEY, val INTEGER)");
    db.exec("INSERT INTO ca VALUES (1, 10, 'x'), (2, 20, 'y')");
  }
  unique_ptr<LogicalOperator> plan(const std::string &sql) {
    auto p = db.conn().ExtractPlan(sql);
    REQUIRE(p);
    return p;
  }
  static LogicalOperator *find(LogicalOperator *op, LogicalOperatorType t) {
    if (op->type == t) {
      return op;
    }
    for (auto &c : op->children) {
      if (auto hit = find(c.get(), t)) {
        return hit;
      }
    }
    return nullptr;
  }
};

} // namespace

TEST_CASE("canary: DML row references are BOUND_COLUMN_REF at optimize time",
          "[engine_assumptions]") {
  // The tee maps bindings to child positions; BOUND_REF indexes appear
  // only after the ColumnBindingResolver. If this flips, every tee
  // injection silently stops arming.
  CanaryFixture fx;
  g_canary_obs.reset();
  fx.db.exec("DELETE FROM ca WHERE id = 2");
  REQUIRE(g_canary_obs.saw_delete);
  REQUIRE(g_canary_obs.delete_ref_is_bcr);
}

TEST_CASE("canary: UPDATE child is a projection with the rowid LAST",
          "[engine_assumptions]") {
  // PhysicalUpdate reads the rowid from chunk.ColumnCount()-1 BY
  // POSITION; the UPDATE tee projects widened columns back out to keep
  // the rowid last. If the binder stops appending the rowid last, teed
  // updates would corrupt silently without this canary.
  CanaryFixture fx;
  // optimizer-time shape (where the tee runs): child is a projection,
  // SET refs are BOUND_COLUMN_REF
  // note: a provably-empty predicate (id = 999 vs stats max 2) folds the
  // whole child to LOGICAL_EMPTY_RESULT — the tee declines that shape;
  // observe a satisfiable one
  g_canary_obs.reset();
  fx.db.exec("UPDATE ca SET val = val + 1 WHERE id = 1");
  REQUIRE(g_canary_obs.saw_update);
  REQUIRE(g_canary_obs.update_child_is_proj);
  REQUIRE(g_canary_obs.update_refs_are_bcr);
  // resolved-plan shape: projection still there, rowid last and BIGINT
  auto p = fx.plan("UPDATE ca SET val = val + 1 WHERE id = 1");
  auto upd_op = CanaryFixture::find(p.get(), LogicalOperatorType::LOGICAL_UPDATE);
  REQUIRE(upd_op);
  auto &upd = upd_op->Cast<LogicalUpdate>();
  REQUIRE(upd.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION);
  auto &proj = upd.children[0]->Cast<LogicalProjection>();
  REQUIRE_FALSE(proj.expressions.empty());
  REQUIRE(proj.expressions.back()->return_type.id() == LogicalTypeId::BIGINT);
  REQUIRE(upd.columns.size() == upd.expressions.size());
}

TEST_CASE("canary: projection-pushdown GET exposes only projection_ids",
          "[engine_assumptions]") {
  // Widening appends to column_ids AND projection_ids; if the binding
  // rule changes, appended old-image columns become invisible and the
  // resolver throws 'Failed to bind column reference'.
  CanaryFixture fx;
  auto p = fx.plan("SELECT name FROM ca WHERE val > 5");
  auto get_op = CanaryFixture::find(p.get(), LogicalOperatorType::LOGICAL_GET);
  REQUIRE(get_op);
  auto &get = get_op->Cast<LogicalGet>();
  if (!get.projection_ids.empty()) {
    auto bindings = get.GetColumnBindings();
    REQUIRE(bindings.size() == get.projection_ids.size());
    for (idx_t i = 0; i < bindings.size(); i++) {
      REQUIRE(bindings[i].column_index == get.projection_ids[i]);
    }
  }
  // and appending must widen the bindings
  auto before = get.GetColumnBindings().size();
  get.AddColumnId(0);
  if (!get.projection_ids.empty()) {
    get.projection_ids.push_back(get.GetColumnIds().size() - 1);
  }
  REQUIRE(get.GetColumnBindings().size() == before + 1);
}

TEST_CASE("canary: update_is_del_and_insert fires exactly on indexed SET",
          "[engine_assumptions]") {
  // Design 1 rejects indexed SET columns because rowids are unstable;
  // if the engine changes when updates become delete+re-append, the
  // rowid re-verification guard starts passing/failing wrongly.
  CanaryFixture fx;
  auto p1 = fx.plan("UPDATE cpk SET id = id + 1");
  auto &u1 = CanaryFixture::find(p1.get(), LogicalOperatorType::LOGICAL_UPDATE)
                 ->Cast<LogicalUpdate>();
  REQUIRE(u1.update_is_del_and_insert); // PK column
  auto p2 = fx.plan("UPDATE cpk SET val = val + 1");
  auto &u2 = CanaryFixture::find(p2.get(), LogicalOperatorType::LOGICAL_UPDATE)
                 ->Cast<LogicalUpdate>();
  REQUIRE_FALSE(u2.update_is_del_and_insert); // non-indexed column
}

TEST_CASE("canary: full-insert child is table-width, defaults use the map",
          "[engine_assumptions]") {
  // The INSERT tee assumes: no column list => child already emits
  // table-order/table-width rows; a partial list marks missing columns
  // INVALID in column_index_map (resolved by a PHYSICAL projection the
  // tee cannot see past).
  CanaryFixture fx;
  auto p1 = fx.plan("INSERT INTO ca SELECT id + 100, val, name FROM ca");
  auto &i1 = CanaryFixture::find(p1.get(), LogicalOperatorType::LOGICAL_INSERT)
                 ->Cast<LogicalInsert>();
  REQUIRE(i1.column_index_map.empty());
  REQUIRE(i1.children[0]->GetColumnBindings().size() == 3);
  auto p2 = fx.plan("INSERT INTO ca (id) VALUES (300)");
  auto &i2 = CanaryFixture::find(p2.get(), LogicalOperatorType::LOGICAL_INSERT)
                 ->Cast<LogicalInsert>();
  REQUIRE_FALSE(i2.column_index_map.empty());
  bool has_invalid = false;
  for (auto &col : i2.table.GetColumns().Physical()) {
    if (i2.column_index_map[col.Physical()] == DConstants::INVALID_INDEX) {
      has_invalid = true;
    }
  }
  REQUIRE(has_invalid);
}

TEST_CASE("canary: autocommit hook ordering", "[engine_assumptions]") {
  // The fold split (autocommit folds at QueryBegin, explicit txn at
  // QueryEnd, QueryEnd skips autocommit) depends on: an autocommit
  // statement having an ACTIVE transaction with IsAutoCommit() true at
  // BOTH QueryBegin and QueryEnd, and its TransactionCommit hook firing
  // mid-statement (before QueryEnd).
  struct ProbeState : public ClientContextState {
    bool begin_active = false, begin_auto = false;
    bool end_active = false, end_auto = false;
    int commit_before_end = -1; // 1 = commit hook saw no QueryEnd yet
    bool saw_end = false;
    void QueryBegin(ClientContext &context) override {
      begin_active = context.transaction.HasActiveTransaction();
      begin_auto = context.transaction.IsAutoCommit();
    }
    void QueryEnd(ClientContext &context,
                  optional_ptr<ErrorData> error) override {
      saw_end = true;
      end_active = context.transaction.HasActiveTransaction();
      end_auto = context.transaction.IsAutoCommit();
    }
    void TransactionCommit(MetaTransaction &, ClientContext &) override {
      commit_before_end = saw_end ? 0 : 1;
    }
  };
  CanaryFixture fx;
  auto probe = fx.db.conn().context->registered_state
                   ->GetOrCreate<ProbeState>("dbsp_canary_probe");
  fx.db.exec("INSERT INTO ca VALUES (500, 5, 'p')");
  // QueryBegin: transaction active, autocommit — this is why the fold
  // and all design-1 captures run there
  REQUIRE(probe->begin_active);
  REQUIRE(probe->begin_auto);
  // TransactionCommit fires MID-statement, before QueryEnd — captures
  // apply there
  REQUIRE(probe->commit_before_end == 1);
  // QueryEnd: the transaction is GONE — nothing may fold or resolve
  // catalog entries here for autocommit statements
  REQUIRE_FALSE(probe->end_active);
}

// D2 spike: OptimizerExtension DML tee (include/dbsp_plan_tee.hpp).
// Proves the two primitives the design needs on pinned DuckDB v1.5.4:
// the optimizer hook can rewrite a bound LogicalDelete's child, and the
// injected extension operator observes the deleted rowids at execution
// time without changing the statement's behavior.

#include "../test_helpers.hpp"
#include "catch.hpp"
#include "dbsp_plan_tee.hpp"
#include <cstdlib>

using namespace dbsp_test;

TEST_CASE("D2 spike: tee observes DELETE child rowids",
          "[integration][plan_tee]") {
  setenv("DBSP_TEE_SPIKE", "1", 1);
  DuckDBTestHarness db;
  db.createTable("tee_t", "id INT, val INT",
                 {"(1, 10)", "(2, 20)", "(3, 30)", "(4, 40)"});

  auto &buffer = dbsp_native::tee_spike_buffer();
  buffer.clear();

  db.exec("DELETE FROM tee_t WHERE val > 15");

  // the tee saw exactly the deleted rows...
  REQUIRE(buffer.count() == 3);
  // ...and the DELETE itself still worked
  auto res = db.query("SELECT COUNT(*) FROM tee_t");
  REQUIRE(res->GetValue(0, 0).GetValue<int64_t>() == 1);

  // multi-statement sanity: rollback keeps table intact, tee still fires
  buffer.clear();
  db.exec("BEGIN");
  db.exec("DELETE FROM tee_t");
  db.exec("ROLLBACK");
  REQUIRE(buffer.count() == 1);
  res = db.query("SELECT COUNT(*) FROM tee_t");
  REQUIRE(res->GetValue(0, 0).GetValue<int64_t>() == 1);

  unsetenv("DBSP_TEE_SPIKE");
  buffer.clear();
}

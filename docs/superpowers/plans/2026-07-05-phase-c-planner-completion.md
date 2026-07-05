# Phase C: Planner Completion & Parser Retirement — Implementation Plan

> **STATUS: COMPLETE (2026-07-05).** All tasks executed; see CHANGELOG.md
> Phase C entries. Notable deviations: the IR optimizer lives in
> `dbsp_plan_translator.hpp` (`plan_ir` namespace) rather than a separate
> header (avoids a header cycle around PlanOpSpec); rewritten pushdown
> expressions live in `PlanKeepAlive::rewritten_exprs`, not spec-local
> storage (lifetime). The C5 inventory gate found three unplanned gaps —
> views-on-views binding, COUNT(*) rowid scans, DECIMAL SUM — all closed
> before deletion.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the planner frontend's remaining gaps (ORDER BY/LIMIT, recursive CTEs, DISTINCT ON), port the optimizer to circuit-IR rewrites, then delete the bespoke SQL parser.

**Architecture:** All SELECT translation goes through `PlanTranslator` (DuckDB `ExtractPlan`, optimizer off → canonical plans). New logical operators map to either fine-grained circuit nodes or proven `Native*` views wrapped in `EmbeddedViewNode` (the B4 window pattern). Once the planner covers everything the parser did, `create_view`'s fallback branch and `dbsp_sql_parser.hpp`/`dbsp_optimizer.hpp` are deleted.

**Tech Stack:** C++17, DuckDB v1.5.4 submodule (pinned), Catch2 v2, CMake.

## Global Constraints

- DuckDB pinned at v1.5.4; `duckdb::vector` (not `std::vector`) for DataChunk APIs.
- `PlanTranslator::translate` callers hold `InternalQueryGuard` (already true; don't add queries outside it).
- Never reverse lock order: `struct_mutex_` → `table_locks_[name]` → `view_mutex_`.
- Build/test: `cd test/build_test && make -j8 && ctest --output-on-failure` (warm run ≈ 2–4 s; first run ~40 s is cold-start noise).
- Plan-shape probes: build `probe.cpp` in the session scratchpad against `test/build_test` static libs (no system duckdb CLI — wrong version).
- After every task: update `docs/`, `README.md`, header comments, and the architecture diagram per project CLAUDE.md before calling the task complete.
- Baseline: 37/37 tests green on `main` (verify before starting).

## Verified plan shapes (probed 2026-07-05, optimizer off)

```
ORDER BY:            ORDER_BY → PROJECTION → GET
ORDER BY + LIMIT:    LIMIT → ORDER_BY → PROJECTION → GET
LIMIT/OFFSET only:   LIMIT → PROJECTION → GET
ORDER BY dropped col: PROJECTION(#0) → LIMIT → ORDER_BY(#1) → PROJECTION(id,val) → GET
WITH RECURSIVE:      CTE(idx 0) → [REC_CTE(idx 1) → {anchor, step w/ CTE_SCAN(idx 1)}, main w/ CTE_SCAN(idx 0)]
DISTINCT ON:         PROJECTION(#0,#1) → ORDER_BY → DISTINCT(targets #2) → PROJECTION → GET
```

Key API facts:
- `LogicalOrder { vector<BoundOrderByNode> orders; vector<idx_t> projection_map; }` — order exprs are `BoundReferenceExpression` after ColumnBindingResolver (ExtractPlan runs it).
- `LogicalLimit { BoundLimitNode limit_val, offset_val; }` — `Type()` ∈ {UNSET, CONSTANT_VALUE, CONSTANT_PERCENTAGE, EXPRESSION_*}; only UNSET/CONSTANT_VALUE supported; `GetConstantValue()`.
- `LogicalRecursiveCTE : LogicalCTE { bool union_all; vector<unique_ptr<Expression>> key_targets; }` — children[0]=anchor, children[1]=recursive step; the step's `CTE_SCAN` has `cte_index == table_index` of the REC_CTE. Reject non-empty `key_targets` (USING KEY).
- `dbsp_query` consumes `view->scan(cb)`; `NativeSortView::scan` iterates its sorted multiset — ordered output only flows through `scan()`, never `get_result()`.

---

### Task C1: ORDER BY / LIMIT through the planner

**Files:**
- Modify: `include/dbsp_plan_translator.hpp` (Walker + PlanOpSpec + build + PlannedCircuitView::scan)
- Test: `test/integration/test_planner_frontend.cpp`

**Interfaces:**
- Produces: `PlanOpSpec::Kind::SORT_LIMIT` with fields `std::vector<NativeSortView::SortColumn> sort_columns; int64_t limit = -1; int64_t offset = 0; std::vector<duckdb::idx_t> project_idxs; bool presentation_root = false;`
- Produces: `PlannedCircuitView::scan()` override delegating to `ordered_view_` (`NativeMaterializedView*`, set when the root spec is a presentation SORT_LIMIT).

**Design:** One spec kind covers the whole top-of-plan stack `[PROJECTION(pure #refs)]? [LIMIT]? ORDER_BY` and `[PROJECTION(pure #refs)]? LIMIT`. Build maps it to `EmbeddedViewNode` wrapping `NativeLimitView` (when limit ≥ 0 or offset > 0) or `NativeSortView` (sort only). The trailing pure-column-ref PROJECTION becomes the view's `ProjectFn`, so sort columns dropped from the output still order it (parser parity). If the projection above ORDER_BY has non-column-ref expressions, translate it as a normal MAP above the SORT_LIMIT; content stays correct, scan order is not preserved (matches SQL: a scan of materialized state has no order guarantee). Membership is unchanged by sort alone; LIMIT/OFFSET change membership and `NativeLimitView`'s delta logic already handles that incrementally.

- [ ] **Step 1: Write failing differential tests** in `test_planner_frontend.cpp` (same harness style as existing B-tests):

```cpp
TEST_CASE("planner: ORDER BY view has sorted scan output", "[planner][sort]") {
  PlannerFixture fx; // existing fixture: db + cdc + planner ON
  fx.exec("CREATE TABLE st(id INT, val INT)");
  fx.exec("INSERT INTO st VALUES (1, 30), (2, 10), (3, 20)");
  REQUIRE(fx.create_view("v_sorted", "SELECT id, val FROM st ORDER BY val DESC"));
  REQUIRE(fx.planner_built("v_sorted")); // must NOT have fallen back
  auto rows = fx.query_rows("v_sorted"); // via dbsp_query → scan()
  REQUIRE(rows.size() == 3);
  CHECK(rows[0][1] == 30); CHECK(rows[1][1] == 20); CHECK(rows[2][1] == 10);
  fx.exec("INSERT INTO st VALUES (4, 25)"); fx.sync();
  rows = fx.query_rows("v_sorted");
  REQUIRE(rows.size() == 4);
  CHECK(rows[1][1] == 25); // incremental insert lands in order
}

TEST_CASE("planner: LIMIT with ORDER BY maintains top-k incrementally", "[planner][limit]") {
  PlannerFixture fx;
  fx.exec("CREATE TABLE lt(id INT, val INT)");
  fx.exec("INSERT INTO lt VALUES (1, 30), (2, 10), (3, 20)");
  REQUIRE(fx.create_view("v_top2", "SELECT id FROM lt ORDER BY val DESC LIMIT 2"));
  REQUIRE(fx.planner_built("v_top2"));
  CHECK(fx.query_rows("v_top2").size() == 2); // ids 1,3 (vals 30,20)
  fx.exec("INSERT INTO lt VALUES (4, 40)"); fx.sync();
  auto rows = fx.query_rows("v_top2");
  REQUIRE(rows.size() == 2);
  CHECK(rows[0][0] == 4); CHECK(rows[1][0] == 1); // 40, 30 win; sort col dropped from output
  fx.exec("DELETE FROM lt WHERE id = 4"); fx.sync();
  rows = fx.query_rows("v_top2");
  CHECK(rows[0][0] == 1); CHECK(rows[1][0] == 3); // deletion re-admits id 3
}

TEST_CASE("planner: bare LIMIT/OFFSET", "[planner][limit]") {
  PlannerFixture fx;
  fx.exec("CREATE TABLE bt(id INT)");
  fx.exec("INSERT INTO bt VALUES (1), (2), (3), (4), (5)");
  REQUIRE(fx.create_view("v_lim", "SELECT id FROM bt LIMIT 3 OFFSET 1"));
  REQUIRE(fx.planner_built("v_lim"));
  CHECK(fx.query_rows("v_lim").size() == 3);
}

TEST_CASE("planner: LIMIT n% rejected with E110", "[planner][limit]") {
  PlannerFixture fx;
  fx.exec("CREATE TABLE pt(id INT)");
  // percentage limit unsupported → planner error (still creates via fallback
  // until C5; assert the translator's error, not create_view failure)
  auto err = fx.translate_error("SELECT id FROM pt LIMIT 10%");
  CHECK(err.find("DBSP-E110") != std::string::npos);
}
```

If the fixture lacks `planner_built` / `translate_error` helpers, add them: `planner_built` checks `dynamic_cast<PlannedCircuitView*>(manager.get_view(name)) != nullptr` (expose a `is_planner_view(name)` accessor on CDCManager if the view pointer type isn't reachable); `translate_error` calls `PlanTranslator::translate` directly under an `InternalQueryGuard` and returns `.error`.

- [ ] **Step 2: Run to verify failure.** `cd test/build_test && make -j8 && ctest -R planner --output-on-failure` — new tests FAIL (E110 "logical operator ORDER_BY"/"LIMIT" → fallback path lacks `planner_built`).

- [ ] **Step 3: Implement.** In `dbsp_plan_translator.hpp`:

1. Extend `PlanOpSpec` (fields listed under Interfaces) and add `SORT_LIMIT` to `Kind`.
2. Add includes: `duckdb/planner/operator/logical_order.hpp`, `logical_limit.hpp`.
3. Walker: add `visit_order` / `visit_limit` and a stack-folding helper. Sketch:

```cpp
// In Walker::visit switch:
case duckdb::LogicalOperatorType::LOGICAL_ORDER_BY:
  return visit_order(op.Cast<duckdb::LogicalOrder>(), /*limit=*/-1, /*offset=*/0);
case duckdb::LogicalOperatorType::LOGICAL_LIMIT:
  return visit_limit(op.Cast<duckdb::LogicalLimit>());

SpecPtr visit_limit(duckdb::LogicalLimit &op) {
  int64_t limit = -1, offset = 0;
  using LT = duckdb::LimitNodeType;
  if (op.limit_val.Type() == LT::CONSTANT_VALUE) {
    limit = (int64_t)op.limit_val.GetConstantValue();
  } else if (op.limit_val.Type() != LT::UNSET) {
    return unsupported("non-constant LIMIT");
  }
  if (op.offset_val.Type() == LT::CONSTANT_VALUE) {
    offset = (int64_t)op.offset_val.GetConstantValue();
  } else if (op.offset_val.Type() != LT::UNSET) {
    return unsupported("non-constant OFFSET");
  }
  auto &child = *op.children[0];
  if (child.type == duckdb::LogicalOperatorType::LOGICAL_ORDER_BY) {
    return visit_order(child.Cast<duckdb::LogicalOrder>(), limit, offset);
  }
  auto child_spec = visit(child);
  if (!child_spec) return nullptr;
  return make_sort_limit(std::move(child_spec), {}, limit, offset);
}

SpecPtr visit_order(duckdb::LogicalOrder &op, int64_t limit, int64_t offset) {
  if (!op.projection_map.empty()) return unsupported("ORDER BY with projection map");
  std::vector<NativeSortView::SortColumn> cols;
  for (auto &o : op.orders) {
    if (o.expression->GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF)
      return unsupported("ORDER BY expression (use a plain column)");
    auto &ref = o.expression->Cast<duckdb::BoundReferenceExpression>();
    cols.push_back({ref.index,
                    o.type != duckdb::OrderType::DESCENDING,
                    o.null_order == duckdb::OrderByNullType::NULLS_FIRST});
  }
  auto child_spec = visit(*op.children[0]);
  if (!child_spec) return nullptr;
  return make_sort_limit(std::move(child_spec), std::move(cols), limit, offset);
}

SpecPtr make_sort_limit(SpecPtr child, std::vector<NativeSortView::SortColumn> cols,
                        int64_t limit, int64_t offset) {
  auto spec = std::make_unique<PlanOpSpec>();
  spec->kind = PlanOpSpec::Kind::SORT_LIMIT;
  spec->sort_columns = std::move(cols);
  spec->limit = limit; spec->offset = offset;
  spec->children.push_back(std::move(child));
  // columns unchanged (pass-through); Walker::columns already reflect child
  return spec;
}
```

4. Trailing-projection folding in `visit_projection`: when every expression is a `BoundReferenceExpression` **and** the child operator is ORDER_BY or LIMIT, translate the child first and, if it came back as a SORT_LIMIT spec, set `project_idxs` on it (and recompute `columns` from the projection) instead of stacking a MAP_EXPR. Non-ref expressions: existing MAP_EXPR path unchanged.
5. `PlanTranslator::translate`: after the walk, if `root->kind == SORT_LIMIT`, set `root->presentation_root = true`.
6. `PlannedCircuitView::build` case:

```cpp
case PlanOpSpec::Kind::SORT_LIMIT: {
  OutputFn child = build(*spec.children[0], keep_alive);
  TableSchema vschema; vschema.table_name = name_ + "_sortlimit";
  // result columns after optional projection (schema_ already final for root)
  NativeSortView::ProjectFn project = nullptr;
  if (!spec.project_idxs.empty()) {
    auto idxs = spec.project_idxs;
    project = [idxs](const DuckDBRow &row) {
      DuckDBRow out; out.columns.reserve(idxs.size());
      for (auto i : idxs)
        out.columns.push_back(i < row.columns.size() ? row.columns[i] : duckdb::Value());
      return out;
    };
  }
  std::unique_ptr<NativeMaterializedView> view;
  if (spec.limit >= 0 || spec.offset > 0) {
    view = std::make_unique<NativeLimitView>(
        name_ + "_limit", "", EmbeddedViewNode::kInputName, vschema,
        spec.limit, spec.offset, spec.sort_columns, project);
  } else {
    view = std::make_unique<NativeSortView>(
        name_ + "_sort", "", EmbeddedViewNode::kInputName, vschema,
        spec.sort_columns, project);
  }
  if (spec.presentation_root) ordered_view_ = view.get();
  auto *node = circuit_.add_node(std::make_unique<EmbeddedViewNode>(
      circuit_.next_node_id(), std::move(child), std::move(view)));
  return [node]() -> const DuckDBZSet & { return node->output(); };
}
```

7. `PlannedCircuitView`: member `NativeMaterializedView *ordered_view_ = nullptr;` and

```cpp
void scan(const std::function<void(const DuckDBRow &, Weight)> &cb) const override {
  if (ordered_view_) { ordered_view_->scan(cb); return; }
  NativeMaterializedView::scan(cb);
}
```

(Verify the base declares `scan` virtual with this exact signature before writing; match it.)

- [ ] **Step 4: Run tests.** Full suite green (`ctest --output-on-failure`), including all pre-existing tests both with planner ON.
- [ ] **Step 5: Docs + commit.** Update translator header comment (scope: add C1 line), `docs/PHASE_B_PLAN.md` status note or successor section, `docs/API.md` if it documents ORDER BY/LIMIT as parser-only. Commit: `feat: Phase C1 - ORDER BY/LIMIT through the planner frontend`.

---

### Task C2: Recursive CTEs through the planner

**Files:**
- Modify: `include/dbsp_plan_translator.hpp`
- Test: `test/integration/test_planner_frontend.cpp`

**Interfaces:**
- Produces: `PlanOpSpec::Kind::REC_CTE` (children `{anchor, step}`, `bool union_all`, `cte_index`) and `Kind::SOURCE` reuse with sentinel table `"__rec_cte_<index>__"` for the step's self-reference.
- Produces: `PlanRecursiveNode : dbsp::Node` — fixed-point driver owning the step subtree as an inner `PlannedCircuitView`.

**Design:** Mirror `NativeRecursiveView`'s proven algorithm, but anchor and step are planner-built. Anchor subtree builds inline in the outer circuit. Step subtree becomes an inner `PlannedCircuitView` whose sources are the sentinel (self-reference) plus any base tables; `apply_changes(name, delta)` already routes by source name, so the fixed-point loop just feeds the sentinel repeatedly. Improvements over the parser version: UNION dedup state (`accumulated_`) persists across `step()` calls (correct dedup across successive table deltas), and multi-table recursive steps work (parser was single-source). Parity limitation kept and documented: negative weights (deletions) are ignored inside the fixed point.

- [ ] **Step 1: Write failing tests:**

```cpp
TEST_CASE("planner: recursive CTE UNION ALL", "[planner][recursive]") {
  PlannerFixture fx;
  fx.exec("CREATE TABLE seed(id INT)");
  fx.exec("INSERT INTO seed VALUES (1)");
  REQUIRE(fx.create_view("v_rec",
      "WITH RECURSIVE r AS (SELECT id FROM seed UNION ALL "
      "SELECT id+1 FROM r WHERE id < 5) SELECT * FROM r"));
  REQUIRE(fx.planner_built("v_rec"));
  CHECK(fx.query_rows("v_rec").size() == 5); // 1..5
  fx.exec("INSERT INTO seed VALUES (4)"); fx.sync();
  CHECK(fx.query_rows("v_rec").size() == 7); // + {4,5}
}

TEST_CASE("planner: recursive CTE UNION dedups across deltas", "[planner][recursive]") {
  PlannerFixture fx;
  fx.exec("CREATE TABLE seed2(id INT)");
  fx.exec("INSERT INTO seed2 VALUES (1)");
  REQUIRE(fx.create_view("v_rec_u",
      "WITH RECURSIVE r AS (SELECT id FROM seed2 UNION "
      "SELECT id+1 FROM r WHERE id < 5) SELECT * FROM r"));
  REQUIRE(fx.planner_built("v_rec_u"));
  CHECK(fx.query_rows("v_rec_u").size() == 5);
  fx.exec("INSERT INTO seed2 VALUES (3)"); fx.sync();
  CHECK(fx.query_rows("v_rec_u").size() == 5); // 3,4,5 already present
}

TEST_CASE("planner: recursive CTE joining a second table", "[planner][recursive]") {
  PlannerFixture fx; // transitive closure — parser path could NOT do this
  fx.exec("CREATE TABLE edges(src INT, dst INT)");
  fx.exec("INSERT INTO edges VALUES (1,2), (2,3), (3,4)");
  REQUIRE(fx.create_view("v_reach",
      "WITH RECURSIVE reach AS (SELECT src, dst FROM edges WHERE src = 1 "
      "UNION SELECT r.src, e.dst FROM reach r JOIN edges e ON r.dst = e.src) "
      "SELECT * FROM reach"));
  REQUIRE(fx.planner_built("v_reach"));
  CHECK(fx.query_rows("v_reach").size() == 3); // (1,2)(1,3)(1,4)
  fx.exec("INSERT INTO edges VALUES (4,5)"); fx.sync();
  CHECK(fx.query_rows("v_reach").size() == 4); // + (1,5)
}
```

- [ ] **Step 2: Run — FAIL** (E110 "recursive CTE — handled by the parser frontend" → fallback; `planner_built` false, and the multi-table case fails outright on the parser too).

- [ ] **Step 3: Implement.**

1. Include `duckdb/planner/operator/logical_recursive_cte.hpp`.
2. Walker state: `std::set<duckdb::idx_t> recursive_cte_indexes;` — `visit_cte_ref` first checks it:

```cpp
SpecPtr visit_cte_ref(duckdb::LogicalCTERef &op) {
  if (recursive_cte_indexes.count(op.cte_index)) {
    auto spec = std::make_unique<PlanOpSpec>();
    spec->kind = PlanOpSpec::Kind::SOURCE;
    spec->table = "__rec_cte_" + std::to_string(op.cte_index) + "__";
    // columns: set from op.chunk_types / bound_columns as existing code does
    ...
    return spec;
  }
  ... // existing materialized-CTE path unchanged
}
```

3. `visit` case replaces the E110:

```cpp
case duckdb::LogicalOperatorType::LOGICAL_RECURSIVE_CTE: {
  auto &rec = op.Cast<duckdb::LogicalRecursiveCTE>();
  if (!rec.key_targets.empty())
    return unsupported("recursive CTE USING KEY");
  recursive_cte_indexes.insert(rec.table_index);
  auto anchor = visit(*rec.children[0]);
  if (!anchor) return nullptr;
  auto anchor_cols = columns; // step walk will clobber; REC output = anchor layout
  auto step = visit(*rec.children[1]);
  if (!step) return nullptr;
  columns = std::move(anchor_cols);
  auto spec = std::make_unique<PlanOpSpec>();
  spec->kind = PlanOpSpec::Kind::REC_CTE;
  spec->set_op = rec.union_all ? PlanOpSpec::SetOp::UNION_ALL : PlanOpSpec::SetOp::UNION;
  spec->cte_index = rec.table_index;
  spec->children.push_back(std::move(anchor));
  spec->children.push_back(std::move(step));
  return spec;
}
```

4. `PlanRecursiveNode` (place next to `EmbeddedViewNode`):

```cpp
// Fixed-point driver for WITH RECURSIVE. The anchor is computed inline in the
// outer circuit; the recursive step runs as an inner PlannedCircuitView whose
// self-reference is a source named by `sentinel`. Deletions (negative
// weights) are ignored inside the fixed point — parity with the retired
// parser implementation.
class PlanRecursiveNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;

  PlanRecursiveNode(dbsp::NodeId id, InputFn anchor,
                    std::unique_ptr<NativeMaterializedView> step_view,
                    std::string sentinel, bool union_all,
                    std::vector<std::pair<std::string, InputFn>> base_inputs,
                    size_t max_iterations = 1000)
      : dbsp::Node(id, "plan_recursive"), anchor_(std::move(anchor)),
        step_view_(std::move(step_view)), sentinel_(std::move(sentinel)),
        union_all_(union_all), base_inputs_(std::move(base_inputs)),
        max_iterations_(max_iterations) {}

  void step() override {
    output_.clear();
    DuckDBZSet current = anchor_();
    for (auto &[table, fn] : base_inputs_) {          // step reacts to base deltas
      const DuckDBZSet &d = fn();
      if (d.empty()) continue;
      step_view_->apply_changes(table, d);
      for (const auto &[row, w] : step_view_->get_delta()) current.insert(row, w);
    }
    DuckDBZSet frontier;
    for (const auto &[row, w] : current) {            // admit positives
      if (w <= 0) continue;
      admit(row, w, frontier);
    }
    size_t iter = 0;
    while (!frontier.empty() && iter++ < max_iterations_) {
      step_view_->apply_changes(sentinel_, frontier);
      DuckDBZSet next;
      for (const auto &[row, w] : step_view_->get_delta()) {
        if (w <= 0) continue;
        admit(row, w, next);
      }
      frontier = std::move(next);
    }
    has_output_ = !output_.empty();
  }

  void reset() override {
    accumulated_.clear(); output_.clear(); has_output_ = false;
    step_view_->reset();
  }
  bool has_output() const override { return has_output_; }
  const DuckDBZSet &output() const { return output_; }

private:
  void admit(const DuckDBRow &row, int64_t w, DuckDBZSet &frontier) {
    if (union_all_) {
      accumulated_.insert(row, w); output_.insert(row, w); frontier.insert(row, w);
    } else if (accumulated_.get(row) == 0) {
      accumulated_.insert(row, 1); output_.insert(row, 1); frontier.insert(row, 1);
    }
  }
  InputFn anchor_;
  std::unique_ptr<NativeMaterializedView> step_view_;
  std::string sentinel_;
  bool union_all_;
  std::vector<std::pair<std::string, InputFn>> base_inputs_;
  size_t max_iterations_;
  DuckDBZSet accumulated_, output_;
  bool has_output_ = false;
};
```

5. `PlannedCircuitView::build` case:

```cpp
case PlanOpSpec::Kind::REC_CTE: {
  OutputFn anchor = build(*spec.children[0], keep_alive);
  std::string sentinel = "__rec_cte_" + std::to_string(spec.cte_index) + "__";
  TableSchema step_schema; step_schema.table_name = name_ + "_rec_step";
  auto step_view = std::make_unique<PlannedCircuitView>(
      name_ + "_rec_step", "", step_schema, keep_alive, *spec.children[1]);
  // Route base-table deltas into the inner view: ensure an outer SourceNode
  // exists for every non-sentinel inner source (CDC pushes into outer sources)
  std::vector<std::pair<std::string, OutputFn>> base_inputs;
  for (const auto &t : step_view->source_tables()) {
    if (t == sentinel) continue;
    PlanOpSpec src; src.kind = PlanOpSpec::Kind::SOURCE; src.table = t;
    base_inputs.emplace_back(t, build(src, keep_alive)); // reuses/creates outer source
  }
  bool union_all = spec.set_op == PlanOpSpec::SetOp::UNION_ALL;
  auto *node = circuit_.add_node(std::make_unique<PlanRecursiveNode>(
      circuit_.next_node_id(), std::move(anchor), std::move(step_view),
      sentinel, union_all, std::move(base_inputs)));
  return [node]() -> const DuckDBZSet & { return node->output(); };
}
```

Note: the inner `PlannedCircuitView`'s sentinel source must NOT leak into the outer view's `source_tables()` — it lives in the inner view only, and `build` above skips it. Also confirm `dbsp::Node` subclasses can call `apply_changes` on a `PlannedCircuitView` (public method — yes).

- [ ] **Step 4: Run tests.** New tests green; existing `test_recursive.cpp` integration tests still green (they now exercise the planner path — if any asserted parser-specific errors, fix the test intent, mirroring the B5 aspirational-test cleanup).
- [ ] **Step 5: Docs + commit.** Translator header comment scope line; `docs/THEORY.md` recursion section if it references parser fixed-point. Commit: `feat: Phase C2 - recursive CTEs through the planner frontend`.

---

### Task C3: DISTINCT ON through the planner

**Files:**
- Modify: `include/dbsp_plan_translator.hpp` (`visit_distinct`)
- Consumes: `include/dbsp_distinct_on.hpp` — verified ctor:
  `NativeDistinctOnView(name, sql, source_table, result_schema, std::vector<size_t> partition_keys, std::vector<SortColumn> sort_columns)`
- Test: `test/integration/test_planner_frontend.cpp`

**Interfaces:**
- Consumes: `EmbeddedViewNode`, the existing DISTINCT ON view from `dbsp_distinct_on.hpp`, SORT_LIMIT folding from C1.

**Design:** Plan shape is `PROJECTION → ORDER_BY → DISTINCT(targets) → child`. Extend `visit_distinct`: when `distinct_type == DISTINCT_ON`, require all `distinct_targets` to be `BoundReferenceExpression`, wrap the child in an `EmbeddedViewNode` around the existing DISTINCT ON view (same pattern as WINDOW), with the ORDER BY above deciding which row wins per key — read `dbsp_distinct_on.hpp` first and match how the parser's ViewFactory constructed it (it encodes key columns + sort columns). The ORDER_BY above the DISTINCT then folds per C1.

- [ ] **Step 1: Failing test:**

```cpp
TEST_CASE("planner: DISTINCT ON keeps first row per key", "[planner][distinct_on]") {
  PlannerFixture fx;
  fx.exec("CREATE TABLE dt(id INT, val INT, tag VARCHAR)");
  fx.exec("INSERT INTO dt VALUES (1, 10, 'a'), (2, 30, 'a'), (3, 20, 'b')");
  REQUIRE(fx.create_view("v_don",
      "SELECT DISTINCT ON (tag) id, val FROM dt ORDER BY tag, val DESC"));
  REQUIRE(fx.planner_built("v_don"));
  auto rows = fx.query_rows("v_don");
  REQUIRE(rows.size() == 2);            // one per tag
  CHECK(rows[0][0] == 2);               // 'a' → val 30 wins
  fx.exec("INSERT INTO dt VALUES (4, 40, 'a')"); fx.sync();
  rows = fx.query_rows("v_don");
  REQUIRE(rows.size() == 2);
  CHECK(rows[0][0] == 4);               // new winner for 'a'
}
```

- [ ] **Step 2: Run — FAIL** (planner rejects "DISTINCT ON", falls back; `planner_built` false).
- [ ] **Step 3: Implement.** Add spec kind `DISTINCT_ON` carrying `std::vector<size_t> key_idxs` (from `distinct_targets` bound-ref indices) and `std::vector<NativeSortView::SortColumn> tie_break` (from the `LogicalOrder` directly above, mapped to the DISTINCT's input layout — indices are shared because ORDER_BY sits immediately above DISTINCT in the canonical plan and both see the same child projection). `visit` special-case: when ORDER_BY's child is `LOGICAL_DISTINCT` with `DistinctType::DISTINCT_ON`, fold the order into the DISTINCT_ON spec (winner-per-key tie-break) instead of a SORT_LIMIT. Build case:

```cpp
case PlanOpSpec::Kind::DISTINCT_ON: {
  OutputFn child = build(*spec.children[0], keep_alive);
  TableSchema vschema; vschema.table_name = name_ + "_distinct_on";
  auto view = std::make_unique<NativeDistinctOnView>(
      name_ + "_distinct_on", "", EmbeddedViewNode::kInputName, vschema,
      spec.key_idxs, spec.tie_break);
  auto *node = circuit_.add_node(std::make_unique<EmbeddedViewNode>(
      circuit_.next_node_id(), std::move(child), std::move(view)));
  return [node]() -> const DuckDBZSet & { return node->output(); };
}
```

The trailing `PROJECTION(#refs)` above stays a MAP_COLS/MAP_EXPR as today (DISTINCT ON output keeps full child layout; the projection then selects the SELECT-list columns).
- [ ] **Step 4: Run tests** — suite green including `test_distinct_ddl.cpp`.
- [ ] **Step 5: Docs + commit:** `feat: Phase C3 - DISTINCT ON through the planner frontend`.

---

### Task C4: Circuit-IR optimizer (port DBSPOptimizer)

**Files:**
- Create: `include/dbsp_plan_optimizer.hpp`
- Modify: `include/dbsp_plan_translator.hpp` (call optimizer in `translate` after the walk; add `FILTER_MAP` kind + `PlanFilterMapNode`; add `PlanOpSpec::owned_exprs`)
- Test: `test/integration/test_planner_frontend.cpp`

**Interfaces:**
- Produces: `namespace dbsp_native { struct PlanOptStats { size_t filters_combined=0, filters_pushed_down=0, operators_fused=0; }; PlanOptStats optimize_plan(std::unique_ptr<PlanOpSpec> &root); }`
- Produces: `PlanOpSpec::Kind::FILTER_MAP` (fields: `exprs` = map exprs, new `std::vector<const duckdb::Expression*> filter_exprs`), `std::vector<std::unique_ptr<duckdb::Expression>> owned_exprs` (storage for rewritten pushdown predicates).
- Produces: `PlannedCircuitView::node_count()` (`circuit_` size accessor) for tests.

**Design:** Three passes on the `PlanOpSpec` tree, pre-node-construction — the IR home the old `ParsedViewDef` passes move to. `prune_projections` is deliberately dropped: DuckDB's binder already prunes via GET `column_ids`, so there is nothing left to prune on canonical plans (record this in the header comment).

1. **combine_filters:** `FILTER_EXPR(FILTER_EXPR(x))` → single `FILTER_EXPR` with concatenated `exprs` (filters preserve schema, so `input_types` are identical).
2. **pushdown_filters:** `FILTER_EXPR` directly above `JOIN`: for each predicate, collect its `BoundReferenceExpression` indices (walk with `duckdb::ExpressionIterator::EnumerateChildren`). All indices `< left_width` → move to a FILTER_EXPR on the left child as-is. All `>= left_width` → `Expression::Copy()`, subtract `left_width` from every bound-ref index in the copy, store the copy in `owned_exprs`, push a FILTER_EXPR on the right child. Mixed → stays put. (`left_width == spec.left_types.size()`.)
3. **fuse_filter_project:** `MAP_EXPR(FILTER_EXPR(x))` → `FILTER_MAP` (filter exprs evaluate against the same input layout as the map exprs — the filter's own input — so `input_types` carries over unchanged). Build case creates one `PlanFilterMapNode`:

```cpp
// Fused filter+project: evaluates predicates and, for surviving rows, the
// projection — one node, no intermediate Z-set between WHERE and SELECT.
class PlanFilterMapNode : public dbsp::Node {
public:
  using InputFn = std::function<const DuckDBZSet &()>;
  PlanFilterMapNode(dbsp::NodeId id, InputFn input,
                    std::shared_ptr<std::vector<std::unique_ptr<RowExprEval>>> filters,
                    std::shared_ptr<std::vector<std::unique_ptr<RowExprEval>>> projs)
      : dbsp::Node(id, "plan_filter_project"), input_(std::move(input)),
        filters_(std::move(filters)), projs_(std::move(projs)) {}
  void step() override {
    output_.clear();
    for (const auto &[row, w] : input_()) {
      bool pass = true;
      for (auto &f : *filters_) {
        duckdb::Value v = f->eval(row);
        if (v.IsNull() || !v.GetValue<bool>()) { pass = false; break; }
      }
      if (!pass) continue;
      DuckDBRow out; out.columns.reserve(projs_->size());
      for (auto &p : *projs_) out.columns.push_back(p->eval(row));
      output_.insert(out, w);
    }
    has_output_ = !output_.empty();
  }
  void reset() override { output_.clear(); has_output_ = false; }
  bool has_output() const override { return has_output_; }
  const DuckDBZSet &output() const { return output_; }
private:
  InputFn input_;
  std::shared_ptr<std::vector<std::unique_ptr<RowExprEval>>> filters_, projs_;
  DuckDBZSet output_;
  bool has_output_ = false;
};
```

(Check `dbsp::FilterNode`/`MapNode` step() in `dbsp_circuit.hpp` first — mirror their delta-handling exactly; if they do more than the loop above, e.g. weight normalization, copy that.)

- [ ] **Step 1: Failing tests:**

```cpp
TEST_CASE("planner IR optimizer: filter+project fuse into one node", "[planner][opt]") {
  PlannerFixture fx;
  fx.exec("CREATE TABLE ot(id INT, val INT)");
  fx.exec("INSERT INTO ot VALUES (1, 5), (2, 15)");
  REQUIRE(fx.create_view("v_opt", "SELECT id FROM ot WHERE val > 10"));
  // shape: SOURCE → [MAP_COLS?] → FILTER_MAP → sink. Without fusion there is
  // one extra node. Assert relative, not absolute, to stay robust:
  auto fused = fx.node_count("v_opt");
  fx.disable_ir_optimizer(); // testing hook: dbsp_ir_optimize(false) table fn OR
                             // a CDCManager flag — pick the simplest, document it
  REQUIRE(fx.create_view("v_raw", "SELECT id FROM ot WHERE val > 10"));
  CHECK(fused < fx.node_count("v_raw"));
  // Behavior identical:
  CHECK(fx.query_rows("v_opt") == fx.query_rows("v_raw"));
}

TEST_CASE("planner IR optimizer: join filter pushdown keeps results exact", "[planner][opt]") {
  PlannerFixture fx;
  fx.exec("CREATE TABLE l(id INT, x INT)"); fx.exec("CREATE TABLE r(id INT, y INT)");
  fx.exec("INSERT INTO l VALUES (1, 5), (2, 20)");
  fx.exec("INSERT INTO r VALUES (1, 7), (2, 2)");
  REQUIRE(fx.create_view("v_pd",
      "SELECT l.id FROM l JOIN r ON l.id = r.id WHERE l.x > 10 AND r.y < 5"));
  auto rows = fx.query_rows("v_pd");
  REQUIRE(rows.size() == 1); CHECK(rows[0][0] == 2);
  fx.exec("INSERT INTO l VALUES (3, 30)"); fx.exec("INSERT INTO r VALUES (3, 1)");
  fx.sync();
  CHECK(fx.query_rows("v_pd").size() == 2);
}
```

- [ ] **Step 2: Run — FAIL** (`node_count`/`disable_ir_optimizer` don't exist; counts equal).
- [ ] **Step 3: Implement** passes + node + hooks. `translate` calls `optimize_plan(root)` between walk and view construction, guarded by the flag.
- [ ] **Step 4: Run tests** — full suite green.
- [ ] **Step 5: Docs + commit:** update `docs/ARCHITECTURE.md` optimizer section (passes now on circuit IR; prune dropped as obsolete — DuckDB binder prunes). Commit: `feat: Phase C4 - optimizer passes ported to circuit IR`.

---

### Task C5: Delete the bespoke parser

**Files:**
- Delete: `include/dbsp_sql_parser.hpp`, `include/dbsp_optimizer.hpp`
- Modify: `include/dbsp_cdc.hpp` (create_view fallback branch + includes), `test/CMakeLists.txt` (or `test/build_test` cmake source lists)
- Delete/rewrite tests: `test/unit/test_sql_parser.cpp`, `test_parser_errors.cpp`, `test_order_limit_parser.cpp`, `test_set_ops_parser.cpp`, `test_optimizer.cpp` → delete (they test deleted code). `test/unit/test_advanced_projection.cpp`, `test_advanced_window.cpp`, `test_cte_subquery.cpp`, `test_window_functions.cpp` → inspect each: keep the assertions that exercise `Native*` views or end-to-end CDC behavior by rewriting construction to go through `CDCManager::create_view` (planner path); delete parser-construction plumbing. `test_optimization_integration.cpp` → rewrite against the C4 IR optimizer or fold into `test_planner_frontend.cpp`.
- Check: `include/dbsp_parser_extension.hpp` (DDL parser — stays; verify it doesn't include `dbsp_sql_parser.hpp`; if it does, extract the small shared piece rather than keeping the whole header).

**Interfaces:**
- After this task `create_view` is planner-only: translation failure sets `last_error_` to the translator's E110/binder message and returns false. No silent capability loss is acceptable — every removed capability must be a capability C1–C3 replaced or one the parser never actually had (e.g. the OR-filter silent-drop bug dies here by design).

- [ ] **Step 1: Inventory gate (must pass before deleting).** Run the full suite with the fallback disabled but code intact:

```cpp
// temporary probe in create_view: if (use_planner_ && !view) { last_error_ = translated_error; return false; }
```

Every test that fails only because of fallback removal identifies a real gap. Expected gaps already closed: ORDER BY/LIMIT (C1), recursion (C2), DISTINCT ON (C3). If anything else surfaces (candidates from B2 notes: DECIMAL SUM/AVG, DISTINCT/FILTER/ORDER-BY aggregates), STOP and report — decide close-vs-accept with the user before deletion. Do not proceed on a red gate.

- [ ] **Step 2: Remove fallback.** In `create_view`: keep the planner branch, on failure set `last_error_` (prefixed error already formatted by translator) and return false. Delete the `DBSPSqlParser`/`DBSPOptimizer`/CTE-intermediate-view/derived-table blocks (dbsp_cdc.hpp ~444–517) and the `ViewFactory::create_view` call (~558–565). The `#if 1 // SQL parser re-enabled` guard goes too. Keep the source-resolution/dedup loop — it is path-independent.
- [ ] **Step 3: Delete headers + fix build.** Remove the two headers, remove their `#include`s, run `make -j8`; chase compile errors (expected: `dbsp_cdc.hpp`, unit tests, possibly `dbsp_parser_extension.hpp`). `ParsedViewDef`-only types (e.g. `ViewType::FILTER_PROJECT` consumers in `dbsp_duckdb_types.hpp`) — `NativeFilterProjectView` and friends stay only if a remaining test or `EmbeddedViewNode` use survives; if nothing references one after the rewrite, delete it in the same commit (dead code).
- [ ] **Step 4: Rewrite/delete unit tests** per the file list above. Rewritten tests must keep encoding intent (why the behavior matters), not just construction mechanics.
- [ ] **Step 5: Run full suite ×3** (exit-crash history: `for i in 1 2 3; do ctest --output-on-failure || break; done`). All green, no exit segfaults.
- [ ] **Step 6: Docs + diagram + commit.** This is the big one per project CLAUDE.md: redraw `docs/ARCHITECTURE.md` diagram (parser + ViewFactory boxes removed; planner is the only frontend), update `README.md` feature list/version text, `docs/API.md` error surface (fallback gone → E110 errors now user-visible for unsupported SQL — list them), `docs/ERROR_HANDLING.md`, `docs/TESTING.md` (deleted/renamed test files), `CHANGELOG.md`. Commit: `feat: Phase C5 - delete bespoke SQL parser; planner is the only frontend`.

---

### Task C6: Memory + closeout

- [ ] Update auto-memory: MEMORY.md Phase C section (one line per milestone + gotchas found), new/updated memory file `phase-c-planner-completion.md`; mark `phase-b-plan.md` superseded where relevant.
- [ ] Verify checklist from project CLAUDE.md is fully done for each task (docs, comments, README, diagram).
- [ ] Final: `ctest` ×3 green, `git log --oneline` shows one commit per task, working tree clean.

## Risks

- **NativeLimitView delta correctness under churn** — its recompute-and-diff approach is O(state) per delta but exact; tests C1 cover insert AND delete re-admission.
- **Recursive fixed-point with deletions** — ignored (documented parity limitation). If a test asserts deletion propagation through recursion, that is a pre-existing semantic gap, not a C2 regression; surface it, don't hide it.
- **Hidden parser capabilities** — the C5 Step-1 gate exists precisely to catch these before deletion. Deletion is one commit, reviewable and revertable.
- **`test/build_test` + git**: never `git stash` with the build dir dirty.

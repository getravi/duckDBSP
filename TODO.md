# duckDBSP TODO List - DETAILED IMPLEMENTATION PLAN

This file tracks remaining features and improvements for the duckDBSP DuckDB extension.

**Plan Created**: 2026-02-06
**Overall Target Completion**: 2026-03-20 (6 weeks for Phases 1-3)

## 🚀 PROJECT STATUS SUMMARY

| Phase | Status | Completion |
|-------|--------|-----------|
| **Phase 1 (Foundation)** | ✅ COMPLETE | 2026-02-06 |
| Phase 2 (Advanced Features) | ⏳ Queued | 2026-02-24 |
| Phase 3 (Production Ready) | ⏳ Queued | 2026-03-20 |

**Phase 1 Summary**: All three core tasks completed ahead of schedule
- **P1.1** ✅ Parser extension for DDL syntax (CREATE/REFRESH MATERIALIZED VIEW)
- **P1.2** ✅ HAVING clause for aggregate filtering
- **P1.3** ✅ MIN/MAX aggregate functions with incremental maintenance

**Test Results**: 1000+ assertions passing across unit and integration tests
- SQL parser: 17 test cases (133 assertions)
- Extension basic: 8 test cases (132 assertions)
- CDC: 6 test cases (1066 assertions)
- Cascading views: 6 test cases (72 assertions)
- Others: 5 test cases (42 assertions)

---

## ✅ COMPLETED (February 2026)

### DuckDB 1.4.0 Migration - DONE ✅
- [x] **Upgrade to DuckDB v1.4.0**
- [x] **Fix ExtensionLoader API compatibility**
- [x] **Update query result APIs** (GetValue<T>, ColumnCount)
- [x] **Create custom hash functions** for DuckDB types
- [x] **Build system compatibility**
- [x] **Successful extension compilation**

**Status**: Extension builds successfully at `build/dbsp.duckdb_extension` (40MB, Mach-O 64-bit)
**Date Completed**: February 5, 2026

### Test Infrastructure Setup - DONE ✅
- [x] **Created test CMakeLists.txt** for building test suite
- [x] **Fixed Catch2 compatibility** (migrated from Catch2 v3 to Catch v2.13.7)
- [x] **Fixed include paths** and API compatibility issues
- [x] **Successfully built 8/10 test targets** (unit + integration tests)
- [x] **Executed test suite** with CTest framework

**Status**: Test infrastructure operational, all tests passing
**Date Completed**: February 5, 2026

### Security Hardening - Path Validation - DONE ✅
- [x] **Fixed null byte injection vulnerability** in `validate_filepath()`
- [x] **Cross-platform path validation** (Windows paths rejected on all platforms)
- [x] **All security validation tests passing** (41 assertions)

**Status**: Security validation complete, no known vulnerabilities
**Date Completed**: February 5, 2026

### DependencyGraph Bug Fixes - DONE ✅
- [x] **Fixed topological_order() returning empty vector**
- [x] **Fixed test expectations** for transitive dependents
- [x] **All CDC manager tests passing** (13 assertions in 5 test cases)

**Status**: DependencyGraph correctness verified
**Date Completed**: February 5, 2026

### Extension Loading Infrastructure - DONE ✅
- [x] **Downloaded and integrated DuckDB metadata system**
- [x] **Fixed extension entry point naming**
- [x] **Updated test infrastructure**
- [x] **Fixed runtime deadlock issue**

**Status**: Extension loads and executes successfully
**Date Completed**: February 5, 2026

---

## ✅ PHASE 1: FOUNDATION - COMPLETE (Feb 6, 2026)

### P1.1: Wire Parser Extension for DDL Syntax ✅ COMPLETE
**Priority**: 🔴 CRITICAL (Quick win, high UX impact)
**Status**: COMPLETE
**Effort**: 2-3 days
**Completion Date**: Feb 6, 2026

**Description**: Complete parser extension integration to enable native SQL DDL syntax
Current: Users must use table functions `SELECT * FROM dbsp_create_view(...)`
Target: Users can use SQL syntax `CREATE MATERIALIZED VIEW ... AS SELECT ...`

**Files to Modify**:
- `dbsp_extension.cpp` - Register parser extension in `LoadInternal()`
- `dbsp_parser_extension.hpp` - Verify implementation completeness
- `test/integration/test_extension_basic.cpp` - Add DDL syntax tests

**Implementation Steps**:
1. **Step 1.1.1**: Audit `dbsp_parser_extension.hpp` to verify all functions are implemented
   - Verify `MaterializedViewPlan()` constructs proper plan node
   - Verify `CreateMaterializedViewBind()` handles view creation
   - Verify `RefreshMaterializedViewBind()` works correctly
   - Verify `DropMaterializedViewBind()` handles cascading deletes

2. **Step 1.1.2**: Register parser extension in `LoadInternal()` (dbsp_extension.cpp)
   - Find where `catalog.AddFunction()` calls are made
   - Add: `connection.RegisterParserExtension(MaterializedViewPlanFunction, ...)`
   - Ensure hook is registered before table functions are loaded

3. **Step 1.1.3**: Test DDL syntax
   - Create integration test: `CREATE MATERIALIZED VIEW test_view AS SELECT ...`
   - Verify view appears in `dbsp_views()` output
   - Verify `DROP MATERIALIZED VIEW test_view` works
   - Verify `DROP MATERIALIZED VIEW CASCADE` cascades correctly
   - Verify `REFRESH MATERIALIZED VIEW test_view` is a no-op (auto-refresh)

4. **Step 1.1.4**: Update documentation
   - Add example showing both table function and DDL syntax
   - Document that DDL is now preferred method

**Success Criteria**:
- ✅ `CREATE MATERIALIZED VIEW name AS SELECT * FROM table` creates view
- ✅ View appears in `INFORMATION_SCHEMA.VIEWS` (if supported by DuckDB)
- ✅ View appears in `dbsp_views()` output
- ✅ `DROP MATERIALIZED VIEW name` removes view
- ✅ `DROP MATERIALIZED VIEW name CASCADE` removes dependents
- ✅ `REFRESH MATERIALIZED VIEW name` executes without error
- ✅ All new tests pass

**Testing**:
- Add 5+ test cases to `test_extension_basic.cpp`
- Test simple views, filtered views, aggregate views
- Test cascade deletion
- Test refresh

**Dependencies**: None - independent task
**Blockers**: None identified
**Risk Level**: LOW - Infrastructure exists, just needs integration

---

### P1.2: Implement HAVING Clause for Aggregate Filtering ✅ COMPLETE
**Priority**: 🔴 HIGH (Most requested SQL feature)
**Status**: COMPLETE
**Effort**: 3-4 days
**Completion Date**: Feb 6, 2026
**Depends On**: P1.1 (recommended but not required)

**Description**: Add HAVING clause support for filtering aggregated results
Current: Error E101 thrown
Target: `SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) > 10`

**Example Queries to Support**:
```sql
SELECT dept, COUNT(*) as cnt FROM employees
GROUP BY dept
HAVING COUNT(*) > 10;

SELECT category, SUM(price) as total FROM products
GROUP BY category
HAVING SUM(price) > 1000;

SELECT customer_id, AVG(order_amount) as avg_amt FROM orders
GROUP BY customer_id
HAVING AVG(order_amount) >= 100 AND COUNT(*) > 5;
```

**Files to Modify**:
- `dbsp_sql_parser.hpp` - Parse HAVING clause from SELECT
- `dbsp_materialized_view.hpp` - Add filter to NativeAggregateView
- `test/unit/test_sql_parser.cpp` - Add parser tests
- `test/integration/test_cascading_views.cpp` - Add functional tests

**Implementation Steps**:

1. **Step 1.2.1**: Extend ParsedViewDef struct (dbsp_sql_parser.hpp)
   - Add field: `std::optional<Expression> having_filter`
   - Add field: `std::vector<AggregateExpr> having_aggregates` (aggs used in HAVING)

2. **Step 1.2.2**: Update SQL parser to recognize HAVING (dbsp_sql_parser.hpp)
   - In `parse_select_statement()`, after parsing GROUP BY, check for HAVING keyword
   - Parse HAVING expression (can reference group columns and aggregates)
   - Store in `ParsedViewDef.having_filter`
   - Example: `HAVING COUNT(*) > 10` → Store as BinaryExpression(COUNT(*), >, 10)

3. **Step 1.2.3**: Implement HAVING in NativeAggregateView (dbsp_materialized_view.hpp)
   - Current: `apply_changes()` builds aggregate map, returns as-is
   - New: After building aggregate map, filter by HAVING condition
   - For each (group_key, agg_values) pair:
     - Evaluate HAVING expression with group columns + aggregate values
     - Include pair only if HAVING evaluates to true
   - Deletion: Remove pairs that no longer satisfy HAVING condition

4. **Step 1.2.4**: Update ViewFactory to handle HAVING
   - When creating NativeAggregateView, pass having_filter
   - ViewFactory must detect HAVING is used and reject if aggregates don't support it

5. **Step 1.2.5**: Test HAVING parsing and execution
   - Unit test: Parse SELECT...HAVING into ParsedViewDef
   - Unit test: Evaluate HAVING conditions with mock data
   - Integration test: Create view with HAVING, verify results
   - Integration test: Update base table, verify HAVING re-filters correctly

**Success Criteria**:
- ✅ Parser recognizes HAVING keyword without error
- ✅ Can parse simple HAVING conditions (e.g., `HAVING COUNT(*) > 10`)
- ✅ Can parse complex HAVING conditions (e.g., `HAVING SUM(x) > 100 AND COUNT(*) > 5`)
- ✅ HAVING reference to group columns works (e.g., `HAVING dept = 'Sales'`)
- ✅ Aggregate values correctly filtered by HAVING condition
- ✅ Incremental updates re-evaluate HAVING (rows added/removed as needed)
- ✅ All new tests pass

**Testing**:
- Unit: 5+ test cases for HAVING parsing
- Integration: 3+ test cases showing HAVING filtering behavior
- Edge case: Empty result set when no groups satisfy HAVING
- Edge case: All groups satisfy HAVING (HAVING always true)

**Dependencies**: P1.1 (recommended but can be done independently)
**Blockers**: None - straightforward feature
**Risk Level**: LOW - Clear requirements, bounded scope

---

### P1.3: Add MIN and MAX Aggregate Functions ✅ COMPLETE
**Priority**: 🔴 HIGH (Completes basic aggregates)
**Status**: COMPLETE
**Effort**: 2-3 days
**Completion Date**: Feb 6, 2026
**Depends On**: P1.2 (HAVING helps verify aggregates work)

**Description**: Implement MIN and MAX aggregate functions alongside SUM/COUNT/AVG
Current: Error E2xx thrown
Target: `SELECT dept, MIN(salary), MAX(salary) FROM emp GROUP BY dept`

**Example Queries to Support**:
```sql
SELECT MIN(price), MAX(price) FROM products;

SELECT category, MIN(date), MAX(date) FROM events GROUP BY category;

SELECT customer_id, MIN(order_amount), MAX(order_amount)
FROM orders
GROUP BY customer_id;
```

**Files to Modify**:
- `dbsp_sql_parser.hpp` - Recognize MIN/MAX aggregate functions
- `dbsp_materialized_view.hpp` - Implement MIN/MAX aggregation logic
- `test/unit/test_sql_parser.cpp` - Add MIN/MAX parser tests
- `test/integration/test_cascading_views.cpp` - Add functional tests

**Implementation Steps**:

1. **Step 1.3.1**: Extend AggregateFunction enum (dbsp_materialized_view.hpp)
   - Add: `MIN`, `MAX` to enum
   - Update switch statements to handle these cases

2. **Step 1.3.2**: Update parser to recognize MIN/MAX (dbsp_sql_parser.hpp)
   - In `parse_aggregate_function()`, add case for "MIN" and "MAX"
   - Store as AggregateFunction::MIN or AggregateFunction::MAX

3. **Step 1.3.3**: Implement MIN/MAX computation (dbsp_materialized_view.hpp)
   - In `NativeAggregateView::AggregateAccumulator`:
     - For MIN: Track minimum value across all additions
     - For MAX: Track maximum value across all additions
     - Handle NULL values (NULL not included in MIN/MAX, except all-NULL → NULL)

4. **Step 1.3.4**: Handle deletions correctly
   - Challenge: When deleting, how do we know if the min/max changed?
   - Solution A (Simple): Rebuild aggregate from remaining elements
   - Solution B (Efficient): Use auxiliary data structure (sorted set)
   - Recommend Solution A for now (simpler, sufficient for v0.1)
   - For MIN deletions: Rebuild if deleted element was the minimum

5. **Step 1.3.5**: Test MIN/MAX aggregation
   - Unit test: Parse MIN(col) and MAX(col)
   - Unit test: Compute MIN/MAX incrementally with additions
   - Integration test: GROUP BY with MIN/MAX
   - Edge case: All NULL values → NULL result
   - Edge case: Mixed NULLs and values
   - Edge case: Duplicate min/max values

**Success Criteria**:
- ✅ Parser recognizes MIN(col) and MAX(col)
- ✅ MIN/MAX computed correctly for initial data
- ✅ Incremental updates maintain correct MIN/MAX
- ✅ NULL values handled correctly (not included unless all-NULL)
- ✅ Works with GROUP BY
- ✅ Works with HAVING (e.g., `HAVING MIN(x) > 10`)
- ✅ All new tests pass

**Testing**:
- Unit: 4+ test cases for MIN/MAX parsing and computation
- Integration: 3+ test cases with GROUP BY
- Edge case: NULL handling
- Edge case: Single value → MIN = MAX
- Edge case: Duplicate values → result is still correct

**Dependencies**: P1.2 (optional - HAVING helps test MIN/MAX)
**Blockers**: None - straightforward feature
**Risk Level**: LOW - Similar implementation to SUM

---

## 🔴 PHASE 2: CORE DBSP COMPLETENESS (Weeks 3-5, Target: Feb 24 - Mar 10)

> **⚠️ REVISED FOCUS**: Phase 2 now prioritizes **core DBSP theoretical features** over SQL syntax sugar, based on alignment with DBSP research papers and correctness requirements.

### P2.1: DISTINCT SQL Integration ⭐ QUICK WIN
**Priority**: 🔴 CRITICAL (Core DBSP feature, already implemented)
**Status**: NOT STARTED
**Effort**: 1-2 days
**Target Completion**: Feb 21-22
**Depends On**: None (independent)

**Description**: Expose existing DISTINCT operator to SQL parser
Current: `IncrementalDistinct` exists in C++ but `SELECT DISTINCT` throws error
Target: `SELECT DISTINCT column FROM table` works correctly

**Background**: The incremental distinct operator (Δ-distinct) is a fundamental DBSP primitive covered in the relational algebra formalization. It's already correctly implemented in `dbsp_stream.hpp` but not accessible via SQL.

**Example Queries to Support**:
```sql
SELECT DISTINCT customer_id FROM orders;

SELECT DISTINCT category, brand FROM products;

-- Combined with other operations
SELECT DISTINCT dept FROM employees WHERE salary > 50000;
```

**Files to Modify**:
- `dbsp_sql_parser.hpp` - Detect SELECT DISTINCT syntax
- `dbsp_cdc.hpp` - Wire DistinctView to ViewFactory
- `test/unit/test_sql_parser.cpp` - Add DISTINCT parsing tests
- `test/integration/test_extension_basic.cpp` - Add functional tests

**Implementation Steps**:

1. **Step 2.1.1**: Update SQL parser to detect DISTINCT
   ```cpp
   // In parse_select_list()
   if (select_node->modifier == SelectModifier::DISTINCT) {
       def.type = ViewType::DISTINCT;
   }
   ```

2. **Step 2.1.2**: Update ViewFactory to create DistinctView
   - When `def.type == ViewType::DISTINCT`, instantiate `DistinctView`
   - Wire to source table

3. **Step 2.1.3**: Test DISTINCT functionality
   - Unit test: Parse `SELECT DISTINCT`
   - Integration test: Verify duplicates removed
   - Integration test: Incremental updates maintain distinctness
   - Edge case: DISTINCT with NULL values

**Success Criteria**:
- ✅ Parser recognizes `SELECT DISTINCT` without error
- ✅ Duplicate rows removed from results
- ✅ Incremental updates correctly maintain uniqueness
- ✅ NULL values handled correctly (NULLs are distinct from each other in SQL)
- ✅ Works with WHERE, GROUP BY, JOIN
- ✅ All new tests pass

**Testing**:
- Unit: 3+ tests for DISTINCT parsing
- Integration: 4+ tests for distinct behavior with updates
- Edge case: All duplicate values
- Edge case: No duplicates (DISTINCT is no-op)
- Edge case: Mix of duplicates and unique values

**Dependencies**: None
**Blockers**: None - implementation already exists
**Risk Level**: LOW - Simple SQL wiring

---

### P2.2: Fix MIN/MAX Incremental Deletions ⭐ CORRECTNESS FIX
**Priority**: 🔴 CRITICAL (Correctness issue for O(δ) claims)
**Status**: NOT STARTED
**Effort**: 3-4 days
**Target Completion**: Feb 24-27
**Depends On**: P1.3 (MIN/MAX implementation)

**Description**: Replace O(n) MIN/MAX deletion handling with O(log n) solution
Current: Rebuilds aggregate when minimum/maximum is deleted (degrades to O(n))
Target: Maintain ordered multiset per group for true O(log n) incremental updates

**Problem**: Current TODO.md acknowledges at line 272-273:
> "Challenge: When deleting, how do we know if the min/max changed?"
> "Solution A (Simple): Rebuild aggregate from remaining elements"

This violates DBSP's O(δ) guarantee and makes MIN/MAX non-incremental.

**Example Issue**:
```sql
-- Group has 1M rows, min value appears once
SELECT dept, MIN(salary) FROM employees GROUP BY dept;

-- Delete the min salary row → O(n) rebuild!
DELETE FROM employees WHERE salary = 30000;
```

**Files to Modify**:
- `dbsp_materialized_view.hpp` - Update AggregateView::AggState
- `include/dbsp_materialized_view.hpp` - Add ordered multiset tracking
- `test/integration/test_cascading_views.cpp` - Add MIN/MAX deletion tests
- `test/benchmarks/bench_aggregates.cpp` - Verify O(log n) performance

**Implementation Steps**:

1. **Step 2.2.1**: Extend AggState to track all values
   ```cpp
   struct AggState {
       int64_t sum = 0;
       int64_t count = 0;
       std::multiset<int64_t> values;  // NEW: Ordered values for MIN/MAX
   };
   ```

2. **Step 2.2.2**: Update MIN computation
   ```cpp
   // Insert: O(log n)
   state.values.insert(value);
   
   // Delete: O(log n)
   auto it = state.values.find(value);
   if (it != state.values.end()) {
       state.values.erase(it);
   }
   
   // MIN: O(1)
   int64_t min = state.values.empty() ? 0 : *state.values.begin();
   ```

3. **Step 2.2.3**: Update MAX computation
   ```cpp
   // MAX: O(1)
   int64_t max = state.values.empty() ? 0 : *state.values.rbegin();
   ```

4. **Step 2.2.4**: Memory optimization (optional)
   - Only maintain `values` multiset for MIN/MAX aggregates
   - For SUM/COUNT/AVG, keep current approach (don't waste memory)

5. **Step 2.2.5**: Test and benchmark
   - Unit test: MIN/MAX with deletions
   - Integration test: Large group (10k rows), delete min/max values
   - Benchmark: Verify O(log n) vs old O(n) behavior

**Success Criteria**:
- ✅ MIN correctly updated when minimum value deleted (O(log n))
- ✅ MAX correctly updated when maximum value deleted (O(log n))
- ✅ Multiple occurrences of min/max value handled correctly
- ✅ Empty groups return NULL (standard SQL behavior)
- ✅ Benchmark shows O(log n) deletion time, not O(n)
- ✅ All existing MIN/MAX tests still pass

**Testing**:
- Unit: 5+ tests for MIN/MAX deletion scenarios
- Integration: 3+ tests with large groups
- Performance: Benchmark deletion of min/max in 10k, 100k, 1M groups
- Edge case: Delete all values (group becomes empty)
- Edge case: Multiple identical min/max values

**Dependencies**: P1.3
**Blockers**: None
**Risk Level**: LOW - Well-understood data structure (std::multiset)

---

### P2.3: Support Complex JOIN Predicates
**Priority**: 🟡 MEDIUM (Extends correct bilinear join logic)
**Status**: NOT STARTED
**Effort**: 2-3 days
**Target Completion**: Feb 28 - Mar 3
**Depends On**: P1.2 (HAVING)

**Description**: Extend JOIN support from equality-only to complex predicates
Current: Only `ON t1.id = t2.id` works
Target: `ON t1.id = t2.id AND t1.type = t2.type` and `ON t1.val > t2.val`

**Example Queries to Support**:
```sql
-- Multi-column equality join
SELECT * FROM orders o
JOIN customers c
  ON o.customer_id = c.id AND o.region = c.region;

-- Non-equi join (less common, can support later)
SELECT * FROM salaries s
JOIN grades g
  ON s.salary >= g.min_salary AND s.salary < g.max_salary;
```

**Files to Modify**:
- `dbsp_sql_parser.hpp` - Parse complex JOIN ON predicates
- `dbsp_materialized_view.hpp` - Update JoinView to handle complex predicates
- `test/unit/test_sql_parser.cpp` - Parser tests
- `test/integration/test_cascading_views.cpp` - Functional tests

**Implementation Steps**:

1. **Step 2.3.1**: Extend JoinPredicate struct
   - Current: Single equality (left_col, right_col)
   - New: List of predicates (AND-ed together)
   - Support: Equality and comparison operators (>, <, >=, <=, !=)

2. **Step 2.3.2**: Update parser to handle compound ON clauses
   - Current: Parses single equality
   - New: Parse `ON cond1 AND cond2 AND ... AND condN`
   - Each condition can be `=`, `>`, `<`, `>=`, `<=`, `!=`
   - Store list of predicates in ParsedViewDef

3. **Step 2.3.3**: Update NativeJoinView to evaluate complex predicates
   - Current: Equality join uses hash table
   - New: For each potential pair, evaluate all predicates
   - Optimization: Still use equality predicates for hash table keys
   - Then filter by non-equi predicates

4. **Step 2.3.4**: Test complex JOIN predicates
   - Unit test: Parse complex ON clauses
   - Integration test: Multi-column equality join
   - Integration test: Non-equi predicates (if supported)

**Success Criteria**:
- ✅ Parser recognizes AND-ed predicates in ON clause
- ✅ Multi-column equality joins work correctly
- ✅ Non-equi predicates filter results correctly
- ✅ NULL handling in predicates correct (NULL != NULL)
- ✅ Incremental updates through complex joins work
- ✅ All new tests pass

**Testing**:
- Unit: 3+ tests for complex predicate parsing
- Integration: 3+ tests for multi-column joins
- Edge case: NULL values in join keys
- Edge case: All predicates true/false

**Dependencies**: P1.2
**Blockers**: None identified
**Risk Level**: LOW - Incremental extension of existing join code

---

### P2.4: Recursive Query Support ⭐ PHASE 2 MILESTONE
**Priority**: 🟡 MEDIUM-HIGH (Core DBSP differentiator)
**Status**: NOT STARTED
**Effort**: 1-2 weeks
**Target Completion**: Mar 4-10
**Depends On**: P2.1, P2.2, P2.3

**Description**: Implement WITH RECURSIVE for transitive closures and recursive queries
Current: No recursive query support
Target: `WITH RECURSIVE ... SELECT ...` with incremental maintenance

**Background**: Recursive queries are a **core DBSP capability** covered extensively in the formal specification (`recursive.lean`) and VLDB paper Section 7. This is what differentiates DBSP from traditional incremental view maintenance.

**Example Queries to Support**:
```sql
-- Transitive closure: Find all reachable nodes in a graph
WITH RECURSIVE reachable AS (
    SELECT src, dst FROM edges
    UNION
    SELECT e.src, r.dst 
    FROM edges e 
    JOIN reachable r ON e.dst = r.src
)
SELECT * FROM reachable;

-- Employee hierarchy
WITH RECURSIVE employee_tree AS (
    SELECT id, name, manager_id, 0 as level FROM employees WHERE manager_id IS NULL
    UNION ALL
    SELECT e.id, e.name, e.manager_id, et.level + 1
    FROM employees e
    JOIN employee_tree et ON e.manager_id = et.id
)
SELECT * FROM employee_tree;

-- Bill of materials (BOM) explosion
WITH RECURSIVE bom AS (
    SELECT part_id, component_id, quantity FROM parts WHERE part_id = 'WIDGET'
    UNION ALL
    SELECT b.part_id, p.component_id, b.quantity * p.quantity
    FROM bom b
    JOIN parts p ON b.component_id = p.part_id
)
SELECT * FROM bom;
```

**Files to Modify**:
- `dbsp_sql_parser.hpp` - Parse WITH RECURSIVE syntax
- `include/dbsp_stream.hpp` - Add stream introduction/elimination (δ₀, ∫)
- `include/dbsp_circuit.hpp` - Add recursive operator support
- New: `dbsp_recursive.hpp` - Recursive query implementation
- `test/unit/test_sql_parser.cpp` - Parser tests
- `test/integration/test_recursive.cpp` - Functional tests (NEW FILE)

**Implementation Steps**:

1. **Step 2.4.1**: Study DBSP recursive theory
   - Read formal spec: `recursive.lean` from Lean formalization
   - Understand stream introduction δ₀: Creates nested time domain
   - Understand stream elimination ∫: Computes fixed point
   - Review VLDB paper Section 7: "Recursive Queries"

2. **Step 2.4.2**: Extend SQL parser for WITH RECURSIVE
   ```cpp
   // Detect WITH RECURSIVE clause
   if (statement has WITH clause && is RECURSIVE) {
       parse recursive CTE definition
       identify base case (anchor)
       identify recursive case
   }
   ```

3. **Step 2.4.3**: Implement stream introduction δ₀
   ```cpp
   // dbsp_stream.hpp
   template <typename T>
   class StreamIntroduction {
       // Converts a Z-set into a unit pulse at time 0
       Stream<ZSet<T>> introduce(const ZSet<T>& initial);
   };
   ```

4. **Step 2.4.4**: Implement stream elimination ∫
   ```cpp
   // dbsp_stream.hpp
   template <typename T>
   class StreamElimination {
       // Iterates until fixed point is reached
       ZSet<T> eliminate(Stream<ZSet<T>> recursive_stream);
   };
   ```

5. **Step 2.4.5**: Create RecursiveView class
   ```cpp
   // dbsp_recursive.hpp
   class RecursiveView : public MaterializedView {
       void apply_changes(const RowZSet& changes) override {
           // Apply δ₀ (introduce into nested time)
           // Run recursive iteration until convergence
           // Apply ∫ (eliminate back to original time)
       }
   };
   ```

6. **Step 2.4.6**: Implement fixed-point detection
   - Detect when recursive iteration produces no new results
   - Safety: Limit max iterations (e.g., 1000) to prevent infinite loops
   - Optimization: Use delta-based convergence check

7. **Step 2.4.7**: Test recursive queries
   - Unit test: Parse WITH RECURSIVE
   - Integration test: Transitive closure on small graph (5 nodes)
   - Integration test: Incremental update to edges triggers re-computation
   - Integration test: Employee hierarchy with new manager
   - Stress test: Large graph (1000 nodes), ensure convergence

**Success Criteria**:
- ✅ Parser recognizes WITH RECURSIVE syntax
- ✅ Base case (anchor) and recursive case separated correctly
- ✅ Transitive closure computes correctly
- ✅ Fixed-point convergence detection works
- ✅ Incremental updates trigger minimal re-computation
- ✅ Safety: Max iteration limit prevents infinite loops
- ✅ All new tests pass

**Testing**:
- Unit: 4+ tests for WITH RECURSIVE parsing
- Integration: 5+ tests for recursive query evaluation
- Edge case: Empty base case
- Edge case: No fixed point (error handling)
- Edge case: Single iteration (base case is complete result)
- Performance: Benchmark convergence time for large graphs

**Dependencies**: P2.1, P2.2, P2.3
**Blockers**: Requires deep understanding of DBSP recursion theory
**Risk Level**: MEDIUM-HIGH - Complex feature, but well-documented in theory

**Note**: This is an **advanced DBSP feature** that differentiates this extension from simpler incremental view systems. Implementing this correctly demonstrates mastery of DBSP theory.

---

## 🟡 PHASE 3: PRODUCTION READINESS (Weeks 6-7, Target: Mar 11-24)

### P3.1: Enhanced Error Messages and Diagnostics
**Priority**: 🟡 MEDIUM (Important for adoption)
**Status**: NOT STARTED
**Effort**: 2-3 days
**Target Completion**: Mar 11-13
**Depends On**: P2.4 (After core features implemented)

**Description**: Improve error messages for unsupported features and edge cases

**Current State**:
- Error codes exist (DBSP-E101 through DBSP-E504)
- Some messages are generic or reference TODO items
- Documentation links exist but could be more helpful

**Target**:
- Clear, actionable error messages with concrete workarounds
- Helpful suggestions for unsupported features
- Better diagnostics for common mistakes

**Files to Modify**:
- `dbsp_errors.hpp` - Improve error message strings
- `docs/errors/*.md` - Enhance documentation with examples
- `dbsp_sql_parser.hpp` - Add context-specific error messages
- `dbsp_cdc.hpp` - Add validation error messages

**Implementation Steps**:

1. **Step 3.1.1**: Audit current error messages
   - For each DBSP-Exxx code, document current message
   - Identify vague or unhelpful messages
   - Map errors to specific code locations

2. **Step 3.1.2**: Rewrite error messages
   - Format: `[Error Code]: Problem | Workaround | Example`
   - Example for E101 (HAVING not supported):
     ```
     DBSP-E101: HAVING clause not yet supported
     Workaround: Use nested views with WHERE clause
     Example:
       CREATE MATERIALIZED VIEW high_count AS
       SELECT dept, COUNT(*) as cnt FROM emp GROUP BY dept;

       CREATE MATERIALIZED VIEW filtered AS
       SELECT * FROM high_count WHERE cnt > 10;
     ```
   - Apply format to all 50+ error codes

3. **Step 3.1.3**: Add context to error messages
   - Include line number or query snippet
   - Include table/view name for relevant errors
   - Include suggestions based on SQL pattern

4. **Step 3.1.4**: Update error documentation
   - For each error code, create/update `docs/errors/DBSP-Exxx.md`
   - Include: Description, Workaround, Examples, Related Errors
   - Make searchable from error message (link or grep-friendly)

5. **Step 3.1.5**: Test error messages
   - Unit test: Each error code produces expected message
   - Unit test: Message includes workaround suggestion
   - Manual test: Try unsupported features, verify helpful error

**Success Criteria**:
- ✅ All error messages include workaround or suggestion
- ✅ Error messages are <= 200 characters (concise)
- ✅ Context information included (table/view names, line numbers)
- ✅ Documentation pages exist for common errors
- ✅ Users can easily find solutions using error codes

**Testing**:
- Unit: 5+ tests for error message generation
- Manual: Trigger each common error, verify message quality

**Dependencies**: P1.2
**Blockers**: None
**Risk Level**: LOW - Documentation and messaging

---

### P3.2: Reader-Writer Locks for Concurrency
**Priority**: 🟡 MEDIUM (Performance improvement)
**Status**: NOT STARTED
**Effort**: 3-5 days
**Target Completion**: Mar 14-18
**Depends On**: P2.1 (After core features stable)

**Description**: Replace global mutex with reader-writer locks for concurrent queries
Current: Single `std::mutex` serializes all operations
Target: Multiple concurrent readers, exclusive writers

**Files to Modify**:
- `dbsp_cdc.hpp` - Replace `std::mutex` with `std::shared_mutex`
- `dbsp_cdc.hpp` - Update lock guards to use shared locks for reads
- `test/integration/test_extension_cdc.cpp` - Add concurrency tests
- New: `bench/bench_concurrency.cpp` - Concurrency benchmarks

**Implementation Steps**:

1. **Step 3.2.1**: Identify read vs write code paths
   - Read-only: `query()`, `views()`, `tables()`, `deps()`
   - Write: `track()`, `sync()`, `create_view()`, `drop()`, `drop_cascade()`
   - Mixed: Some operations do read then write

2. **Step 3.2.2**: Replace mutex with shared_mutex
   - In CDCManager class:
     ```cpp
     // Before: std::mutex state_lock;
     // After:  std::shared_mutex state_lock;
     ```

3. **Step 3.2.3**: Update lock usage
   - Read operations: `std::shared_lock<std::shared_mutex> lock(state_lock);`
   - Write operations: `std::unique_lock<std::shared_mutex> lock(state_lock);`
   - Review each operation, choose appropriate lock type

4. **Step 3.2.4**: Test concurrent access
   - Unit test: Multiple threads reading views simultaneously
   - Integration test: Concurrent reads + occasional writes
   - Stress test: High concurrency with short-lived operations

5. **Step 3.2.5**: Benchmark improvements
   - Measure: 100 concurrent readers, single writer
   - Measure: 10 concurrent readers, 1 writer, alternating
   - Compare: Before (mutex) vs after (shared_mutex)
   - Document: Performance improvement %

**Success Criteria**:
- ✅ No data races detected by ThreadSanitizer
- ✅ Concurrent queries execute in parallel
- ✅ Write operations still exclusive
- ✅ Performance improvement measurable (20%+ in read-heavy workloads)
- ✅ All existing tests still pass

**Testing**:
- Integration: 3+ tests for concurrent access patterns
- Stress test: Verify no deadlocks under high concurrency
- Sanitizer: Run with ThreadSanitizer enabled
- Benchmark: Before/after performance comparison

**Dependencies**: P2.1 (Optional - after core features stable)
**Blockers**: Must carefully review locking strategy
**Risk Level**: MEDIUM - Concurrency bugs can be subtle

---

### P3.3: Set Up GitHub Actions CI/CD Pipeline
**Priority**: 🟡 MEDIUM (Infrastructure, enables faster iteration)
**Status**: NOT STARTED
**Effort**: 2-3 days
**Target Completion**: Mar 19-21
**Depends On**: None - can be done anytime

**Description**: Create automated testing on commit/PR via GitHub Actions
Current: Tests must be run manually
Target: Automated builds and tests on every commit/PR

**Files to Create**:
- `.github/workflows/build-test.yml` - Build and unit test on every commit
- `.github/workflows/integration-test.yml` - Integration tests on every PR
- `.github/workflows/benchmark.yml` - Performance benchmarks on release
- `docs/DEVELOPMENT.md` - Developer setup and testing guide

**Implementation Steps**:

1. **Step 3.3.1**: Design workflow structure
   - On commit to main: Build + unit tests (fast)
   - On PR: Build + unit + integration tests (comprehensive)
   - On release tag: Build + all tests + benchmarks
   - On daily schedule: Nightly full suite + sanitizers

2. **Step 3.3.2**: Create build workflow (`.github/workflows/build-test.yml`)
   - Trigger: Push to main, pull requests
   - Steps:
     - Checkout code
     - Install dependencies (DuckDB, CMake, Catch2)
     - Build extension: `mkdir build && cd build && cmake .. && make`
     - Run unit tests: `ctest -R unit`
     - Report results
   - Platforms: macOS, Linux (Ubuntu)

3. **Step 3.3.3**: Create integration test workflow
   - Trigger: Pull requests
   - Steps:
     - Same build steps
     - Run integration tests: `ctest -R integration`
     - Archive test logs
     - Report coverage

4. **Step 3.3.4**: Create nightly sanitizer workflow
   - Trigger: Daily schedule (midnight UTC)
   - Build with: `-DENABLE_ASAN=ON -DENABLE_UBSAN=ON -DENABLE_TSAN=ON`
   - Report any failures to team

5. **Step 3.3.5**: Add status badges to README
   - Build status badge
   - Test pass/fail badge
   - Coverage badge (optional)

6. **Step 3.3.6**: Create developer documentation
   - How to run tests locally
   - How to run with sanitizers
   - How to run benchmarks
   - Troubleshooting guide

**Success Criteria**:
- ✅ Commits to main trigger build + unit test
- ✅ PRs trigger comprehensive test suite
- ✅ All tests pass on all platforms
- ✅ Status badges visible on README
- ✅ Developers can see CI results directly in GitHub

**Testing**:
- Manual: Trigger workflow, verify it runs correctly
- Verify: All tests pass on CI
- Verify: Workflow can detect failing tests

**Dependencies**: None - independent
**Blockers**: None
**Risk Level**: LOW - Standard GitHub Actions workflow

---

### P3.4: Circuit Optimization Pass
**Priority**: 🟡 MEDIUM (Performance improvement)
**Status**: NOT STARTED
**Effort**: 5-7 days
**Target Completion**: Mar 22-24
**Depends On**: P2.4 (After core features complete)

**Description**: Implement circuit optimization passes for operator fusion and pushdown
Current: Circuit abstraction exists but no optimization
Target: Automatic query optimization following DBSP theory

**Background**: DBSP formal specification (`circuits.lean`) proves correctness of a general algorithm for incrementalizing and optimizing circuits. This enables automatic query optimization.

**Optimization Strategies**:
1. **Filter Pushdown**: Move filters closer to data sources
2. **Projection Pushdown**: Eliminate unused columns early
3. **Operator Fusion**: Combine multiple map/filter operations
4. **Join Reordering**: Optimize join order for smallest intermediates

**Files to Modify**:
- New: `include/dbsp_optimizer.hpp` - Circuit optimization passes
- `dbsp_cdc.hpp` - Apply optimization before circuit execution
- `test/benchmarks/bench_optimization.cpp` - Measure improvements
- `docs/OPTIMIZATION.md` - Document optimization strategy

**Implementation Steps**:

1. **Step 3.4.1**: Create circuit analysis pass
   - Traverse circuit graph
   - Identify optimization opportunities
   - Build dependency graph

2. **Step 3.4.2**: Implement filter pushdown
   - Detect filter nodes
   - Move filters before expensive operations (joins, aggregates)
   - Verify semantics-preserving transformation

3. **Step 3.4.3**: Implement projection pushdown
   - Track which columns are actually used
   - Eliminate unused column projections early
   - Reduce data volume through pipeline

4. **Step 3.4.4**: Implement operator fusion
   - Combine consecutive map operations
   - Combine consecutive filter operations
   - Reduce materialization overhead

5. **Step 3.4.5**: Benchmark improvements
   - Measure query execution time before/after optimization
   - Track memory usage reduction
   - Document speedup percentages

**Success Criteria**:
- ✅ Filter pushdown reduces rows processed
- ✅ Projection pushdown reduces memory usage
- ✅ Operator fusion reduces overhead
- ✅ Optimized circuits produce identical results
- ✅ Benchmark shows 20%+ improvement on complex queries
- ✅ All existing tests still pass

**Testing**:
- Unit: 5+ tests for each optimization rule
- Integration: Verify optimized and unoptimized produce same results
- Benchmark: Measure performance on TPC-H style queries

**Dependencies**: P2.4
**Blockers**: None
**Risk Level**: MEDIUM - Must preserve semantics

---

## 🟢 PHASE 4: ADVANCED FEATURES (Weeks 8+, Target: Mar 25+)

> **Note**: Phase 4 features are **deferred** because they are either:
> 1. Nice-to-have UX improvements (not core DBSP)
> 2. Anti-patterns to incremental computation
> 3. Can be implemented after core DBSP completeness

### P4.1: Automatic CDC via Transaction Hooks
**Priority**: 🟢 LOW (UX improvement, not core DBSP)
**Status**: NOT STARTED
**Effort**: 1-2 weeks
**Target Completion**: Mar 25 - Apr 5
**Depends On**: P3.4 (After all core features stable)

**Description**: Hook into DuckDB's transaction system for automatic CDC propagation
Current: Manual `SELECT dbsp_sync('table')` required after each update
Target: Automatic CDC on COMMIT, zero manual intervention

**Rationale for Deferral**:
- CDC mechanism is **external** to DBSP theory
- Current manual sync works correctly
- High implementation complexity vs benefit
- Should focus on DBSP correctness first

**Note**: See original P2.3 specification (now moved to P4.1) for full implementation details.

---

### P4.2: ORDER BY and LIMIT Support
**Priority**: 🟢 LOW (Anti-pattern to incremental)
**Status**: NOT STARTED
**Effort**: 5-7 days
**Target Completion**: Apr 6-12
**Depends On**: P3.4

**Description**: Implement ORDER BY and LIMIT/OFFSET for sorted, paginated results
Current: Error E102/E103 thrown
Target: `SELECT * FROM view ORDER BY column DESC LIMIT 10 OFFSET 5`

**Rationale for Deferral**:
- ORDER BY is **not a DBSP primitive** (not in relational algebra formalization)
- Breaks incremental semantics - entire order can change with one insert
- Degrades to O(n log n) on every update
- Client-side sorting of final results is more efficient
- DBSP papers don't discuss ORDER BY for good reason

**Implementation Caveats**:
- ⚠️ **Performance Warning**: This will be O(n log n) on updates, not O(δ)
- ⚠️ **Not Incremental**: Violates DBSP O(δ) guarantee
- ⚠️ **Better Alternative**: Query materialized view, sort client-side

**If Implemented**:
- Document performance characteristics clearly
- Warn users that ORDER BY views are not incremental
- Consider making it opt-in with explicit flag

**Note**: See original P2.1 specification (now moved to P4.2) for full implementation details if still desired.

---

### P4.3: Window Functions and Streaming Aggregates
**Priority**: 🟢 LOW (Advanced feature)
**Status**: NOT STARTED
**Effort**: 2-3 weeks
**Target Completion**: Apr 13 - May 3
**Depends On**: P4.1, P4.2

**Description**: Support window functions for time-based and row-based windows
Current: No window function support
Target: `SELECT AVG(price) OVER (ORDER BY time ROWS BETWEEN 10 PRECEDING AND CURRENT ROW)`

**Example Queries**:
```sql
-- Moving average
SELECT product_id, price,
       AVG(price) OVER (PARTITION BY product_id ORDER BY date ROWS BETWEEN 7 PRECEDING AND CURRENT ROW) as moving_avg
FROM sales;

-- Running total
SELECT order_id, amount,
       SUM(amount) OVER (ORDER BY order_date) as running_total
FROM orders;

-- Rank
SELECT name, salary,
       RANK() OVER (ORDER BY salary DESC) as salary_rank
FROM employees;
```

**DBSP Theory**: Window operators can be expressed in DBSP using time-domain transformations, similar to recursive queries but more complex.

**Files to Modify**:
- `dbsp_sql_parser.hpp` - Parse OVER clause
- New: `dbsp_window.hpp` - Window operator implementations
- `test/integration/test_window_functions.cpp` - Window tests

**Success Criteria**:
- ✅ ROW-based windows work correctly
- ✅ PARTITION BY works
- ✅ ORDER BY within window works
- ✅ Incremental maintenance of windows
- ✅ All standard window functions (RANK, DENSE_RANK, ROW_NUMBER, LAG, LEAD)

**Dependencies**: P4.1, P4.2
**Risk Level**: HIGH - Complex feature

---

### P4.4: Set Operations (UNION, INTERSECT, EXCEPT)
**Priority**: 🟢 LOW (Standard SQL, but not critical)
**Status**: NOT STARTED
**Effort**: 3-4 days
**Target Completion**: May 4-8
**Depends On**: P2.1 (DISTINCT)

**Description**: Support SQL set operations
Current: No set operation support
Target: `SELECT ... UNION SELECT ...` with incremental maintenance

**Example Queries**:
```sql
-- UNION
SELECT customer_id FROM orders
UNION
SELECT customer_id FROM subscriptions;

-- INTERSECT
SELECT product_id FROM inventory
INTERSECT
SELECT product_id FROM active_sales;

-- EXCEPT
SELECT employee_id FROM all_employees
EXCEPT
SELECT employee_id FROM terminated_employees;
```

**DBSP Theory**: Set operations are linear operators, easily incrementalized.

**Success Criteria**:
- ✅ UNION (with implicit DISTINCT)
- ✅ UNION ALL (bag union, no distinct)
- ✅ INTERSECT
- ✅ EXCEPT
- ✅ Incremental maintenance

**Dependencies**: P2.1 (DISTINCT)
**Risk Level**: LOW

---

## 📊 PHASE SUMMARY TABLE (REVISED)

| Phase | Tasks | Duration | Target End | Key Deliverables |
|-------|-------|----------|-----------|------------------|
| **1: Foundation** | P1.1, P1.2, P1.3 | 2 weeks | Feb 20 | DDL syntax, HAVING, MIN/MAX |
| **2: SQL Complete** | P2.1, P2.2, P2.3 | 3 weeks | Mar 10 | ORDER BY/LIMIT, Auto-CDC |
| **3: Production Ready** | P3.1, P3.2, P3.3 | 2 weeks | Mar 24 | Error messages, Concurrency, CI/CD |
| **TOTAL** | 9 tasks | 7 weeks | Mar 24 | Feature-complete MVP |

---

## 🎯 SUCCESS METRICS

After completing Phases 1-3 (Core DBSP Complete):

**DBSP Theoretical Completeness**:
- [x] Z-sets with integer weights ✅ (implemented)
- [x] Integration (I) and Differentiation (D) operators ✅ (implemented)
- [x] Incrementalization Q^Δ = D ∘ Q ∘ I ✅ (implemented)
- [x] Linear operators (filter, project) ✅ (implemented)
- [x] Bilinear operators (join) ✅ (implemented)
- [x] Core aggregates (SUM, COUNT, AVG) ✅ (implemented)
- [x] MIN/MAX aggregates ✅ (implemented in P1.3)
- [ ] MIN/MAX with O(log n) deletions (P2.2) 🔴 **CRITICAL**
- [ ] Distinct operator exposed in SQL (P2.1) 🔴 **CRITICAL**
- [ ] Recursive queries (WITH RECURSIVE) (P2.4) 🟡 **HIGH**
- [ ] Circuit optimization passes (P3.4) 🟡 **MEDIUM**

**Feature Completeness** (Core Features Only):
- [x] Core DBSP algorithms (completed)
- [x] Table tracking and CDC (completed)
- [x] Basic materialized views (completed)
- [x] Native SQL DDL syntax (P1.1) ✅
- [x] HAVING clause support (P1.2) ✅
- [x] MIN/MAX aggregates (P1.3) ✅
- [ ] **DISTINCT SQL integration** (P2.1) 🔴
- [ ] **MIN/MAX correctness fix** (P2.2) 🔴
- [ ] Complex JOIN predicates (P2.3) 🟡
- [ ] **Recursive queries** (P2.4) 🟡
- [ ] Enhanced error messages (P3.1)
- [ ] Concurrent query support (P3.2)
- [ ] CI/CD automation (P3.3)
- [ ] Circuit optimization (P3.4)

**Code Quality**:
- Test coverage: 85%+ (currently ~70%)
- All tests green on CI/CD
- No sanitizer warnings (ASAN, UBSAN, TSAN)
- Zero known security vulnerabilities

**Performance**:
- Concurrent reads: 3-5x faster than serial (with P3.2)
- CDC propagation: <10ms for 1000-row delta (currently ~5ms)
- Memory usage: <500MB for typical workloads

**User Experience**:
- Can use native SQL DDL syntax
- Clear error messages with actionable workarounds
- Automatic CDC eliminates manual sync calls
- Predictable top-N query results with ORDER BY/LIMIT

---

## 📝 NOTES (REVISED)

### Design Philosophy Change

**Previous Approach**: Focus on "SQL Completeness" - implementing standard SQL features
**New Approach**: Focus on **"DBSP Completeness"** - implementing DBSP theory correctly

**Rationale**:
1. This project should differentiate through **superior incremental computation**, not SQL feature parity
2. DBSP theory provides correctness guarantees that competitors lack
3. Features like ORDER BY that break incremental semantics should be deprioritized
4. Core DBSP features (DISTINCT, recursion) are more valuable than UX sugar (auto-CDC)

### Design Decisions

1. **DBSP Theory First**: Align roadmap with formal specification and research papers
2. **Correctness Over Convenience**: Fix MIN/MAX O(n) issue before adding new features
3. **Differentiation**: Recursive queries set this apart from basic IVM systems
4. **Defer Anti-Patterns**: ORDER BY degrades to O(n log n), conflicts with DBSP philosophy
5. **Testing Critical**: Every task includes comprehensive tests + theory validation

### Key Changes from Original TODO

**Promoted to Phase 2** (High Priority):
- P2.1: DISTINCT SQL (was missing) - Already implemented, easy win
- P2.2: MIN/MAX correctness (was noted as TODO) - O(δ) correctness critical
- P2.4: Recursive queries (was missing) - Core DBSP differentiator

**Demoted to Phase 4** (Deferred):
- P4.1: Auto-CDC (was P2.3) - UX improvement, not core theory
- P4.2: ORDER BY (was P2.1) - Anti-pattern to incremental semantics

**Added to Phase 3**:
- P3.4: Circuit optimization - DBSP theory provides proven optimization passes

### Known Risks (Updated)

1. **P2.3 Risk**: Automatic CDC requires deep DuckDB internals knowledge
   - Mitigation: Start with research phase, prototype early
   - Fallback: Keep manual sync as option

2. **P2.1 Risk**: ORDER BY incremental sorting is architecturally complex
   - Mitigation: Use simpler sorting strategy for v0.1
   - Fallback: Client-side sorting remains possible

3. **P3.2 Risk**: Shared mutexes can introduce subtle bugs
   - Mitigation: Thorough testing + ThreadSanitizer validation
   - Fallback: Can revert to single mutex if issues arise

### Estimated Velocity

- **Phase 1**: 2 weeks (assuming 6-8 hrs/day development)
- **Phase 2**: 3 weeks (includes research for P2.3)
- **Phase 3**: 2 weeks (mostly glue and testing)
- **Total**: 7 weeks to completion

### Recommended Start Date (UPDATED)

**NOW** - Start with **revised Phase 2** priorities:

**Week 1 (Feb 21-22)**: P2.1 - DISTINCT SQL Integration
- Quick win: Implementation already exists, just needs parser wiring
- 1-2 days effort
- High impact: Completes core DBSP relational operators

**Week 2 (Feb 24-27)**: P2.2 - Fix MIN/MAX Deletions  
- Critical correctness fix for O(δ) guarantee
- 3-4 days effort
- Use `std::multiset` for O(log n) deletion handling

**Week 3-4 (Feb 28 - Mar 3)**: P2.3 - Complex JOIN Predicates
- Extends existing correct join implementation
- 2-3 days effort

**Week 4-5 (Mar 4-10)**: P2.4 - Recursive Queries
- Core DBSP differentiator
- 1-2 weeks effort
- Study `recursive.lean` formal spec first

**Then proceed to Phase 3** (Production readiness)

---

## 📋 TRACKING UPDATES

**Created**: 2026-02-06
**Last Updated**: 2026-02-06
**Major Revision**: 2026-02-06 - Realigned with DBSP theory (see analysis.md)
**Current Phase**: Phase 1 Complete ✅ → Ready to start **revised Phase 2**

**Revision Summary**:
- Restructured Phase 2 from "SQL Completeness" to "DBSP Core Completeness"
- Added critical missing features (DISTINCT SQL, MIN/MAX fix, recursive queries)
- Deferred UX features (auto-CDC, ORDER BY) to Phase 4
- Added circuit optimization to Phase 3
- Aligned roadmap with DBSP formal specification and research papers

Track progress by:
1. Check off subtasks as they complete
2. Update status field (NOT STARTED → IN PROGRESS → COMPLETED)
3. Update Depends On sections as needed
4. Log blockers/risks encountered
5. **Review analysis.md for detailed DBSP theory alignment**

---

*For questions or design decisions, refer to comprehensive analysis at: `/Users/ravi/.gemini/antigravity/brain/7ad6fad7-6efe-4bd3-9211-425663792c42/analysis.md`*


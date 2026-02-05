# duckDBSP TODO List

This file tracks remaining features and improvements for the duckDBSP project.

## Priority Legend
- 🔴 **High Priority**: Essential for production readiness
- 🟡 **Medium Priority**: Important for quality and maintainability
- 🟢 **Low Priority**: Advanced features for future exploration

---

## 🔴 High Priority: SQL Feature Completeness

### Task #1: Implement HAVING clause for aggregate filtering
**Status**: Pending
**Description**: Add support for HAVING clause in SQL queries to filter aggregated results.
- Extend SQL parser to recognize HAVING syntax
- Add post-aggregation filter logic to materialized views
- Ensure incremental updates work correctly with HAVING conditions
- Add unit and integration tests for various HAVING scenarios

**Example**: `SELECT dept, COUNT(*) FROM employees GROUP BY dept HAVING COUNT(*) > 10`

---

### Task #2: Add ORDER BY and LIMIT support
**Status**: Pending
**Description**: Implement ORDER BY and LIMIT clauses for sorted and paginated results.
- Parse ORDER BY and LIMIT from SQL queries
- Handle ASC/DESC sorting on multiple columns
- Maintain sorted order incrementally during updates
- Add LIMIT and OFFSET for pagination
- Test with various data types and NULL handling

**Example**: `SELECT * FROM view ORDER BY name DESC LIMIT 100`

---

### Task #3: Implement MIN and MAX aggregate functions
**Status**: Pending
**Description**: Add MIN and MAX aggregates to complement existing SUM/COUNT/AVG.
- Extend aggregate function parsing in SQL parser
- Implement incremental MIN/MAX computation (challenging for deletions)
- Handle ties and NULL values correctly
- Consider using auxiliary data structures (e.g., sorted sets) for efficiency
- Add comprehensive tests for edge cases

**Example**: `SELECT dept, MIN(salary), MAX(salary) FROM employees GROUP BY dept`

---

### Task #4: Add subquery support
**Status**: Pending
**Description**: Implement nested SELECT statements (subqueries).
- Support subqueries in WHERE clauses (IN, EXISTS, etc.)
- Support subqueries in FROM clauses (derived tables)
- Handle correlated vs non-correlated subqueries
- Ensure proper dependency tracking for incremental updates
- Add tests for various nesting scenarios

**Example**: `SELECT * FROM orders WHERE customer_id IN (SELECT id FROM customers WHERE active = true)`

---

### Task #5: Implement set operations (UNION, INTERSECT, EXCEPT)
**Status**: Pending
**Description**: Add set operations for combining query results.
- Parse UNION / UNION ALL / INTERSECT / EXCEPT syntax
- Implement Z-set semantics for set operations (leveraging weights)
- Handle schema compatibility checks
- Ensure incremental updates propagate correctly through set operations
- Add tests for all combinations

**Example**: `SELECT * FROM view1 UNION SELECT * FROM view2`

---

### Task #6: Add window function support
**Status**: Pending
**Description**: Implement window functions for advanced analytics.
- Support ROW_NUMBER, RANK, DENSE_RANK
- Support PARTITION BY and ORDER BY in window specs
- Handle ROWS/RANGE frame specifications
- Implement incremental computation for window functions (challenging)
- Add comprehensive tests

**Example**: `SELECT *, ROW_NUMBER() OVER (PARTITION BY dept ORDER BY salary DESC) FROM employees`

---

## 🔴 High Priority: Robustness & Production Readiness

### Task #7: Improve error handling and input validation
**Status**: Pending
**Description**: Enhance robustness with comprehensive error handling.
- Add detailed error messages for invalid SQL syntax
- Validate view/table names more thoroughly
- Check for circular dependencies in cascading views
- Handle resource exhaustion gracefully
- Add bounds checking for all user inputs
- Create error recovery mechanisms
- Add tests for all error conditions

---

### Task #8: Add memory limits and resource management
**Status**: Pending
**Description**: Implement memory bounds and resource controls.
- Add configurable memory limits for views
- Implement eviction policies for large Z-sets
- Add monitoring for memory usage
- Handle out-of-memory conditions gracefully
- Consider disk-based overflow for very large views
- Add memory pressure tests

---

### Task #9: Fix NULL value edge cases
**Status**: Pending
**Description**: Ensure correct NULL handling throughout the system.
- Test NULL values in all aggregate functions
- Verify NULL behavior in GROUP BY and JOIN operations
- Handle NULL in comparison operators correctly
- Test NULL propagation through cascading views
- Ensure SQL standard compliance for NULL semantics
- Add comprehensive NULL-focused test suite

---

## 🔴 High Priority: Performance Optimization

### Task #10: Optimize performance with reader-writer locks
**Status**: Pending
**Description**: Replace single mutex with reader-writer locks for better concurrency.
- Identify read-heavy vs write-heavy code paths
- Replace std::mutex with std::shared_mutex where appropriate
- Allow multiple concurrent readers for view queries
- Ensure writer exclusivity for updates
- Benchmark performance improvements
- Add concurrency stress tests

---

### Task #11: Add query result caching
**Status**: Pending
**Description**: Implement caching for frequently-queried views.
- Cache materialized view query results
- Invalidate cache on updates
- Add configurable cache size limits
- Implement LRU or similar eviction policy
- Measure cache hit rates
- Add benchmarks showing cache effectiveness

---

### Task #17: Profile and optimize hot paths
**Status**: Pending
**Description**: Profile the code and optimize performance bottlenecks.
- Use profiling tools (perf, gprof, or similar)
- Identify hot paths in CDC propagation
- Optimize Z-set operations for large change sets
- Reduce unnecessary copies
- Benchmark before and after optimizations
- Document optimization strategies in ARCHITECTURE.md

---

## 🟡 Medium Priority: Infrastructure & DevOps

### Task #12: Set up GitHub Actions CI/CD pipeline
**Status**: Pending
**Description**: Create automated testing infrastructure.
- Set up GitHub Actions workflow for builds
- Run unit tests on every commit
- Run integration tests on every PR
- Run benchmarks and track performance over time
- Support multiple platforms (Linux, macOS, Windows)
- Add status badges to README
- Configure test result reporting

---

### Task #13: Add code coverage reporting
**Status**: Pending
**Description**: Set up code coverage tracking and reporting.
- Integrate gcov/lcov with CMake build
- Upload coverage reports to codecov.io or similar
- Add coverage badges to README
- Set coverage thresholds for PRs
- Identify untested code paths
- Aim for 90%+ coverage

---

### Task #14: Add sanitizer builds (ASAN, UBSAN, TSAN)
**Status**: Pending
**Description**: Enable sanitizers to catch bugs automatically.
- Add CMake options for AddressSanitizer (memory errors)
- Add UndefinedBehaviorSanitizer (UB detection)
- Add ThreadSanitizer (race conditions)
- Run sanitizer builds in CI
- Fix any issues discovered
- Document sanitizer usage in TESTING.md

---

### Task #15: Implement fuzzing with libFuzzer
**Status**: Pending
**Description**: Add fuzzing to discover edge cases.
- Create fuzz targets for SQL parser
- Create fuzz targets for Z-set operations
- Create fuzz targets for view updates
- Integrate with OSS-Fuzz or similar
- Run continuous fuzzing in CI
- Fix any crashes or hangs discovered

---

### Task #16: Improve logging and diagnostics
**Status**: Pending
**Description**: Add comprehensive logging infrastructure.
- Implement structured logging (spdlog or similar)
- Add log levels (DEBUG, INFO, WARN, ERROR)
- Log CDC propagation events
- Log performance metrics
- Add configurable log output (file, stdout, etc.)
- Include query execution plans in debug logs

---

## 🟢 Low Priority: Advanced Features (Future Work)

### Task #18: Design distributed DBSP architecture
**Status**: Pending
**Description**: Research and design distributed DBSP for scalability.
- Research distributed dataflow systems (Naiad, Differential Dataflow)
- Design partitioning strategy for Z-sets
- Design communication protocol between nodes
- Consider consistency guarantees
- Prototype simple distributed version
- Document design decisions

**Note**: This is a lower priority, advanced feature for future work.

---

### Task #19: Add streaming input connectors
**Status**: Pending
**Description**: Enable streaming data sources beyond manual CDC.
- Design connector interface for external streams
- Implement Kafka connector
- Implement file-based stream (CSV, JSON streaming)
- Add backpressure handling
- Add offset/checkpoint management
- Document connector API

**Note**: This is a lower priority, advanced feature for future work.

---

## Summary Statistics

| Category | Count | Completed |
|----------|-------|-----------|
| 🔴 SQL Features | 6 | 0 |
| 🔴 Robustness | 3 | 0 |
| 🔴 Performance | 3 | 0 |
| 🟡 Infrastructure | 5 | 0 |
| 🟢 Advanced Features | 2 | 0 |
| **Total** | **19** | **0** |

**Overall Progress**: 0% (0/19 tasks completed)

---

## Recommended Order

For maximum impact, consider tackling tasks in this order:

1. **Task #7**: Error handling (foundation for reliability)
2. **Task #12**: CI/CD pipeline (automate quality checks)
3. **Task #9**: NULL edge cases (data correctness)
4. **Task #1**: HAVING clause (most commonly requested SQL feature)
5. **Task #3**: MIN/MAX aggregates (completes basic aggregates)
6. **Task #2**: ORDER BY/LIMIT (essential for usability)
7. **Task #10**: Reader-writer locks (performance quick win)
8. Continue with remaining features based on user needs

---

*Last Updated*: 2026-02-05

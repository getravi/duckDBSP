# duckDBSP Development Roadmap

This document outlines both near-term development priorities and long-term vision for the duckDBSP project.

**Last Updated**: 2026-02-08
**Current Version**: v3.0 (Production-Ready)
**Test Status**: ✅ 20/20 tests passing (1000+ assertions)

---

## 📊 Current Status

| Phase | Status | Completion Date |
|-------|--------|----------------|
| **Phase 1: Foundation** | ✅ COMPLETE | 2026-02-06 |
| **Phase 2: Core DBSP** | ✅ COMPLETE | 2026-02-07 |
| **Phase 3: Production** | ✅ COMPLETE | 2026-02-07 |
| **Phase 4: Advanced SQL** | 🟡 PARTIAL | 2026-02-08 |
| **Phase 5: Automation** | 🟡 PARTIAL | 2026-02-07 |
| **Phase 6: Infrastructure** | ⚪️ PLANNED | Q1 2026 |

**Completed Features** (see [CHANGELOG.md](CHANGELOG.md) for details):
- ✅ Native DDL syntax (`CREATE MATERIALIZED VIEW`)
- ✅ HAVING clause filtering
- ✅ MIN/MAX aggregates with O(log n) incremental updates
- ✅ DISTINCT operator
- ✅ Complex JOIN predicates
- ✅ Recursive queries (`WITH RECURSIVE`)
- ✅ Enhanced error messages with structured error codes
- ✅ Reader-writer locks for concurrency
- ✅ Circuit optimization (filter pushdown, projection pruning, operator fusion)
- ✅ ORDER BY & LIMIT support
- ✅ Advanced window functions (LAG, LEAD, FIRST_VALUE, etc.)
- ✅ Non-recursive CTEs and subqueries

---

## 🎯 Near-Term Priorities (Q1-Q2 2026)

### Phase 4: Remaining Advanced SQL Features

#### P4.4: Set Operations (UNION, INTERSECT, EXCEPT)
**Priority**: 🟢 LOW
**Status**: NOT STARTED
**Effort**: 3-4 days
**Target**: May 2026

**Description**: Incremental maintenance for SQL set operations.

**Example Queries**:
```sql
-- UNION (implicit DISTINCT)
SELECT customer_id FROM orders
UNION
SELECT customer_id FROM subscriptions;

-- UNION ALL (bag union)
SELECT * FROM sales_2024 UNION ALL SELECT * FROM sales_2025;

-- INTERSECT
SELECT product_id FROM inventory
INTERSECT
SELECT product_id FROM active_sales;

-- EXCEPT
SELECT employee_id FROM all_employees
EXCEPT
SELECT employee_id FROM terminated;
```

**DBSP Theory**: Set operations are linear operators, easily incrementalized using Z-set arithmetic.

**Success Criteria**:
- ✅ UNION with implicit DISTINCT
- ✅ UNION ALL (bag union, no distinct)
- ✅ INTERSECT
- ✅ EXCEPT
- ✅ Incremental maintenance with O(delta) updates

---

### Phase 5: Automation & Usability

#### P5.2: Automatic CDC (BROKEN - Needs Fix)
**Priority**: 🟡 MEDIUM
**Status**: INCOMPLETE
**Blocker**: DuckDB transaction hooks not triggering

**Current Implementation**:
- [x] Added `enable_auto_sync()`/`disable_auto_sync()` to CDCManager
- [x] Added `dbsp_auto_sync(true/false)` table function
- [ ] **FIX NEEDED**: `ClientContextState::TransactionCommit` not being called
- [ ] **Research**: Alternative hooks (e.g., `ExtensionCallback::OnTransactionCommit`)

**Goal**: Automatically sync tracked tables on transaction commit without manual `dbsp_sync()` calls.

---

### Phase 6: Infrastructure & DevOps

#### P6.1: CI/CD Pipeline
**Priority**: 🟡 MEDIUM
**Status**: NOT STARTED
**Effort**: 2-3 days
**Target**: March 2026

**Deliverables**:
- `.github/workflows/build-test.yml` - Build + unit tests on every commit
- `.github/workflows/integration-test.yml` - Full integration tests on PRs
- `.github/workflows/benchmark.yml` - Performance benchmarks on release
- Status badges in README.md

**Success Criteria**:
- ✅ Automated builds on Linux and macOS
- ✅ All tests run on every commit/PR
- ✅ Performance regression detection
- ✅ Public build status visibility

---

## 🚀 Long-Term Vision (2026+)

### 1. Robust Durability & Fault Tolerance

**Goal**: Production-grade data integrity across crashes and restarts.

- [ ] **WAL/Checkpoint Integration**: Deep integration with DuckDB's Write-Ahead Log to persist Z-Set states
- [ ] **Atomic Metadata Sync**: Ensure DBSP dependency graph updates are atomic with DuckDB transactions
- [ ] **Recovery Testing**: Automated crash-recovery tests to verify zero-data-loss guarantees
- [ ] **Checkpointing**: Periodic snapshots of view states for fast recovery

**Why it matters**: Enables mission-critical deployments where data loss is unacceptable.

---

### 2. Resource Management & Scalability

**Goal**: Handle "Big Data" workloads within DuckDB's memory constraints.

- [ ] **Spill-to-Disk**: Leverage DuckDB's Buffer Manager to page large Z-Sets to disk (prevent OOM)
- [ ] **Memory Quotas**: Per-session and per-view memory accounting and limits
- [ ] **Parallel Sync**: Multi-threaded CDC propagation for high-throughput batch inserts
- [ ] **Adaptive Operator Selection**: Choose between materialized and streaming operators based on cardinality

**Why it matters**: Scales from laptop analytics to data warehouse workloads.

---

### 3. SQL & Ecosystem Parity

**Goal**: Drop-in replacement for standard materialized views with advanced features.

- [ ] **Schema Evolution**: Handle `ALTER TABLE` operations (Add/Drop/Rename column) without invalidating views
- [ ] **Correlated Subqueries**: Support `LATERAL` joins and complex nested queries
- [ ] **BI Tool Compatibility**: Validation against Tableau, Looker, Metabase (complex SQL generation)
- [ ] **Standard SQL Compliance**: Support PostgreSQL-compatible materialized view syntax

**Why it matters**: Seamless integration with existing SQL workflows and BI tools.

---

### 4. Observability & Operations

**Goal**: Transparency into the "black box" of incremental computation.

- [ ] **EXPLAIN PLAN (Streaming)**: `EXPLAIN MATERIALIZED VIEW` shows DBSP circuit structure
- [ ] **Operational Metrics**: `dbsp_stats()` view with:
  - Sync latency (average/p50/p99)
  - Memory usage per operator
  - Delta sizes per table
  - Circuit execution time
- [ ] **Audit Logging**: Detailed CDC event logs for debugging data propagation
- [ ] **View Lineage**: Track data lineage from source tables through view dependencies

**Why it matters**: Enables troubleshooting, performance tuning, and capacity planning.

---

### 5. Advanced DBSP Theory Features

**Goal**: Leverage unique DBSP capabilities beyond traditional incremental view maintenance.

- [ ] **Dynamic Topology**: Modify view definitions (e.g., add filters) without dropping and recreating
- [ ] **Generalized Fixed-Point Iteration**: Graph algorithms (PageRank, Shortest Path) with configurable convergence
- [ ] **Nested Circuits**: Hierarchical time domains (as specified in DBSP formalization)
- [ ] **Higher-Order Operators**: Support for nested relations and complex aggregates
- [ ] **Provenance Tracking**: Track which source rows contribute to each output row

**Why it matters**: Unlocks advanced analytics patterns impossible with traditional materialized views.

---

### 6. Performance & Optimization

**Goal**: Industry-leading incremental maintenance performance.

- [ ] **Just-In-Time Compilation**: Compile DBSP circuits to native code for hot paths
- [ ] **Vectorized Execution**: SIMD-optimized operators for high throughput
- [ ] **Incremental Joins V2**: Worst-case optimal join algorithms for complex patterns
- [ ] **Compressed Z-Sets**: Run-length encoding for sparse delta batches
- [ ] **Query Result Caching**: Cache intermediate results for common query patterns

**Why it matters**: Competitive advantage in streaming analytics and real-time dashboards.

---

## 📅 Release Schedule

| Version | Target Date | Focus Areas |
|---------|-------------|-------------|
| **v3.1** | March 2026 | CI/CD, Set Operations |
| **v3.2** | May 2026 | Auto CDC fix, Enhanced observability |
| **v4.0** | Q3 2026 | Durability (WAL integration), Schema evolution |
| **v5.0** | Q4 2026 | Scalability (spill-to-disk), Advanced analytics |

---

## 🎯 Success Metrics

### DBSP Theoretical Completeness
- [x] Z-sets with integer weights ✅
- [x] Integration (I) and Differentiation (D) operators ✅
- [x] Incrementalization Q^Δ = D ∘ Q ∘ I ✅
- [x] Linear operators (filter, project) ✅
- [x] Bilinear operators (join) ✅
- [x] Core aggregates (SUM, COUNT, AVG) ✅
- [x] MIN/MAX aggregates with O(log n) deletions ✅
- [x] DISTINCT operator ✅
- [x] Recursive queries (WITH RECURSIVE) ✅
- [x] Circuit optimization passes ✅
- [ ] Dynamic topology updates
- [ ] Nested circuits

### Production Readiness
- [x] Comprehensive test coverage (20/20 tests, 1000+ assertions) ✅
- [x] Concurrent query support ✅
- [x] Structured error messages ✅
- [ ] CI/CD automation
- [ ] Crash recovery
- [ ] Performance benchmarks published

### Ecosystem Integration
- [ ] Published to DuckDB Community Extensions
- [ ] PyPI package (Python bindings)
- [ ] npm package (Node.js bindings)
- [ ] Docker images
- [ ] Documentation site (docs.duckdbsp.org)

---

## 🤝 Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup and guidelines.

**High-Priority Contributions Wanted**:
1. Auto CDC transaction hook fix (P5.2)
2. GitHub Actions CI/CD pipeline (P6.1)
3. Set operations implementation (P4.4)
4. Performance benchmarking suite
5. Documentation improvements

---

## 📚 References

- [DBSP Paper (VLDB 2023)](https://www.vldb.org/pvldb/vol16/p1601-budiu.pdf)
- [Feldera: DBSP in Production](https://www.feldera.com/)
- [DBSP Theory (Lean Formalization)](https://github.com/tchajed/database-stream-processing-theory)

---

**For detailed historical changes, see [CHANGELOG.md](CHANGELOG.md)**
**For API documentation, see [docs/API.md](docs/API.md)**
**For architecture details, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**

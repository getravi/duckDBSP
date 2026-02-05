#include <catch2/catch_test_macros.hpp>
#include "../../duckdb_extension/dbsp_cdc.hpp"

using namespace dbsp_native;

// Note: These tests require a DuckDB context, so they bridge unit/integration
// For pure unit testing, we'd need to mock the DuckDB dependency

TEST_CASE("DependencyGraph detects cycles", "[cdc][deps]") {
    DependencyGraph graph;

    graph.add_dependency("v2", "v1");
    graph.add_dependency("v3", "v2");

    // Creating a cycle should be detected
    bool would_cycle = graph.would_create_cycle("v1", "v3");
    REQUIRE(would_cycle);

    // Non-cycle should be fine
    bool would_not_cycle = graph.would_create_cycle("v4", "v3");
    REQUIRE_FALSE(would_not_cycle);
}

TEST_CASE("DependencyGraph topological sort", "[cdc][deps]") {
    DependencyGraph graph;

    graph.add_dependency("v2", "v1");
    graph.add_dependency("v3", "v2");
    graph.add_dependency("v4", "v1");

    auto order = graph.topological_order("v1");

    // v2 and v4 depend on v1, v3 depends on v2
    // Valid orders: [v2, v4, v3] or [v4, v2, v3] or [v2, v3, v4]
    REQUIRE(order.size() == 3);

    // v3 must come after v2
    auto v2_pos = std::find(order.begin(), order.end(), "v2");
    auto v3_pos = std::find(order.begin(), order.end(), "v3");
    REQUIRE(v2_pos < v3_pos);
}

TEST_CASE("DependencyGraph remove node", "[cdc][deps]") {
    DependencyGraph graph;

    graph.add_dependency("v2", "v1");
    graph.add_dependency("v3", "v2");

    graph.remove_node("v2");

    // v2 and its edges should be gone
    auto order = graph.topological_order("v1");
    REQUIRE(std::find(order.begin(), order.end(), "v2") == order.end());
}

TEST_CASE("DependencyGraph get dependents", "[cdc][deps]") {
    DependencyGraph graph;

    graph.add_dependency("v2", "v1");
    graph.add_dependency("v3", "v1");
    graph.add_dependency("v4", "v2");

    auto deps = graph.get_dependents("v1");

    REQUIRE(deps.size() == 2);
    REQUIRE(std::find(deps.begin(), deps.end(), "v2") != deps.end());
    REQUIRE(std::find(deps.begin(), deps.end(), "v3") != deps.end());
}

TEST_CASE("DependencyGraph transitive dependents", "[cdc][deps]") {
    DependencyGraph graph;

    graph.add_dependency("v2", "v1");
    graph.add_dependency("v3", "v2");
    graph.add_dependency("v4", "v3");

    // All of v2, v3, v4 transitively depend on v1
    auto all_deps = graph.get_all_dependents("v1");

    REQUIRE(all_deps.size() == 3);
    REQUIRE(std::find(all_deps.begin(), all_deps.end(), "v2") != all_deps.end());
    REQUIRE(std::find(all_deps.begin(), all_deps.end(), "v3") != all_deps.end());
    REQUIRE(std::find(all_deps.begin(), all_deps.end(), "v4") != all_deps.end());
}

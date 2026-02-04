// DBSP Z-Set and Operator Tests

#include "../include/dbsp_zset.hpp"
#include "../include/dbsp_stream.hpp"
#include "../include/dbsp_circuit.hpp"
#include "../include/dbsp_materialized_view.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace dbsp;

// Test helper macros
#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " #name "... "; \
    test_##name(); \
    std::cout << "PASSED\n"; \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "Assertion failed: " << #a << " != " << #b << "\n"; \
        std::cerr << "  " << (a) << " != " << (b) << "\n"; \
        std::abort(); \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        std::cerr << "Assertion failed: " << #x << "\n"; \
        std::abort(); \
    } \
} while(0)

// Basic Z-set tests
TEST(zset_basic) {
    ZSet<int> zs;
    ASSERT_TRUE(zs.empty());
    ASSERT_EQ(zs.support_size(), 0u);

    // Insert elements
    zs.insert(1, 1);
    zs.insert(2, 2);
    zs.insert(3, -1);

    ASSERT_EQ(zs[1], 1);
    ASSERT_EQ(zs[2], 2);
    ASSERT_EQ(zs[3], -1);
    ASSERT_EQ(zs[4], 0);  // Not present
    ASSERT_EQ(zs.support_size(), 3u);

    // Insert same element again
    zs.insert(1, 2);
    ASSERT_EQ(zs[1], 3);

    // Insert to cancel out
    zs.insert(3, 1);
    ASSERT_EQ(zs[3], 0);
    ASSERT_TRUE(!zs.contains(3));  // Should be removed
    ASSERT_EQ(zs.support_size(), 2u);
}

TEST(zset_arithmetic) {
    ZSet<int> zs1, zs2;

    zs1.insert(1, 2);
    zs1.insert(2, 3);

    zs2.insert(2, -1);
    zs2.insert(3, 4);

    // Addition
    auto sum = zs1 + zs2;
    ASSERT_EQ(sum[1], 2);
    ASSERT_EQ(sum[2], 2);  // 3 + (-1)
    ASSERT_EQ(sum[3], 4);

    // Subtraction
    auto diff = zs1 - zs2;
    ASSERT_EQ(diff[1], 2);
    ASSERT_EQ(diff[2], 4);  // 3 - (-1)
    ASSERT_EQ(diff[3], -4);

    // Negation
    auto neg = -zs1;
    ASSERT_EQ(neg[1], -2);
    ASSERT_EQ(neg[2], -3);
}

TEST(zset_distinct) {
    ZSet<int> zs;
    zs.insert(1, 5);
    zs.insert(2, -3);
    zs.insert(3, 1);

    auto distinct = zs.distinct();
    ASSERT_EQ(distinct[1], 1);  // 5 > 0 -> 1
    ASSERT_EQ(distinct[2], 0);  // -3 <= 0 -> removed
    ASSERT_EQ(distinct[3], 1);  // 1 > 0 -> 1
    ASSERT_EQ(distinct.support_size(), 2u);
}

TEST(zset_map) {
    ZSet<int> zs;
    zs.insert(1, 2);
    zs.insert(2, 3);
    zs.insert(3, 1);

    auto mapped = zs.map<int>([](int x) { return x * 2; });
    ASSERT_EQ(mapped[2], 2);   // 1*2 with weight 2
    ASSERT_EQ(mapped[4], 3);   // 2*2 with weight 3
    ASSERT_EQ(mapped[6], 1);   // 3*2 with weight 1
}

TEST(zset_filter) {
    ZSet<int> zs;
    zs.insert(1, 1);
    zs.insert(2, 2);
    zs.insert(3, 3);
    zs.insert(4, 4);

    auto filtered = zs.filter([](int x) { return x % 2 == 0; });
    ASSERT_EQ(filtered[1], 0);
    ASSERT_EQ(filtered[2], 2);
    ASSERT_EQ(filtered[3], 0);
    ASSERT_EQ(filtered[4], 4);
    ASSERT_EQ(filtered.support_size(), 2u);
}

// Indexed Z-set tests
TEST(indexed_zset_basic) {
    IndexedZSet<std::string, int> izs;
    ASSERT_TRUE(izs.empty());

    izs.insert("a", 1, 2);
    izs.insert("a", 2, 3);
    izs.insert("b", 1, 1);

    ASSERT_EQ(izs["a"][1], 2);
    ASSERT_EQ(izs["a"][2], 3);
    ASSERT_EQ(izs["b"][1], 1);
    ASSERT_TRUE(izs.contains_key("a"));
    ASSERT_TRUE(izs.contains_key("b"));
    ASSERT_TRUE(!izs.contains_key("c"));
}

// Stream operator tests
TEST(integration_operator) {
    Integration<int> integrate;

    ZSet<int> delta1;
    delta1.insert(1, 1);
    delta1.insert(2, 2);

    auto result1 = integrate.process(delta1);
    ASSERT_EQ(result1[1], 1);
    ASSERT_EQ(result1[2], 2);

    ZSet<int> delta2;
    delta2.insert(2, 3);
    delta2.insert(3, 1);

    auto result2 = integrate.process(delta2);
    ASSERT_EQ(result2[1], 1);
    ASSERT_EQ(result2[2], 5);  // 2 + 3
    ASSERT_EQ(result2[3], 1);
}

TEST(delay_operator) {
    Delay<int> delay;

    ZSet<int> input1;
    input1.insert(1, 1);

    auto output1 = delay.process(input1);
    ASSERT_TRUE(output1.empty());  // First output is empty

    ZSet<int> input2;
    input2.insert(2, 2);

    auto output2 = delay.process(input2);
    ASSERT_EQ(output2[1], 1);  // Previous input
    ASSERT_EQ(output2[2], 0);

    auto output3 = delay.process(ZSet<int>{});
    ASSERT_EQ(output3[2], 2);  // Previous input
}

TEST(incremental_distinct) {
    IncrementalDistinct<int> distinct;

    // Insert element with weight 2
    ZSet<int> delta1;
    delta1.insert(1, 2);
    auto result1 = distinct.process(delta1);
    ASSERT_EQ(result1[1], 1);  // Became positive

    // Insert same element with weight 1
    ZSet<int> delta2;
    delta2.insert(1, 1);
    auto result2 = distinct.process(delta2);
    ASSERT_TRUE(result2.empty());  // Still positive, no change

    // Delete enough to make it non-positive
    ZSet<int> delta3;
    delta3.insert(1, -3);
    auto result3 = distinct.process(delta3);
    ASSERT_EQ(result3[1], -1);  // Became non-positive

    // Re-insert
    ZSet<int> delta4;
    delta4.insert(1, 1);
    auto result4 = distinct.process(delta4);
    ASSERT_EQ(result4[1], 1);  // Became positive again
}

TEST(incremental_join) {
    // Test the bilinear join formula manually using simple indexed maps
    // This tests the join logic without complex hash templates

    std::unordered_map<int, ZSet<int>> left_indexed;
    std::unordered_map<int, ZSet<int>> right_indexed;
    ZSet<std::pair<int, int>, PairHash<int, int>> result;

    // Helper to compute incremental join
    auto join_left_delta = [&](const ZSet<int>& delta) {
        for (const auto& [a, wa] : delta) {
            int key = a;  // key is the value itself
            // Join with integrated right
            auto it = right_indexed.find(key);
            if (it != right_indexed.end()) {
                for (const auto& [b, wb] : it->second) {
                    result.insert(std::make_pair(a, b), wa * wb);
                }
            }
            // Update integrated left
            left_indexed[key].insert(a, wa);
        }
    };

    auto join_right_delta = [&](const ZSet<int>& delta) {
        for (const auto& [b, wb] : delta) {
            int key = b;  // key is the value itself
            // Join with integrated left
            auto it = left_indexed.find(key);
            if (it != left_indexed.end()) {
                for (const auto& [a, wa] : it->second) {
                    result.insert(std::make_pair(a, b), wa * wb);
                }
            }
            // Update integrated right
            right_indexed[key].insert(b, wb);
        }
    };

    ZSet<int> deltaA, deltaB;
    deltaA.insert(1, 1);
    deltaA.insert(2, 1);

    deltaB.insert(1, 1);
    deltaB.insert(3, 1);

    // Process left delta first
    join_left_delta(deltaA);
    // Then process right delta
    join_right_delta(deltaB);

    // Should have (1,1) from the join
    ASSERT_EQ(result[std::make_pair(1, 1)], 1);
    ASSERT_EQ(result.support_size(), 1u);

    // Add more to B
    ZSet<int> deltaB2;
    deltaB2.insert(2, 1);

    join_right_delta(deltaB2);
    // Should now also have (2,2)
    ASSERT_EQ(result[std::make_pair(2, 2)], 1);
    ASSERT_EQ(result.support_size(), 2u);
}

// Materialized view tests
TEST(filter_view) {
    using RowZSet = ZSet<Row, RowHash>;

    FilteredView view(
        "test_filter",
        "test_table",
        [](const Row& row) {
            return std::get<int64_t>(row.columns[0]) > 10;
        }
    );

    RowZSet changes;
    Row r1, r2, r3;
    r1.columns = {int64_t(5)};
    r2.columns = {int64_t(15)};
    r3.columns = {int64_t(20)};

    changes.insert(r1, 1);
    changes.insert(r2, 1);
    changes.insert(r3, 1);

    view.apply_changes("test_table", changes);

    const auto& result = view.get_result();
    ASSERT_EQ(result[r1], 0);  // Filtered out
    ASSERT_EQ(result[r2], 1);
    ASSERT_EQ(result[r3], 1);
    ASSERT_EQ(view.row_count(), 2u);
}

TEST(aggregate_view) {
    using RowZSet = ZSet<Row, RowHash>;

    AggregateView view(
        "test_agg",
        "test_table",
        [](const Row& row) -> Row {
            Row key;
            key.columns.push_back(row.columns[0]);  // Group by first column
            return key;
        },
        [](const Row& row) -> int64_t {
            return std::get<int64_t>(row.columns[1]);  // Sum second column
        },
        AggregateView::AggType::SUM
    );

    RowZSet changes;
    Row r1, r2, r3;
    r1.columns = {std::string("a"), int64_t(10)};
    r2.columns = {std::string("a"), int64_t(20)};
    r3.columns = {std::string("b"), int64_t(5)};

    changes.insert(r1, 1);
    changes.insert(r2, 1);
    changes.insert(r3, 1);

    view.apply_changes("test_table", changes);

    const auto& result = view.get_result();
    // Should have (a, 30) and (b, 5)
    ASSERT_EQ(result.support_size(), 2u);
}

// Circuit tests
TEST(circuit_basic) {
    Circuit circuit;

    // Create a simple pipeline: source -> filter -> sink
    auto source_id = circuit.next_node_id();
    auto source = circuit.add_node(
        std::make_unique<SourceNode<int>>(source_id, "source")
    );

    auto filter_id = circuit.next_node_id();
    auto filter = circuit.add_node(
        std::make_unique<FilterNode<int>>(
            filter_id,
            [source]() -> const ZSet<int>& { return source->output(); },
            [](int x) { return x % 2 == 0; },
            "filter"
        )
    );

    auto sink_id = circuit.next_node_id();
    auto sink = circuit.add_node(
        std::make_unique<SinkNode<int>>(
            sink_id,
            [filter]() -> const ZSet<int>& { return filter->output(); },
            "sink"
        )
    );

    // Push some data
    source->insert(1, 1);
    source->insert(2, 1);
    source->insert(3, 1);
    source->insert(4, 1);

    // Execute one step
    circuit.step();

    // Check result
    const auto& result = sink->materialized();
    ASSERT_EQ(result[1], 0);
    ASSERT_EQ(result[2], 1);
    ASSERT_EQ(result[3], 0);
    ASSERT_EQ(result[4], 1);
}

int main() {
    std::cout << "=== DBSP Unit Tests ===\n\n";

    // Z-set tests
    RUN_TEST(zset_basic);
    RUN_TEST(zset_arithmetic);
    RUN_TEST(zset_distinct);
    RUN_TEST(zset_map);
    RUN_TEST(zset_filter);
    RUN_TEST(indexed_zset_basic);

    // Stream operator tests
    RUN_TEST(integration_operator);
    RUN_TEST(delay_operator);
    RUN_TEST(incremental_distinct);
    RUN_TEST(incremental_join);

    // Materialized view tests
    RUN_TEST(filter_view);
    RUN_TEST(aggregate_view);

    // Circuit tests
    RUN_TEST(circuit_basic);

    std::cout << "\n=== All Tests Passed ===\n";
    return 0;
}

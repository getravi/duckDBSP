// DBSP DuckDB Extension Implementation
// Provides SQL functions for creating and querying real-time materialized views

#include "../include/dbsp_materialized_view.hpp"

#include <iostream>
#include <memory>
#include <sstream>
#include <string>

// Note: This is a standalone implementation. For actual DuckDB integration,
// you would include DuckDB headers and use its extension API.
// For now, this provides a complete working implementation that demonstrates
// the DBSP algorithms for incremental view maintenance.

namespace dbsp {

// Global view manager instance
static MaterializedViewManager g_view_manager;

// Get the global view manager
MaterializedViewManager& get_view_manager() {
    return g_view_manager;
}

// Helper to create a Row from variadic arguments
template<typename... Args>
Row make_row(Args&&... args) {
    Row row;
    row.columns = {std::forward<Args>(args)...};
    return row;
}

// Example SQL-like interface (for demonstration)
class DBSPExtension {
public:
    // Create a filtered view
    // SQL equivalent: CREATE MATERIALIZED VIEW name AS SELECT * FROM table WHERE condition
    static void create_filter_view(
        const std::string& view_name,
        const std::string& source_table,
        std::function<bool(const Row&)> predicate
    ) {
        auto view = std::make_unique<FilteredView>(view_name, source_table, predicate);
        get_view_manager().register_view(std::move(view));
    }

    // Create a projection view
    // SQL equivalent: CREATE MATERIALIZED VIEW name AS SELECT col1, col2 FROM table
    static void create_projection_view(
        const std::string& view_name,
        const std::string& source_table,
        std::function<Row(const Row&)> projection
    ) {
        auto view = std::make_unique<ProjectedView>(view_name, source_table, projection);
        get_view_manager().register_view(std::move(view));
    }

    // Create a join view
    // SQL equivalent: CREATE MATERIALIZED VIEW name AS SELECT * FROM t1 JOIN t2 ON t1.key = t2.key
    static void create_join_view(
        const std::string& view_name,
        const std::string& left_table,
        const std::string& right_table,
        std::function<std::string(const Row&)> left_key,
        std::function<std::string(const Row&)> right_key
    ) {
        auto view = std::make_unique<JoinView>(
            view_name, left_table, right_table, left_key, right_key
        );
        get_view_manager().register_view(std::move(view));
    }

    // Create an aggregate view
    // SQL equivalent: CREATE MATERIALIZED VIEW name AS SELECT key, SUM(value) FROM table GROUP BY key
    static void create_aggregate_view(
        const std::string& view_name,
        const std::string& source_table,
        std::function<Row(const Row&)> key_fn,
        std::function<int64_t(const Row&)> value_fn,
        AggregateView::AggType agg_type
    ) {
        auto view = std::make_unique<AggregateView>(
            view_name, source_table, key_fn, value_fn, agg_type
        );
        get_view_manager().register_view(std::move(view));
    }

    // Create a distinct view
    // SQL equivalent: CREATE MATERIALIZED VIEW name AS SELECT DISTINCT * FROM table
    static void create_distinct_view(
        const std::string& view_name,
        const std::string& source_table
    ) {
        auto view = std::make_unique<DistinctView>(view_name, source_table);
        get_view_manager().register_view(std::move(view));
    }

    // Insert a row into a table (propagates to all views)
    static void insert(const std::string& table_name, const Row& row) {
        get_view_manager().insert_row(table_name, row);
    }

    // Delete a row from a table (propagates to all views)
    static void remove(const std::string& table_name, const Row& row) {
        get_view_manager().delete_row(table_name, row);
    }

    // Update a row in a table (propagates to all views)
    static void update(const std::string& table_name, const Row& old_row, const Row& new_row) {
        get_view_manager().update_row(table_name, old_row, new_row);
    }

    // Query a materialized view
    static const ZSet<Row, RowHash>* query_view(const std::string& view_name) {
        auto* view = get_view_manager().get_view(view_name);
        if (view) {
            return &view->get_result();
        }
        return nullptr;
    }

    // Get the latest changes to a view
    static const ZSet<Row, RowHash>* query_view_delta(const std::string& view_name) {
        auto* view = get_view_manager().get_view(view_name);
        if (view) {
            return &view->get_delta();
        }
        return nullptr;
    }

    // Drop a view
    static bool drop_view(const std::string& view_name) {
        return get_view_manager().remove_view(view_name);
    }

    // List all views
    static std::vector<std::string> list_views() {
        return get_view_manager().list_views();
    }
};

} // namespace dbsp

// Demo/test program
#ifdef DBSP_STANDALONE_TEST

#include <cassert>

void print_zset(const dbsp::ZSet<dbsp::Row, dbsp::RowHash>& zset, const std::string& name) {
    std::cout << "\n" << name << " (" << zset.support_size() << " rows):\n";
    for (const auto& [row, weight] : zset) {
        std::cout << "  [";
        for (size_t i = 0; i < row.columns.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::visit([](const auto& v) { std::cout << v; }, row.columns[i]);
        }
        std::cout << "] weight=" << weight << "\n";
    }
}

int main() {
    using namespace dbsp;

    std::cout << "=== DBSP DuckDB Extension Demo ===\n";

    // Create test data
    // Table: orders(order_id, customer_id, amount)
    // Table: customers(customer_id, name)

    // Create a filter view: orders with amount > 100
    DBSPExtension::create_filter_view(
        "high_value_orders",
        "orders",
        [](const Row& row) {
            return std::get<int64_t>(row.columns[2]) > 100;
        }
    );

    // Create an aggregate view: total amount per customer
    DBSPExtension::create_aggregate_view(
        "customer_totals",
        "orders",
        [](const Row& row) -> Row {
            Row key;
            key.columns.push_back(row.columns[1]); // customer_id
            return key;
        },
        [](const Row& row) -> int64_t {
            return std::get<int64_t>(row.columns[2]); // amount
        },
        AggregateView::AggType::SUM
    );

    // Create a join view: orders with customer names
    DBSPExtension::create_join_view(
        "orders_with_customers",
        "orders",
        "customers",
        [](const Row& row) -> std::string {
            return std::to_string(std::get<int64_t>(row.columns[1])); // customer_id
        },
        [](const Row& row) -> std::string {
            return std::to_string(std::get<int64_t>(row.columns[0])); // customer_id
        }
    );

    std::cout << "\n--- Initial inserts ---\n";

    // Insert customers
    DBSPExtension::insert("customers", make_row(int64_t(1), std::string("Alice")));
    DBSPExtension::insert("customers", make_row(int64_t(2), std::string("Bob")));
    DBSPExtension::insert("customers", make_row(int64_t(3), std::string("Charlie")));

    // Insert orders
    DBSPExtension::insert("orders", make_row(int64_t(101), int64_t(1), int64_t(50)));   // Alice, 50
    DBSPExtension::insert("orders", make_row(int64_t(102), int64_t(1), int64_t(150)));  // Alice, 150
    DBSPExtension::insert("orders", make_row(int64_t(103), int64_t(2), int64_t(200)));  // Bob, 200
    DBSPExtension::insert("orders", make_row(int64_t(104), int64_t(2), int64_t(75)));   // Bob, 75
    DBSPExtension::insert("orders", make_row(int64_t(105), int64_t(3), int64_t(300)));  // Charlie, 300

    // Query views
    auto* high_value = DBSPExtension::query_view("high_value_orders");
    if (high_value) {
        print_zset(*high_value, "High Value Orders (amount > 100)");
    }

    auto* totals = DBSPExtension::query_view("customer_totals");
    if (totals) {
        print_zset(*totals, "Customer Totals");
    }

    auto* joined = DBSPExtension::query_view("orders_with_customers");
    if (joined) {
        print_zset(*joined, "Orders with Customers");
    }

    std::cout << "\n--- Incremental update: Insert new order ---\n";

    // Insert a new order
    DBSPExtension::insert("orders", make_row(int64_t(106), int64_t(1), int64_t(250)));  // Alice, 250

    // Query deltas (incremental changes)
    auto* high_value_delta = DBSPExtension::query_view_delta("high_value_orders");
    if (high_value_delta) {
        print_zset(*high_value_delta, "High Value Orders Delta");
    }

    auto* totals_delta = DBSPExtension::query_view_delta("customer_totals");
    if (totals_delta) {
        print_zset(*totals_delta, "Customer Totals Delta");
    }

    // Full results after update
    if (high_value) {
        print_zset(*DBSPExtension::query_view("high_value_orders"), "High Value Orders (after update)");
    }

    if (totals) {
        print_zset(*DBSPExtension::query_view("customer_totals"), "Customer Totals (after update)");
    }

    std::cout << "\n--- Incremental update: Delete an order ---\n";

    // Delete an order
    DBSPExtension::remove("orders", make_row(int64_t(103), int64_t(2), int64_t(200)));  // Bob, 200

    totals_delta = DBSPExtension::query_view_delta("customer_totals");
    if (totals_delta) {
        print_zset(*totals_delta, "Customer Totals Delta (after delete)");
    }

    print_zset(*DBSPExtension::query_view("customer_totals"), "Customer Totals (after delete)");

    std::cout << "\n--- Incremental update: Update an order ---\n";

    // Update an order (change amount from 75 to 175)
    DBSPExtension::update(
        "orders",
        make_row(int64_t(104), int64_t(2), int64_t(75)),   // old
        make_row(int64_t(104), int64_t(2), int64_t(175))   // new
    );

    high_value_delta = DBSPExtension::query_view_delta("high_value_orders");
    if (high_value_delta) {
        print_zset(*high_value_delta, "High Value Orders Delta (after update)");
    }

    totals_delta = DBSPExtension::query_view_delta("customer_totals");
    if (totals_delta) {
        print_zset(*totals_delta, "Customer Totals Delta (after update)");
    }

    print_zset(*DBSPExtension::query_view("high_value_orders"), "High Value Orders (final)");
    print_zset(*DBSPExtension::query_view("customer_totals"), "Customer Totals (final)");

    std::cout << "\n=== Demo Complete ===\n";

    return 0;
}

#endif // DBSP_STANDALONE_TEST

-- DBSP for DuckDB: Cascading Views Example
-- This example demonstrates views that depend on other views.

-- =============================================================================
-- Setup: E-commerce scenario
-- =============================================================================

CREATE TABLE products (
    id INTEGER PRIMARY KEY,
    name VARCHAR,
    category VARCHAR,
    price DECIMAL(10,2)
);

CREATE TABLE orders (
    id INTEGER PRIMARY KEY,
    customer_id INTEGER,
    product_id INTEGER,
    quantity INTEGER,
    order_date DATE
);

CREATE TABLE customers (
    id INTEGER PRIMARY KEY,
    name VARCHAR,
    tier VARCHAR  -- 'standard', 'gold', 'platinum'
);

-- Populate initial data
INSERT INTO products VALUES
    (1, 'Laptop', 'Electronics', 999.99),
    (2, 'Mouse', 'Electronics', 29.99),
    (3, 'Desk', 'Furniture', 299.99),
    (4, 'Chair', 'Furniture', 199.99),
    (5, 'Monitor', 'Electronics', 399.99);

INSERT INTO customers VALUES
    (1, 'Alice', 'platinum'),
    (2, 'Bob', 'gold'),
    (3, 'Charlie', 'standard'),
    (4, 'Diana', 'gold');

INSERT INTO orders VALUES
    (1, 1, 1, 1, '2024-01-01'),
    (2, 1, 2, 2, '2024-01-01'),
    (3, 2, 3, 1, '2024-01-02'),
    (4, 2, 4, 2, '2024-01-02'),
    (5, 3, 2, 5, '2024-01-03'),
    (6, 4, 1, 1, '2024-01-03'),
    (7, 4, 5, 2, '2024-01-03');

-- Track all tables
SELECT * FROM dbsp_track('products');
SELECT * FROM dbsp_track('orders');
SELECT * FROM dbsp_track('customers');

-- =============================================================================
-- Layer 1: Base aggregation views (depend on tables)
-- =============================================================================

-- Total revenue per customer
SELECT * FROM dbsp_create_view('customer_revenue',
    'SELECT customer_id, SUM(quantity) as total_items FROM orders GROUP BY customer_id');

-- Orders by category (would need join - simplified here)
SELECT * FROM dbsp_create_view('order_summary',
    'SELECT customer_id, COUNT(*) as order_count FROM orders GROUP BY customer_id');

-- =============================================================================
-- Layer 2: Filtered views (depend on Layer 1 views)
-- =============================================================================

-- High-volume customers (more than 2 items)
SELECT * FROM dbsp_create_view('high_volume_customers',
    'SELECT * FROM customer_revenue WHERE total_items > 2');

-- Frequent buyers (more than 1 order)
SELECT * FROM dbsp_create_view('frequent_buyers',
    'SELECT * FROM order_summary WHERE order_count > 1');

-- =============================================================================
-- Layer 3: Further aggregation (depends on Layer 2)
-- =============================================================================

-- Count of high-volume customers
SELECT * FROM dbsp_create_view('high_volume_count',
    'SELECT COUNT(*) FROM high_volume_customers');

-- =============================================================================
-- Examine the dependency graph
-- =============================================================================

-- See what customer_revenue depends on
SELECT * FROM dbsp_deps('customer_revenue');
-- Result: orders (depends_on)
--         high_volume_customers (depended_by)

-- See the full chain for high_volume_count
SELECT * FROM dbsp_deps('high_volume_count');
-- Result: high_volume_customers (depends_on)

SELECT * FROM dbsp_deps('high_volume_customers');
-- Result: customer_revenue (depends_on)
--         high_volume_count (depended_by)

-- =============================================================================
-- Test cascading updates
-- =============================================================================

-- Query initial state
SELECT 'Initial high_volume_customers:' as label;
SELECT * FROM dbsp_query('high_volume_customers');

SELECT 'Initial high_volume_count:' as label;
SELECT * FROM dbsp_query('high_volume_count');

-- Add a large order for Charlie (currently has 5 items from 1 order)
INSERT INTO orders VALUES (8, 3, 1, 10, '2024-01-04');

-- Sync - changes cascade through all layers
SELECT * FROM dbsp_sync('orders');

-- Charlie now has 15 items - should appear in high_volume_customers
SELECT 'After adding large order:' as label;
SELECT * FROM dbsp_query('customer_revenue');
SELECT * FROM dbsp_query('high_volume_customers');
SELECT * FROM dbsp_query('high_volume_count');

-- =============================================================================
-- Demonstrate cascade drop protection
-- =============================================================================

-- Try to drop customer_revenue (has dependents)
SELECT dbsp_drop('customer_revenue');
-- Result: Error - high_volume_customers depends on it

-- Use cascade drop instead
SELECT dbsp_drop_cascade('customer_revenue');
-- Result: Drops customer_revenue, high_volume_customers, and high_volume_count

-- Verify they're gone
SELECT * FROM dbsp_views();

-- =============================================================================
-- Cleanup
-- =============================================================================

SELECT dbsp_drop('order_summary');
SELECT dbsp_drop('frequent_buyers');

DROP TABLE orders;
DROP TABLE products;
DROP TABLE customers;

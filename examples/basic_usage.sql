-- DBSP for DuckDB: Basic Usage Example
-- This example demonstrates the core features of incremental materialized views.

-- =============================================================================
-- Setup: Create and populate a table
-- =============================================================================

CREATE TABLE orders (
    id INTEGER,
    customer VARCHAR,
    product VARCHAR,
    amount DECIMAL(10,2),
    created_at TIMESTAMP
);

INSERT INTO orders VALUES
    (1, 'Alice', 'Widget', 99.99, '2024-01-01 10:00:00'),
    (2, 'Bob', 'Gadget', 149.99, '2024-01-01 11:00:00'),
    (3, 'Alice', 'Widget', 99.99, '2024-01-01 12:00:00'),
    (4, 'Charlie', 'Gizmo', 199.99, '2024-01-01 13:00:00'),
    (5, 'Bob', 'Widget', 99.99, '2024-01-01 14:00:00');

-- =============================================================================
-- Step 1: Track the table for change detection
-- =============================================================================

SELECT * FROM dbsp_track('orders');
-- Result: "Tracking table: orders (5 columns)"

-- =============================================================================
-- Step 2: Create incrementally maintained views
-- =============================================================================

-- Filter view: High-value orders
SELECT * FROM dbsp_create_view('high_value_orders',
    'SELECT * FROM orders WHERE amount > 100');

-- Aggregation view: Total by customer
SELECT * FROM dbsp_create_view('customer_totals',
    'SELECT customer, SUM(amount) FROM orders GROUP BY customer');

-- Aggregation view: Total by product
SELECT * FROM dbsp_create_view('product_totals',
    'SELECT product, SUM(amount), COUNT(*) FROM orders GROUP BY product');

-- Distinct view: Unique customers
SELECT * FROM dbsp_create_view('unique_customers',
    'SELECT DISTINCT customer FROM orders');

-- =============================================================================
-- Step 3: Query the views
-- =============================================================================

-- List all views
SELECT * FROM dbsp_views();

-- Query high-value orders
SELECT * FROM dbsp_query('high_value_orders');
-- Expected: Orders with amount > 100 (Gadget: 149.99, Gizmo: 199.99)

-- Query customer totals
SELECT * FROM dbsp_query('customer_totals');
-- Expected:
--   Alice: 199.98
--   Bob: 249.98
--   Charlie: 199.99

-- Query product totals
SELECT * FROM dbsp_query('product_totals');
-- Expected:
--   Widget: 299.97, 3
--   Gadget: 149.99, 1
--   Gizmo: 199.99, 1

-- =============================================================================
-- Step 4: Insert new data and see incremental updates
-- =============================================================================

-- Insert a new high-value order
INSERT INTO orders VALUES
    (6, 'Alice', 'SuperGadget', 299.99, '2024-01-02 10:00:00');

-- Sync to detect changes
SELECT * FROM dbsp_sync('orders');

-- Query again - views are already updated!
SELECT * FROM dbsp_query('high_value_orders');
-- Now includes: Alice's SuperGadget order

SELECT * FROM dbsp_query('customer_totals');
-- Alice's total is now: 499.97

-- =============================================================================
-- Step 5: Delete data and see updates
-- =============================================================================

-- Delete an order
DELETE FROM orders WHERE id = 4;  -- Remove Charlie's order

-- Sync changes
SELECT * FROM dbsp_sync('orders');

-- Charlie should have a lower total (or be gone)
SELECT * FROM dbsp_query('customer_totals');

-- =============================================================================
-- Step 6: Batch updates
-- =============================================================================

-- Insert multiple rows
INSERT INTO orders VALUES
    (7, 'Diana', 'Widget', 99.99, '2024-01-03 10:00:00'),
    (8, 'Diana', 'Gadget', 149.99, '2024-01-03 11:00:00'),
    (9, 'Eve', 'Gizmo', 199.99, '2024-01-03 12:00:00');

-- Single sync handles all changes
SELECT * FROM dbsp_sync('orders');

-- All views updated
SELECT * FROM dbsp_query('customer_totals');
SELECT * FROM dbsp_query('unique_customers');

-- =============================================================================
-- Cleanup
-- =============================================================================

-- Drop views
SELECT dbsp_drop('high_value_orders');
SELECT dbsp_drop('customer_totals');
SELECT dbsp_drop('product_totals');
SELECT dbsp_drop('unique_customers');

-- Drop table
DROP TABLE orders;

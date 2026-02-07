-- Example 04: Complex JOIN Predicates - Multi-Column and Non-Equi Joins
-- This demonstrates advanced JOIN capabilities beyond simple equality

-- Setup
LOAD 'dbsp';

CREATE TABLE orders (
    order_id INT,
    customer_id INT,
    region VARCHAR,
    amount DECIMAL,
    order_date DATE
);

CREATE TABLE customers (
    customer_id INT,
    name VARCHAR,
    region VARCHAR,
    vip_status BOOLEAN
);

SELECT * FROM dbsp_track('orders');
SELECT * FROM dbsp_track('customers');

-- Example 1: Multi-Column Equality JOIN
-- Match orders to customers by BOTH customer_id AND region
CREATE MATERIALIZED VIEW regional_orders AS
SELECT 
    c.name,
    c.region,
    o.order_id,
    o.amount
FROM orders o
JOIN customers c ON o.customer_id = c.customer_id AND o.region = c.region;

-- Insert data
INSERT INTO customers VALUES
    (1, 'Alice', 'North', true),
    (2, 'Bob', 'South', false),
    (3, 'Charlie', 'North', true);

INSERT INTO orders VALUES
    (101, 1, 'North', 100, '2024-01-01'),  -- Matches Alice (same region)
    (102, 1, 'South', 50, '2024-01-02'),   -- No match (different region)
    (103, 2, 'South', 200, '2024-01-03'),  -- Matches Bob
    (104, 3, 'North', 150, '2024-01-04');  -- Matches Charlie

SELECT * FROM dbsp_sync('orders');
SELECT * FROM dbsp_sync('customers');

-- Query: Only 3 matches (order 102 excluded - wrong region)
SELECT * FROM regional_orders ORDER BY order_id;
-- Alice, North, 101, 100
-- Bob, South, 103, 200
-- Charlie, North, 104, 150

-- Example 2: Non-Equi JOIN (Range Join)
CREATE TABLE price_tiers (
    tier_name VARCHAR,
    min_amount DECIMAL,
    max_amount DECIMAL,
    discount_pct DECIMAL
);

SELECT * FROM dbsp_track('price_tiers');

-- Classify orders into price tiers based on amount range
CREATE MATERIALIZED VIEW order_tiers AS
SELECT 
    o.order_id,
    o.amount,
    t.tier_name,
    t.discount_pct
FROM orders o
JOIN price_tiers t ON o.amount >= t.min_amount AND o.amount < t.max_amount;

-- Insert price tier definitions
INSERT INTO price_tiers VALUES
    ('Bronze', 0, 100, 0.05),
    ('Silver', 100, 200, 0.10),
    ('Gold', 200, 1000000, 0.15);

SELECT * FROM dbsp_sync('price_tiers');

-- Query: Orders classified by tier
SELECT * FROM order_tiers ORDER BY order_id;
-- 101: Silver (100), 103: Gold (200), 104: Silver (150)

-- Example 3: Complex predicates with VIP filtering
CREATE MATERIALIZED VIEW vip_large_orders AS
SELECT 
    c.name,
    o.order_id,
    o.amount,
    c.vip_status
FROM orders o
JOIN customers c ON 
    o.customer_id = c.customer_id 
    AND o.region = c.region
    AND c.vip_status = true
    AND o.amount > 50;

SELECT * FROM dbsp_sync('orders');
SELECT * FROM dbsp_sync('customers');

-- Query: Only VIP customers with orders > 50 in matching regions
SELECT * FROM vip_large_orders ORDER BY order_id;
-- Alice: 101 (100), Charlie: 104 (150)

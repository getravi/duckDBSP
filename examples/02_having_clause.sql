-- Example 02: HAVING Clause - Filtering Aggregated Results
-- This demonstrates incremental aggregate filtering using the HAVING clause

-- Setup
LOAD 'dbsp';

CREATE TABLE sales (
    product VARCHAR,
    region VARCHAR,
    amount DECIMAL,
    quantity INT
);

SELECT * FROM dbsp_track('sales');

-- Create a view that only shows products with total sales > 1000
CREATE MATERIALIZED VIEW high_selling_products AS
SELECT 
    product,
    SUM(amount) as total_revenue,
    SUM(quantity) as total_quantity,
    COUNT(*) as num_sales
FROM sales
GROUP BY product
HAVING SUM(amount) > 1000;

-- Insert initial data
INSERT INTO sales VALUES
    ('Widget', 'North', 500, 10),
    ('Widget', 'South', 600, 12),
    ('Gadget', 'North', 300, 5),
    ('Gadget', 'South', 200, 4),
    ('Doohickey', 'East', 1500, 30);

SELECT * FROM dbsp_sync('sales');

-- Query: Only Widget (1100) and Doohickey (1500) appear
SELECT * FROM high_selling_products ORDER BY product;
-- Widget: 1100, 30 sales
-- Doohickey: 1500, 30 sales

-- Add more sales for Gadget to push it over the threshold
INSERT INTO sales VALUES
    ('Gadget', 'East', 600, 15);

SELECT * FROM dbsp_sync('sales');

-- Query: Now Gadget appears (total: 1100)
SELECT * FROM high_selling_products ORDER BY product;
-- Doohickey: 1500, 30 sales
-- Gadget: 1100, 15 sales  -- NEW!
-- Widget: 1100, 30 sales

-- Complex HAVING with multiple conditions
CREATE MATERIALIZED VIEW premium_products AS
SELECT 
    product,
    AVG(amount) as avg_sale,
    COUNT(*) as sale_count
FROM sales
GROUP BY product
HAVING AVG(amount) > 400 AND COUNT(*) >= 2;

SELECT * FROM dbsp_sync('sales');

-- Query: Only products with avg > 400 AND at least 2 sales
SELECT * FROM premium_products ORDER BY product;

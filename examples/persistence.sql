-- DBSP for DuckDB: Persistence Example
-- This example demonstrates saving and loading view definitions.

-- =============================================================================
-- Setup: Create tables and views
-- =============================================================================

CREATE TABLE sales (
    id INTEGER,
    region VARCHAR,
    product VARCHAR,
    amount DECIMAL(10,2),
    sale_date DATE
);

INSERT INTO sales VALUES
    (1, 'North', 'Widget', 100.00, '2024-01-01'),
    (2, 'South', 'Gadget', 200.00, '2024-01-01'),
    (3, 'North', 'Widget', 150.00, '2024-01-02'),
    (4, 'East', 'Gizmo', 300.00, '2024-01-02'),
    (5, 'South', 'Widget', 100.00, '2024-01-03');

-- Track table
SELECT * FROM dbsp_track('sales');

-- Create views
SELECT * FROM dbsp_create_view('regional_totals',
    'SELECT region, SUM(amount) FROM sales GROUP BY region');

SELECT * FROM dbsp_create_view('product_totals',
    'SELECT product, SUM(amount) FROM sales GROUP BY product');

-- Create cascading view
SELECT * FROM dbsp_create_view('top_regions',
    'SELECT * FROM regional_totals WHERE SUM > 200');

-- Verify views are working
SELECT 'Current views:' as label;
SELECT * FROM dbsp_views();

SELECT 'Regional totals:' as label;
SELECT * FROM dbsp_query('regional_totals');

-- =============================================================================
-- Option 1: Save to DuckDB table
-- =============================================================================

-- Save all view definitions to internal table
SELECT * FROM dbsp_save();
-- Result: "Saved to DuckDB table: _dbsp_views"

-- You can inspect the saved data
SELECT * FROM _dbsp_views;
-- Shows: name, sql, sources, created_at

SELECT * FROM _dbsp_views_tables;
-- Shows tracked tables

-- =============================================================================
-- Option 2: Save to JSON file
-- =============================================================================

-- Save to a file (useful for version control, sharing)
SELECT * FROM dbsp_save('/tmp/dbsp_views.json');
-- Result: "Saved to file: /tmp/dbsp_views.json"

-- The JSON file contains:
-- {
--   "tracked_tables": ["sales"],
--   "views": [
--     {"name": "regional_totals", "sql": "SELECT ...", ...},
--     {"name": "product_totals", "sql": "SELECT ...", ...},
--     {"name": "top_regions", "sql": "SELECT ...", ...}
--   ]
-- }

-- =============================================================================
-- Simulate session restart
-- =============================================================================

-- Drop all views (simulating a fresh session)
SELECT dbsp_drop_cascade('regional_totals');
SELECT dbsp_drop('product_totals');

-- Verify views are gone
SELECT 'Views after drop:' as label;
SELECT * FROM dbsp_views();

-- =============================================================================
-- Restore from DuckDB table
-- =============================================================================

SELECT * FROM dbsp_load();
-- Result: "Loaded from DuckDB table (3 views)"

-- Views are restored and populated with current data
SELECT 'Restored views:' as label;
SELECT * FROM dbsp_views();

SELECT 'Restored regional_totals:' as label;
SELECT * FROM dbsp_query('regional_totals');

-- The cascading view is also restored
SELECT 'Restored top_regions:' as label;
SELECT * FROM dbsp_query('top_regions');

-- =============================================================================
-- Restore from JSON file
-- =============================================================================

-- Drop views again
SELECT dbsp_drop_cascade('regional_totals');
SELECT dbsp_drop('product_totals');

-- Load from file
SELECT * FROM dbsp_load('/tmp/dbsp_views.json');
-- Result: "Loaded from file: /tmp/dbsp_views.json (3 views)"

-- Views restored
SELECT * FROM dbsp_views();

-- =============================================================================
-- Best practices for persistence
-- =============================================================================

-- 1. Save after creating/modifying views
SELECT * FROM dbsp_create_view('new_view', 'SELECT DISTINCT region FROM sales');
SELECT * FROM dbsp_save();  -- Persist immediately

-- 2. For production, save to both table and file for redundancy
SELECT * FROM dbsp_save();                           -- Quick restore
SELECT * FROM dbsp_save('/backups/dbsp_views.json'); -- Portable backup

-- 3. Load at application startup
-- In your application init:
--   SELECT * FROM dbsp_load();

-- 4. Include JSON files in version control
-- views/production.json
-- views/staging.json

-- =============================================================================
-- Cleanup
-- =============================================================================

SELECT dbsp_drop_cascade('regional_totals');
SELECT dbsp_drop('product_totals');
SELECT dbsp_drop('new_view');

DROP TABLE sales;
DROP TABLE IF EXISTS _dbsp_views;
DROP TABLE IF EXISTS _dbsp_views_tables;

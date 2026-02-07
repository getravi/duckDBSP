-- Example 03: Recursive Queries - Graph Traversal and Transitive Closure
-- This demonstrates WITH RECURSIVE for computing reachability in graphs

-- Setup
LOAD 'dbsp';

CREATE TABLE graph_edges (src INT, dst INT, weight DECIMAL);
SELECT * FROM dbsp_track('graph_edges');

-- Example 1: Transitive Closure (Reachability)
CREATE MATERIALIZED VIEW reachable AS
WITH RECURSIVE reach AS (
    -- Base case: direct edges
    SELECT src, dst, 1 as hops FROM graph_edges
    UNION
    -- Recursive case: paths through intermediate nodes
    SELECT e.src, r.dst, r.hops + 1
    FROM graph_edges e
    JOIN reach r ON e.dst = r.src
    WHERE r.hops < 10  -- Safety: limit path length
)
SELECT DISTINCT src, dst, MIN(hops) as shortest_hops
FROM reach
GROUP BY src, dst;

-- Insert a simple graph: 1->2->3->4
INSERT INTO graph_edges VALUES
    (1, 2, 1.0),
    (2, 3, 1.0),
    (3, 4, 1.0);

SELECT * FROM dbsp_sync('graph_edges');

-- Query: See all reachable pairs
SELECT * FROM reachable ORDER BY src, dst;
-- (1,2,1), (1,3,2), (1,4,3)
-- (2,3,1), (2,4,2)
-- (3,4,1)

-- Add a new edge: 1->4 (shortcut)
INSERT INTO graph_edges VALUES (1, 4, 0.5);
SELECT * FROM dbsp_sync('graph_edges');

-- Query: Shortest hops from 1 to 4 is now 1 (direct)
SELECT * FROM reachable WHERE src = 1 AND dst = 4;
-- (1, 4, 1)

-- Example 2: Organizational Hierarchy
CREATE TABLE employees (
    id INT,
    name VARCHAR,
    manager_id INT
);
SELECT * FROM dbsp_track('employees');

CREATE MATERIALIZED VIEW org_tree AS
WITH RECURSIVE hierarchy AS (
    -- Base case: top-level employees (no manager)
    SELECT id, name, manager_id, 0 as level, name as path
    FROM employees
    WHERE manager_id IS NULL
    
    UNION ALL
    
    -- Recursive case: employees under each manager
    SELECT e.id, e.name, e.manager_id, h.level + 1,
           h.path || ' -> ' || e.name
    FROM employees e
    JOIN hierarchy h ON e.manager_id = h.id
)
SELECT * FROM hierarchy;

-- Insert org structure
INSERT INTO employees VALUES
    (1, 'CEO', NULL),
    (2, 'VP Eng', 1),
    (3, 'VP Sales', 1),
    (4, 'Eng Manager', 2),
    (5, 'Sales Manager', 3),
    (6, 'Engineer', 4),
    (7, 'Salesperson', 5);

SELECT * FROM dbsp_sync('employees');

-- Query: See full org tree with levels
SELECT level, name, path FROM org_tree ORDER BY level, name;

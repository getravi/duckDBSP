-- Example 05: MIN/MAX Aggregates with Incremental Updates
-- This demonstrates O(log n) incremental maintenance for MIN and MAX

-- Setup
LOAD 'dbsp';

CREATE TABLE sensor_readings (
    sensor_id INT,
    timestamp TIMESTAMP,
    temperature DECIMAL,
    humidity DECIMAL
);

SELECT * FROM dbsp_track('sensor_readings');

-- Track min/max temperatures per sensor
CREATE MATERIALIZED VIEW sensor_extremes AS
SELECT 
    sensor_id,
    MIN(temperature) as min_temp,
    MAX(temperature) as max_temp,
    AVG(temperature) as avg_temp,
    COUNT(*) as reading_count
FROM sensor_readings
GROUP BY sensor_id;

-- Insert initial readings
INSERT INTO sensor_readings VALUES
    (1, '2024-01-01 08:00:00', 20.5, 45.0),
    (1, '2024-01-01 09:00:00', 22.0, 46.5),
    (1, '2024-01-01 10:00:00', 23.5, 48.0),
    (2, '2024-01-01 08:00:00', 18.0, 50.0),
    (2, '2024-01-01 09:00:00', 19.5, 52.0);

SELECT * FROM dbsp_sync('sensor_readings');

-- Query: Initial min/max
SELECT * FROM sensor_extremes ORDER BY sensor_id;
-- Sensor 1: min=20.5, max=23.5, avg=22.0
-- Sensor 2: min=18.0, max=19.5, avg=18.75

-- Add a new extreme reading for sensor 1
INSERT INTO sensor_readings VALUES
    (1, '2024-01-01 11:00:00', 25.0, 47.0);

SELECT * FROM dbsp_sync('sensor_readings');

-- Query: MAX updated incrementally (O(log n), not O(n)!)
SELECT * FROM sensor_extremes WHERE sensor_id = 1;
-- Sensor 1: min=20.5, max=25.0, avg=22.75

-- Delete the minimum reading
DELETE FROM sensor_readings 
WHERE sensor_id = 1 AND temperature = 20.5;

SELECT * FROM dbsp_sync('sensor_readings');

-- Query: MIN recalculated using sorted multiset (O(log n))
SELECT * FROM sensor_extremes WHERE sensor_id = 1;
-- Sensor 1: min=22.0, max=25.0

-- Example 2: Alert when range exceeds threshold
CREATE MATERIALIZED VIEW temperature_alerts AS
SELECT 
    sensor_id,
    MIN(temperature) as min_temp,
    MAX(temperature) as max_temp,
    MAX(temperature) - MIN(temperature) as range
FROM sensor_readings
GROUP BY sensor_id
HAVING MAX(temperature) - MIN(temperature) > 5.0;

SELECT * FROM dbsp_sync('sensor_readings');

-- Query: Sensors with > 5 degree range
SELECT * FROM temperature_alerts ORDER BY sensor_id;

-- Add extreme reading to trigger alert
INSERT INTO sensor_readings VALUES
    (2, '2024-01-01 10:00:00', 25.0, 55.0);

SELECT * FROM dbsp_sync('sensor_readings');

-- Query: Sensor 2 now shows alert (range: 25.0 - 18.0 = 7.0)
SELECT * FROM temperature_alerts ORDER BY sensor_id;

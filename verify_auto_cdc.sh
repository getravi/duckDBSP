#!/bin/bash
set -e

# Build the extension if needed (assuming it is already built)
EXTENSION_PATH="build/dbsp.duckdb_extension"

if [ ! -f "$EXTENSION_PATH" ]; then
    echo "Error: Extension binary not found at $EXTENSION_PATH"
    echo "Please wait for build.sh to complete."
    exit 1
fi

# Determine DuckDB binary (same logic as verify_extension.sh)
if [ -f "build/duckdb/duckdb" ] && [ ! -d "build/duckdb/duckdb" ]; then
    DUCKDB_BIN="./build/duckdb/duckdb"
elif [ -f "build/duckdb/src/shell/duckdb" ]; then
   DUCKDB_BIN="./build/duckdb/src/shell/duckdb"
else
    DUCKDB_BIN="duckdb"
fi

echo "Using DuckDB shell: $DUCKDB_BIN"

# Run DuckDB with the verification script
$DUCKDB_BIN -unsigned <<EOF
.bail on
.echo on

-- Load extension
LOAD 'build/dbsp.duckdb_extension';

-- Create table and view
CREATE TABLE t1 (id INT, val INT);
SELECT * FROM dbsp_track('t1');
SELECT * FROM dbsp_create_view('v1', 'SELECT * FROM t1 WHERE val > 10');

-- Enable Auto Sync
SELECT * FROM dbsp_auto_sync(true);

-- Insert data (should trigger auto-sync on commit)
BEGIN TRANSACTION;
INSERT INTO t1 VALUES (1, 5), (2, 15), (3, 20);
COMMIT;

-- Query view (NO MANUAL SYNC)
SELECT * FROM dbsp_query('v1');

-- Cleanup
SELECT dbsp_drop('v1');
EOF

#!/bin/bash
# Verification script for DBSP DuckDB Extension

EXTENSION_PATH="build/dbsp.duckdb_extension"

if [ ! -f "$EXTENSION_PATH" ]; then
    echo "Error: Extension binary not found at $EXTENSION_PATH"
    echo "Please wait for build.sh to complete."
    exit 1
fi

echo "Verifying extension load..."
duckdb -c "LOAD '$EXTENSION_PATH'; SELECT 'Extension loaded successfully!';" 

echo "Running basic functional test..."
duckdb <<EOF
LOAD '$EXTENSION_PATH';
CREATE TABLE t1 (id INT, val INT);
SELECT dbsp_track('t1');
SELECT dbsp_create_view('v1', 'SELECT * FROM t1 WHERE val > 10');
INSERT INTO t1 VALUES (1, 5), (2, 15), (3, 20);
SELECT dbsp_sync();
SELECT * FROM dbsp_query('v1');
EOF

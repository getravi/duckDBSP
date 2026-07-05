#!/bin/bash
# Build script for DBSP DuckDB Extension
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
DUCKDB_VERSION="v1.5.4"

echo "=== DBSP DuckDB Extension Build ==="
echo ""

# Check if DuckDB source exists
if [ ! -d "$SCRIPT_DIR/duckdb" ]; then
    echo "Fetching DuckDB ${DUCKDB_VERSION}..."
    git clone --depth 1 --branch ${DUCKDB_VERSION} https://github.com/duckdb/duckdb.git "$SCRIPT_DIR/duckdb"
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure
echo "Configuring..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DDUCKDB_SOURCE_DIR="$SCRIPT_DIR/duckdb"

# Build
echo "Building..."
if [[ "$OSTYPE" == "darwin"* ]]; then
    make -j$(sysctl -n hw.ncpu)
else
    make -j$(nproc)
fi

echo ""
echo "=== Build Complete ==="
echo "Extension: $BUILD_DIR/dbsp.duckdb_extension"
echo ""
echo "Usage:"
echo "  duckdb -cmd \"LOAD '$BUILD_DIR/dbsp.duckdb_extension'\""

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

# Apply the engine patches (the patch files ARE the fork — stock DuckDB lacks
# the txn-callback symbols the extension needs). Idempotent: skip patches the
# tree already carries, fail loudly if one neither applies nor reverse-applies.
for patch in "$SCRIPT_DIR"/patches/*.patch; do
    if git -C "$SCRIPT_DIR/duckdb" apply --check --reverse "$patch" 2>/dev/null; then
        echo "Patch already applied: $(basename "$patch")"
    elif git -C "$SCRIPT_DIR/duckdb" apply --check "$patch" 2>/dev/null; then
        echo "Applying patch: $(basename "$patch")"
        git -C "$SCRIPT_DIR/duckdb" apply "$patch"
    else
        echo "ERROR: $(basename "$patch") neither applies cleanly nor is already applied." >&2
        echo "The duckdb/ tree has drifted from the patch — reconcile before building." >&2
        exit 1
    fi
done

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Optional accelerators: use ccache/ninja when available (CI installs them;
# harmless to omit locally). Generator choice only affects a fresh build dir —
# an existing CMakeCache.txt keeps whatever generator configured it.
CMAKE_EXTRA_ARGS=()
if command -v ccache >/dev/null 2>&1; then
    CMAKE_EXTRA_ARGS+=("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache" "-DCMAKE_C_COMPILER_LAUNCHER=ccache")
fi
if command -v ninja >/dev/null 2>&1 && [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    CMAKE_EXTRA_ARGS+=("-GNinja")
fi

# Configure
echo "Configuring..."
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DDUCKDB_SOURCE_DIR="$SCRIPT_DIR/duckdb" \
    "${CMAKE_EXTRA_ARGS[@]}"

# Build (parallelism capped at 8 — higher has frozen this machine before)
if [[ "$OSTYPE" == "darwin"* ]]; then
    NCPU=$(sysctl -n hw.ncpu)
else
    NCPU=$(nproc)
fi
JOBS=$(( NCPU < 8 ? NCPU : 8 ))
echo "Building (-j${JOBS})..."
cmake --build . -j"${JOBS}"

echo ""
echo "=== Build Complete ==="
echo "Extension: $BUILD_DIR/dbsp.duckdb_extension"
echo ""
echo "Usage:"
echo "  duckdb -cmd \"LOAD '$BUILD_DIR/dbsp.duckdb_extension'\""

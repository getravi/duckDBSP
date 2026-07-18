#!/usr/bin/env bash
# Build the patched (dbsp engine-hook) DuckDB Python wheel locally.
#
# The Python client lives in the separate duckdb/duckdb-python repo (the
# engine tree has no tools/pythonpkg since the 1.2/1.3 split). Its
# external/duckdb submodule pins the same engine commit as ours (v1.5.4 =
# 08e34c4), so patches/v1.5.4-dbsp-txn-callback.patch applies 1:1.
#
# ABI pairing: the patch adds a member to DBConfig, so the wheel and the
# dbsp.duckdb_extension binary must be built as a pair — a stock engine
# must never load a hook-built extension, and vice versa.
#
# Usage: scripts/build_engine_wheel.sh [duckdb-python-checkout]
#   default checkout: ../duckdb-python (cloned at tag v1.5.4)
set -euo pipefail

DBSP_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PYPKG="${1:-$DBSP_ROOT/../duckdb-python}"
PATCH="$DBSP_ROOT/patches/v1.5.4-dbsp-txn-callback.patch"

if [ ! -d "$PYPKG" ]; then
  echo "cloning duckdb-python v1.5.4 -> $PYPKG"
  git clone --branch v1.5.4 --depth 1 https://github.com/duckdb/duckdb-python.git "$PYPKG"
fi
cd "$PYPKG"
git submodule update --init --depth 1 external/duckdb

# apply the engine patch (idempotent: skip if already applied)
if git -C external/duckdb apply --check "$PATCH" 2>/dev/null; then
  git -C external/duckdb apply "$PATCH"
  echo "engine patch applied"
else
  git -C external/duckdb apply --reverse --check "$PATCH" 2>/dev/null \
    && echo "engine patch already applied" \
    || { echo "ERROR: patch neither applies nor is applied — engine tree diverged"; exit 1; }
fi

# Fork version marker: the client's custom backend (duckdb_packaging)
# IGNORES SETUPTOOLS_SCM_PRETEND_VERSION* and supports only
# OVERRIDE_GIT_DESCRIBE in vX.Y.Z[-postN] form — local segments (+dbsp)
# are impossible, so the fork ships as 1.5.4.post1 (> 1.5.4, distinct
# from stock). Cap build parallelism: ninja defaults to all cores;
# unbounded clang on a 16GB machine swap-storms.
export OVERRIDE_GIT_DESCRIBE="v1.5.4-post1"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
uv build --wheel

echo
ls -la dist/*.whl
echo "smoke: uv pip install dist/*.whl && python -c \"import duckdb; assert duckdb.__version__ == '1.5.4.post1'\""

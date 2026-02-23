#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPS_DIR="$PROJECT_DIR/deps"
CPYTHON_DIR="$DEPS_DIR/cpython"

HOST_TRIPLE="wasm32-wasip2"
PYTHON_WASM="$CPYTHON_DIR/cross-build/$HOST_TRIPLE/python.wasm"

if [ ! -f "$PYTHON_WASM" ]; then
    echo "ERROR: python.wasm not found at $PYTHON_WASM" >&2
    echo "Run 'make' first to build." >&2
    exit 1
fi

# Sysconfig data lives under the cross-build directory.
# When we mount the CPython checkout as /, this becomes accessible at
# /cross-build/<triple>/build/lib.wasi-wasm32-<version>/
SYSCONFIG_DIR="/cross-build/$HOST_TRIPLE/build/lib.wasi-wasm32-3.14"

exec wasmtime run \
    --wasm max-wasm-stack=16777216 \
    --dir "$CPYTHON_DIR::/" \
    --env "PYTHONPATH=$SYSCONFIG_DIR" \
    "$PYTHON_WASM" "$@"

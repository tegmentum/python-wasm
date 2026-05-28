#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PROFILE="${PROFILE:-default}"
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"
CPYTHON_DIR="$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR"

PYTHON_WASM="$CPYTHON_DIR/cross-build/$HOST_TRIPLE/python.wasm"

if [ ! -f "$PYTHON_WASM" ]; then
    echo "ERROR: python.wasm not found at $PYTHON_WASM" >&2
    echo "Run 'make' first to build." >&2
    exit 1
fi

# Sysconfig data lives under the cross-build directory.
# When we mount the CPython checkout as /, this becomes accessible at
# /cross-build/<triple>/build/lib.wasi-wasm32-<py_minor>/ — derive py_minor
# from PYTHON_VERSION (e.g., "3.14.3" -> "3.14").
PY_MINOR="$(printf '%s\n' "$PYTHON_VERSION" | cut -d. -f1-2)"
SYSCONFIG_DIR="/cross-build/$HOST_TRIPLE/build/lib.wasi-wasm32-$PY_MINOR"

exec wasmtime run \
    --wasm max-wasm-stack=16777216 \
    --dir "$CPYTHON_DIR::/" \
    --env "PYTHONPATH=$SYSCONFIG_DIR" \
    "$PYTHON_WASM" "$@"

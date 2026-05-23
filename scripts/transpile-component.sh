#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CPYTHON_DIR="$PROJECT_DIR/deps/cpython"
HOST_TRIPLE="wasm32-wasip2"
PYTHON_WASM="$CPYTHON_DIR/cross-build/$HOST_TRIPLE/python.wasm"
OUTPUT_DIR="$PROJECT_DIR/web/public/python-component"

if [ ! -f "$PYTHON_WASM" ]; then
    echo "ERROR: python.wasm not found at $PYTHON_WASM" >&2
    echo "Run 'make build' first." >&2
    exit 1
fi

# Clean first so a changed module count (e.g. across WASI SDK versions) doesn't
# leave orphaned core wasm modules behind.
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo "Transpiling python.wasm for browser use..."

npx --prefix "$PROJECT_DIR/web" jco transpile "$PYTHON_WASM" \
    -o "$OUTPUT_DIR" \
    --no-nodejs-compat \
    --instantiation async \
    --name python

echo "Transpiled to $OUTPUT_DIR"
ls -lh "$OUTPUT_DIR"

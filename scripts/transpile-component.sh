#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CPYTHON_DIR="$PROJECT_DIR/deps/cpython"
HOST_TRIPLE="wasm32-wasip2"
# Prefer the composed component (Phase 1/2/...). Fall back to the raw
# wasi-sdk build only if no capability extensions are wired in.
RAW_PYTHON_WASM="$CPYTHON_DIR/cross-build/$HOST_TRIPLE/python.wasm"
COMPOSED_PYTHON_WASM="$PROJECT_DIR/build/python.composed.wasm"
if [ -f "$COMPOSED_PYTHON_WASM" ]; then
    PYTHON_WASM="$COMPOSED_PYTHON_WASM"
    echo "Transpiling COMPOSED python component (capabilities composed in)..."
elif [ -f "$RAW_PYTHON_WASM" ]; then
    PYTHON_WASM="$RAW_PYTHON_WASM"
    echo "Transpiling RAW python.wasm (no capabilities composed in yet)..."
else
    echo "ERROR: python.wasm not found. Run 'make build && make python-composed' first." >&2
    exit 1
fi
OUTPUT_DIR="$PROJECT_DIR/web/public/python-component"

# Clean first so a changed module count (e.g. across WASI SDK versions) doesn't
# leave orphaned core wasm modules behind.
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

npx --prefix "$PROJECT_DIR/web" jco transpile "$PYTHON_WASM" \
    -o "$OUTPUT_DIR" \
    --no-nodejs-compat \
    --instantiation async \
    --name python

echo "Transpiled to $OUTPUT_DIR"
ls -lh "$OUTPUT_DIR"

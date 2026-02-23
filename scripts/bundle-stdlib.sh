#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CPYTHON_DIR="$PROJECT_DIR/deps/cpython"
OUTPUT="$PROJECT_DIR/web/public/stdlib.tar.gz"

if [ ! -d "$CPYTHON_DIR/Lib" ]; then
    echo "ERROR: CPython source not found at $CPYTHON_DIR" >&2
    echo "Run 'make build' first." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT")"

echo "Bundling CPython stdlib..."

cd "$CPYTHON_DIR"

tar czf "$OUTPUT" \
    --exclude='Lib/test' \
    --exclude='Lib/test/*' \
    --exclude='Lib/idlelib' \
    --exclude='Lib/idlelib/*' \
    --exclude='Lib/tkinter' \
    --exclude='Lib/tkinter/*' \
    --exclude='Lib/turtledemo' \
    --exclude='Lib/turtledemo/*' \
    --exclude='Lib/ensurepip' \
    --exclude='Lib/ensurepip/*' \
    --exclude='Lib/lib2to3' \
    --exclude='Lib/lib2to3/*' \
    --exclude='__pycache__' \
    --exclude='*.pyc' \
    Lib/ \
    cross-build/wasm32-wasip2/build/lib.wasi-wasm32-3.14/

SIZE=$(ls -lh "$OUTPUT" | awk '{print $5}')
echo "stdlib bundled: $OUTPUT ($SIZE)"

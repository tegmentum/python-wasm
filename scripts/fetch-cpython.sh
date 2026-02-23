#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPS_DIR="$PROJECT_DIR/deps"
CPYTHON_DIR="$DEPS_DIR/cpython"

CPYTHON_VERSION="v3.14.3"

# Guard: skip if already cloned
if [ -f "$CPYTHON_DIR/configure" ]; then
    echo "CPython source already present at $CPYTHON_DIR"
    exit 0
fi

mkdir -p "$DEPS_DIR"

echo "Cloning CPython $CPYTHON_VERSION..."
git clone --depth 1 --branch "$CPYTHON_VERSION" \
    https://github.com/python/cpython.git "$CPYTHON_DIR"

# Apply patches
PATCHES_DIR="$PROJECT_DIR/patches"
if [ -d "$PATCHES_DIR" ] && ls "$PATCHES_DIR"/*.patch >/dev/null 2>&1; then
    echo "Applying patches..."
    cd "$CPYTHON_DIR"
    for patch in "$PATCHES_DIR"/*.patch; do
        echo "  Applying $(basename "$patch")..."
        git apply "$patch"
    done
    cd "$PROJECT_DIR"
fi

echo "CPython $CPYTHON_VERSION ready at $CPYTHON_DIR"

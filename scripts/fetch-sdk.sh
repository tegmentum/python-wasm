#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPS_DIR="$PROJECT_DIR/deps"

SDK_VERSION="29.0"
SDK_DIR="$DEPS_DIR/wasi-sdk-${SDK_VERSION}-arm64-macos"

# Guard: skip if already downloaded
if [ -f "$SDK_DIR/VERSION" ]; then
    echo "WASI SDK $SDK_VERSION already present at $SDK_DIR"
    exit 0
fi

mkdir -p "$DEPS_DIR"

TARBALL="wasi-sdk-${SDK_VERSION}-arm64-macos.tar.gz"
URL="https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-29/${TARBALL}"

echo "Downloading WASI SDK $SDK_VERSION..."
curl -L --retry 3 --progress-bar -o "$DEPS_DIR/$TARBALL" "$URL"

echo "Extracting..."
tar xzf "$DEPS_DIR/$TARBALL" -C "$DEPS_DIR"
rm "$DEPS_DIR/$TARBALL"

# Verify key binaries exist
if [ ! -x "$SDK_DIR/bin/clang" ]; then
    echo "ERROR: $SDK_DIR/bin/clang not found after extraction" >&2
    exit 1
fi

if [ ! -x "$SDK_DIR/bin/wasm-component-ld" ]; then
    echo "ERROR: $SDK_DIR/bin/wasm-component-ld not found after extraction" >&2
    exit 1
fi

echo "WASI SDK $SDK_VERSION installed at $SDK_DIR"

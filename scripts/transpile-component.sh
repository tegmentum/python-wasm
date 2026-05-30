#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PROFILE="${PROFILE:-default}"
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"

CPYTHON_DIR="$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR"
# Prefer the composed component (Phase 1/2/...). Fall back to the raw
# wasi-sdk build only if no capability extensions are wired in.
RAW_PYTHON_WASM="$CPYTHON_DIR/cross-build/$HOST_TRIPLE/python.wasm"
COMPOSED_PYTHON_WASM="$PROJECT_DIR/$BUILD_DIR/python.composed.wasm"
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

# JSPI is required for the ws-gateway path: pollable.block, the io
# blocking ops, and input-stream.read all need WebAssembly.Suspending
# so the wasm guest yields the host event loop while waiting on the
# tunneled TCP roundtrip. --async-wasi-imports covers the standard
# blocking ones; --async-imports explicitly adds input-stream.read
# (spec'd non-blocking, but Python's recv translates to a tight
# read-poll loop in wasi-libc -- without the yield the host event
# loop is starved, inbound WS frames queue forever, and node OOMs).
# Without these flags the same wasm bundle still works for non-network
# code (cap-routed compression / sqlite / hashlib stay sync-fast).
npx --prefix "$PROJECT_DIR/web" jco transpile "$PYTHON_WASM" \
    -o "$OUTPUT_DIR" \
    --no-nodejs-compat \
    --instantiation async \
    --name python \
    --async-mode jspi \
    --async-wasi-imports \
    --async-wasi-exports \
    --async-imports 'wasi:io/streams@0.2.6#[method]input-stream.read'

# Workaround for an upstream jco/wac issue: jco's emitted
# `definedResourceTables` array doesn't include the RTIDs of resources
# defined by composed-in capability components (zlib's compressor,
# sqlite's connection, etc.). When transferBorrow runs across the
# python.wasm -> cap boundary it checks this array; if false, it wraps
# the rep in a new borrow handle instead of returning the rep directly.
# The cap's wit-bindgen-c export wrapper then dereferences that handle
# as a pointer and reads garbage (-> compress_chunk: stream error;
# sqlite Connection is closed).
#
# Replace the static array with an always-true Proxy. For our setup
# every guest-defined resource SHOULD return rep -- callers are
# python.wasm importing from caps, so the cap (definer) always wants
# the rep. Wasmtime does this implicitly; jco's emitted JS forgets to.
PATCH_FILE="$OUTPUT_DIR/python.js"
if grep -q "^const definedResourceTables = \[" "$PATCH_FILE"; then
    sed -i.bak 's|^const definedResourceTables = \[.*\];$|const definedResourceTables = new Proxy([], { get: () => true });|' "$PATCH_FILE"
    rm -f "$PATCH_FILE.bak"
    echo "Applied definedResourceTables -> Proxy(always-true) patch (cap-resource fix)."
else
    echo "WARN: transpile-component: definedResourceTables line not found -- patch skipped." >&2
fi

echo "Transpiled to $OUTPUT_DIR"
ls -lh "$OUTPUT_DIR"

#!/usr/bin/env bash
# Phase 0 verification gate (see docs/componentize-python.md).
#
# Asserts that the wasi-sdk-built python.wasm is a valid wasm component with:
#   - export wasi:cli/run@<v>
#   - only wasi:* imports
# This is a regression guard. If a future toolchain change drops the build
# back to a core module (no component wrap), the existing transpile/web
# pipeline silently still works in some configurations but stops composing
# with capability components — this gate catches that explicitly.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PYW="$PROJECT_DIR/deps/cpython/cross-build/wasm32-wasip2/python.wasm"

if [ ! -f "$PYW" ]; then
    echo "verify-python-component: $PYW not found — run 'make build' first." >&2
    exit 1
fi

command -v wasm-tools >/dev/null 2>&1 || {
    echo "verify-python-component: 'wasm-tools' is required on PATH." >&2
    exit 1
}

if ! wasm-tools validate "$PYW" >/dev/null 2>&1; then
    echo "FAIL: $PYW does not validate as wasm." >&2
    exit 1
fi

wit="$(wasm-tools component wit "$PYW" 2>/dev/null || true)"
if [ -z "$wit" ]; then
    echo "FAIL: $PYW is a core module, not a component (wasm-tools component wit produced no output)." >&2
    exit 1
fi

if ! printf '%s\n' "$wit" | grep -qE '^\s+export wasi:cli/run@'; then
    echo "FAIL: component does not export wasi:cli/run@*. Got:" >&2
    printf '%s\n' "$wit" | grep -E '^\s+export' >&2 || true
    exit 1
fi

non_wasi="$(printf '%s\n' "$wit" | grep -E '^\s+import' | grep -v 'wasi:' || true)"
if [ -n "$non_wasi" ]; then
    echo "FAIL: component has non-wasi:* imports (Phase 0 requires only wasi:*):" >&2
    printf '%s\n' "$non_wasi" >&2
    exit 1
fi

size="$(du -h "$PYW" | cut -f1)"
echo "OK: $PYW is a valid wasi-p2 component ($size), exports wasi:cli/run, imports only wasi:*."

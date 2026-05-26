#!/usr/bin/env bash
# Componentize-python plan, Phase 1: compose python.wasm with the
# compression-multiplexer capability component, producing python.composed.wasm.
#
# This is the wac-plug interim step. Phase 4 will replace this script with a
# composectl-emit plan that takes the same inputs from a CAS.
#
# Output: build/python.composed.wasm
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PYW="$PROJECT_DIR/deps/cpython/cross-build/wasm32-wasip2/python.wasm"
MUX="${COMPRESSION_MULTIPLEXER_WASM:-$HOME/git/compression-multiplexer/target/wasm32-wasip2/release/compression_multiplexer.wasm}"
OUT="$PROJECT_DIR/build/python.composed.wasm"

[ -f "$PYW" ] || { echo "compose-python-component: $PYW not found — run 'make build' first." >&2; exit 1; }
[ -f "$MUX" ] || { echo "compose-python-component: compression_multiplexer.wasm not found ($MUX)." >&2; exit 1; }
command -v wac >/dev/null 2>&1 || { echo "compose-python-component: 'wac' (wac-cli) is required on PATH." >&2; exit 1; }

mkdir -p "$PROJECT_DIR/build"
wac plug "$PYW" --plug "$MUX" -o "$OUT"
echo "==> $(du -h "$OUT" | cut -f1) $OUT"

# Sanity: the compression-dispatcher import is satisfied.
remaining="$(wasm-tools component wit "$OUT" 2>/dev/null \
    | grep -E '^\s+import' | grep -v 'wasi:' || true)"
if [ -n "$remaining" ]; then
    echo "WARN: composed component still has non-wasi:* imports:" >&2
    printf '%s\n' "$remaining" >&2
fi

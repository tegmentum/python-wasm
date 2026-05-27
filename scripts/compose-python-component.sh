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
OUT="$PROJECT_DIR/build/python.composed.wasm"

# Capability artifacts — overridable via env vars. As capability components are
# added to CPython's set of WIT imports (Phase 1, Phase 2, ...) they get plugged
# in here so the composed component has zero unsatisfied non-wasi:* imports.
MUX_COMPRESSION="${COMPRESSION_MULTIPLEXER_WASM:-$HOME/git/compression-multiplexer/target/wasm32-wasip2/release/compression_multiplexer.wasm}"
MUX_CRYPTO_HASH="${CRYPTO_HASH_MULTIPLEXER_WASM:-$HOME/git/crypto-hash-multiplexer/target/wasm32-wasip2/release/crypto_hash_multiplexer.wasm}"
MUX_HASHING="${HASHING_MULTIPLEXER_WASM:-$HOME/git/hashing-multiplexer/target/wasm32-wasip2/release/hashing_multiplexer.wasm}"
OPENSSL_COMPONENT="${OPENSSL_COMPONENT_WASM:-$HOME/git/openssl-wasm/build/openssl-component.wasm}"
SQLITE_COMPONENT="${SQLITE_COMPONENT_WASM:-$HOME/git/sqlite-wasm/build/sqlite-core.wasm}"

[ -f "$PYW" ] || { echo "compose-python-component: $PYW not found — run 'make build' first." >&2; exit 1; }
for f in "$MUX_COMPRESSION" "$MUX_CRYPTO_HASH" "$MUX_HASHING" "$OPENSSL_COMPONENT" "$SQLITE_COMPONENT"; do
    [ -f "$f" ] || { echo "compose-python-component: capability not found: $f" >&2; exit 1; }
done
command -v wac >/dev/null 2>&1 || { echo "compose-python-component: 'wac' (wac-cli) is required on PATH." >&2; exit 1; }

mkdir -p "$PROJECT_DIR/build"
wac plug "$PYW" \
    --plug "$MUX_COMPRESSION" \
    --plug "$MUX_CRYPTO_HASH" \
    --plug "$MUX_HASHING" \
    --plug "$OPENSSL_COMPONENT" \
    --plug "$SQLITE_COMPONENT" \
    -o "$OUT"
echo "==> $(du -h "$OUT" | cut -f1) $OUT"

# Sanity: the compression-dispatcher import is satisfied.
remaining="$(wasm-tools component wit "$OUT" 2>/dev/null \
    | grep -E '^\s+import' | grep -v 'wasi:' || true)"
if [ -n "$remaining" ]; then
    echo "WARN: composed component still has non-wasi:* imports:" >&2
    printf '%s\n' "$remaining" >&2
fi

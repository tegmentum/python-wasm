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

# Load profile to derive paths. PROFILE comes from env (set by the Makefile)
# or defaults to "default" for direct invocation. The loader emits the
# capability env vars too, but only if the profile doesn't override what the
# caller already exported — env wins via parameter expansion below.
PROFILE="${PROFILE:-default}"
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"

PYW="$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR/cross-build/$HOST_TRIPLE/python.wasm"
OUT_DIR="$PROJECT_DIR/$BUILD_DIR"
OUT="$OUT_DIR/python.composed.wasm"

# Capability artifacts — overridable via env vars. As capability components are
# added to CPython's set of WIT imports (Phase 1, Phase 2, ...) they get plugged
# in here so the composed component has zero unsatisfied non-wasi:* imports.
ZLIB_COMPONENT="${ZLIB_COMPONENT_WASM:-$HOME/git/zlib-wasm/build/bin/zlib.component.wasm}"
BZIP2_COMPONENT="${BZIP2_COMPONENT_WASM:-$HOME/git/bzip2-wasm/target/wasm32-wasip2/release/bzip2_wasm.wasm}"
LZMA_COMPONENT="${LZMA_COMPONENT_WASM:-$HOME/git/lzma-wasm/target/wasm32-wasip2/release/lzma_wasm.wasm}"
ZSTD_COMPONENT="${ZSTD_COMPONENT_WASM:-$HOME/git/zstd-wasm/target/wasm32-wasip2/release/zstd_wasm.wasm}"
LZ4_COMPONENT="${LZ4_COMPONENT_WASM:-$HOME/git/lz4-wasm/target/wasm32-wasip2/release/lz4_wasm.wasm}"
OPENZL_COMPONENT_COMPRESSION="${OPENZL_COMPONENT_WASM:-$HOME/git/openzl-wasm/target/wasm32-wasip2/release/openzl_wasm.wasm}"
MUX_CRYPTO_HASH="${CRYPTO_HASH_MULTIPLEXER_WASM:-$HOME/git/crypto-hash-multiplexer/target/wasm32-wasip2/release/crypto_hash_multiplexer.wasm}"
MUX_HASHING="${HASHING_MULTIPLEXER_WASM:-$HOME/git/hashing-multiplexer/target/wasm32-wasip2/release/hashing_multiplexer.wasm}"
OPENSSL_COMPONENT="${OPENSSL_COMPONENT_WASM:-$HOME/git/openssl-wasm/build/openssl-component.wasm}"
SQLITE_COMPONENT="${SQLITE_COMPONENT_WASM:-$HOME/git/sqlite-wasm/build/sqlite-core.wasm}"
# Tier 1 (v86): defaults to the v86-posix-stub artifact (every spawn returns
# guest-not-ready). Swap V86_POSIX_COMPONENT to the real v86-component build
# once it exports v86:posix/process — same contract, different digest.
V86_POSIX_COMPONENT="${V86_POSIX_COMPONENT_WASM:-$HOME/git/v86/target/wasm32-wasip2/release/v86_posix_stub.wasm}"
PASSWORD_HASH_MULTIPLEXER="${PASSWORD_HASH_MULTIPLEXER_WASM:-$HOME/git/password-hash-multiplexer/target/wasm32-wasip2/release/password_hash_multiplexer.wasm}"

# pylon Phase 4.1: when WITH_V86_POSIX=0, the bare python.wasm has no
# v86:posix/process import (wire-cpython-ext.sh skipped the _v86_posix
# extension), so the v86-posix-stub plug must be skipped here too —
# wac plug fails if the plug provides an export nothing requires.
PLUG_ARGS=(
    --plug "$ZLIB_COMPONENT"
    --plug "$BZIP2_COMPONENT"
    --plug "$LZMA_COMPONENT"
    --plug "$ZSTD_COMPONENT"
    --plug "$LZ4_COMPONENT"
    --plug "$OPENZL_COMPONENT_COMPRESSION"
    --plug "$MUX_CRYPTO_HASH"
    --plug "$MUX_HASHING"
    --plug "$OPENSSL_COMPONENT"
    --plug "$SQLITE_COMPONENT"
    --plug "$PASSWORD_HASH_MULTIPLEXER"
)
REQUIRED_PLUGS=("$ZLIB_COMPONENT"
                "$BZIP2_COMPONENT" "$LZMA_COMPONENT" "$ZSTD_COMPONENT"
                "$LZ4_COMPONENT" "$OPENZL_COMPONENT_COMPRESSION"
                "$MUX_CRYPTO_HASH" "$MUX_HASHING"
                "$OPENSSL_COMPONENT" "$SQLITE_COMPONENT" "$PASSWORD_HASH_MULTIPLEXER")
if [ "${WITH_V86_POSIX:-1}" = "1" ]; then
    PLUG_ARGS+=(--plug "$V86_POSIX_COMPONENT")
    REQUIRED_PLUGS+=("$V86_POSIX_COMPONENT")
fi

[ -f "$PYW" ] || { echo "compose-python-component: $PYW not found — run 'make build' first." >&2; exit 1; }
for f in "${REQUIRED_PLUGS[@]}"; do
    [ -f "$f" ] || { echo "compose-python-component: capability not found: $f" >&2; exit 1; }
done
command -v wac >/dev/null 2>&1 || { echo "compose-python-component: 'wac' (wac-cli) is required on PATH." >&2; exit 1; }

mkdir -p "$OUT_DIR"
wac plug "$PYW" "${PLUG_ARGS[@]}" -o "$OUT"

# Strip non-essential custom sections (DWARF debug info, name table,
# producers, target_features). Drops python.composed.wasm from ~43 MiB
# to ~17 MiB with no behavioral change — the stripped sections only
# matter for source-mapped debuggers. Opt out via COMPOSED_STRIP=0
# (debug-symbol builds) or [build].strip_composed = false in the profile.
if [ "${COMPOSED_STRIP:-1}" = "1" ] && command -v wasm-tools >/dev/null 2>&1; then
    wasm-tools strip "$OUT" -o "${OUT}.stripped" \
        && mv "${OUT}.stripped" "$OUT" \
        && echo "==> $(du -h "$OUT" | cut -f1) $OUT  (stripped)"
else
    echo "==> $(du -h "$OUT" | cut -f1) $OUT  (unstripped — COMPOSED_STRIP=0)"
fi

# Back-compat path for pre-profiles tooling that hardcodes build/python.composed.wasm
# (e.g. ~/git/v86/scripts/test-v86-posix-roundtrip.sh). Point a symlink at
# whichever profile just built. Safe to overwrite on each compose.
ln -sfn "$(basename "$OUT_DIR")/$(basename "$OUT")" \
        "$PROJECT_DIR/build/python.composed.wasm"

# Sanity: the compression-dispatcher import is satisfied.
remaining="$(wasm-tools component wit "$OUT" 2>/dev/null \
    | grep -E '^\s+import' | grep -v 'wasi:' || true)"
if [ -n "$remaining" ]; then
    echo "WARN: composed component still has non-wasi:* imports:" >&2
    printf '%s\n' "$remaining" >&2
fi

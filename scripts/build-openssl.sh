#!/usr/bin/env bash
# Assemble an OpenSSL prefix (headers + static libs) for wasm32-wasip2 from the
# prebuilt openssl-wasm component, so CPython's `Tools/wasm/wasi build` can detect
# it (--with-openssl) and statically link the _ssl and _hashlib extension modules
# into python.wasm.
#
# openssl-wasm builds real OpenSSL 3.x with wasi-sdk 33 (the same SDK as this
# project), so the static libs are ABI-compatible. Building OpenSSL for wasip2
# from scratch is a heavy, patch-heavy port; this reuses the working artifacts.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PREFIX="$PROJECT_DIR/deps/openssl-prefix"

# Locate the openssl-wasm checkout (override with OPENSSL_WASM_DIR).
OSSL="${OPENSSL_WASM_DIR:-}"
for cand in "$OSSL" "$PROJECT_DIR/../openssl-wasm" "$HOME/git/openssl-wasm"; do
    if [ -n "$cand" ] && [ -f "$cand/build/openssl/libcrypto.a" ]; then OSSL="$cand"; break; fi
done
if [ -z "$OSSL" ] || [ ! -f "$OSSL/build/openssl/libcrypto.a" ]; then
    echo "ERROR: openssl-wasm not found (set OPENSSL_WASM_DIR). Need build/openssl/{libcrypto.a,libssl.a}." >&2
    exit 1
fi

if [ -f "$PREFIX/lib/libssl.a" ] && [ -f "$PREFIX/include/openssl/ssl.h" ]; then
    echo "OpenSSL prefix already assembled at $PREFIX; skipping."
    exit 0
fi

echo "Assembling OpenSSL prefix from $OSSL ..."
rm -rf "$PREFIX"
mkdir -p "$PREFIX/lib" "$PREFIX/include"
# Full upstream header set (incl. macros.h/opensslconf.h), then overlay the
# build-generated configured headers (configuration.h/opensslv.h).
cp -R "$OSSL/third_party/openssl/include/openssl" "$PREFIX/include/openssl"
cp "$OSSL"/build/openssl/include/openssl/*.h "$PREFIX/include/openssl/" 2>/dev/null || true
cp "$OSSL/build/openssl/libcrypto.a" "$OSSL/build/openssl/libssl.a" "$PREFIX/lib/"

# openssl-wasm is built no-threads (single-threaded WASI). OpenSSL still ships the
# threading API as no-op stubs, so define OPENSSL_THREADS to satisfy CPython's
# "requires thread-safe OpenSSL" guard (safe: the interpreter is single-threaded).
conf="$PREFIX/include/openssl/opensslconf.h"
if ! grep -q "define OPENSSL_THREADS" "$conf"; then
    printf '#ifndef OPENSSL_THREADS\n#define OPENSSL_THREADS 1\n#endif\n' | cat - "$conf" > "$conf.tmp"
    mv "$conf.tmp" "$conf"
fi

echo "OpenSSL prefix ready: $PREFIX (pass --with-openssl=$PREFIX to CPython configure)."

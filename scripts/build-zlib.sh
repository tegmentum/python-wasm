#!/usr/bin/env bash
# Build zlib for wasm32-wasip2 and install it into the wasi-sdk sysroot so
# CPython's `Tools/wasm/wasi build` detects it (MODULE_ZLIB_TRUE) and statically
# links the `zlib` extension module into python.wasm.
#
# wasi-sdk ships no zlib, so without this the WASI CPython build reports
# "stdlib extension module zlib... missing" and `import zlib` fails. Run before
# `make build` (the build target does this automatically).
#
# TRANSITIONAL (componentize-python plan, Phase 5):
#   The static zlib here is superseded by the _compression CPython extension +
#   compression-multiplexer capability component (see Phase 1, cpython-ext/
#   _compression/). This script stays for parity / A-B comparison with the
#   capability-based path; Lib/zlib.py shimming and full retirement happens
#   once the capability path has shipped a release. See
#   docs/componentize-python.md for the retirement plan.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SDK="$PROJECT_DIR/deps/wasi-sdk-33.0-arm64-macos"
SYSROOT="$SDK/share/wasi-sysroot"
TARGET="wasm32-wasip2"
ZLIB_VERSION="1.3.1"
BUILD_DIR="$PROJECT_DIR/deps/zlib-build"

LIB="$SYSROOT/lib/$TARGET/libz.a"
HDR="$SYSROOT/include/$TARGET/zlib.h"

if [ -f "$LIB" ] && [ -f "$HDR" ]; then
    echo "zlib already installed in the wasi-sdk sysroot ($TARGET); skipping."
    exit 0
fi

if [ ! -d "$SDK" ]; then
    echo "ERROR: wasi-sdk not found at $SDK. Run scripts/fetch-sdk.sh first." >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ ! -d "zlib-$ZLIB_VERSION" ]; then
    echo "Fetching zlib $ZLIB_VERSION..."
    curl -fsSL -o zlib.tar.gz \
        "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz"
    tar xzf zlib.tar.gz
fi
cd "zlib-$ZLIB_VERSION"

CC="$SDK/bin/clang"
# All core + gz* sources: CPython's zlibmodule only needs the core, but
# configure's link probe is `gzread in -lz`, so the gz API must be present.
# The per-target sysroot include dir must be passed explicitly (clang does not
# auto-add include/<target> here); gz* needs <sys/types.h> from there.
SRCS="adler32 crc32 deflate infback inffast inflate inftrees trees zutil compress uncompr gzclose gzlib gzread gzwrite"
echo "Compiling zlib for $TARGET..."
objs=""
for s in $SRCS; do
    "$CC" --target="$TARGET" --sysroot="$SYSROOT" -isystem "$SYSROOT/include/$TARGET" \
        -O2 -DHAVE_UNISTD_H -c "$s.c" -o "$s.o"
    objs="$objs $s.o"
done
"$SDK/bin/llvm-ar" rcs libz.a $objs

install -m 644 zlib.h zconf.h "$SYSROOT/include/$TARGET/"
install -m 644 libz.a "$LIB"
echo "Installed zlib $ZLIB_VERSION (libz.a + headers) into $SYSROOT for $TARGET."

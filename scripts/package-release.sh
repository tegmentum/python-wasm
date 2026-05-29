#!/usr/bin/env bash
# Build the python-wasm release tarball.
#
# D₁a of docs/distribution-channels-plan.md.
#
# Output: dist/release/python-wasm-<version>-<platform>.tar.gz
#
# The tarball contents are platform-independent (wasm is universal; the
# launcher is POSIX sh). Per-platform tarballs exist for distribution
# clarity and so install.sh / brew can pick the right one without
# detection logic on the user side.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

VERSION="${1:-0.1.0}"
PLATFORM="${2:-$(uname | tr A-Z a-z)-$(uname -m | sed 's/x86_64/x86_64/;s/aarch64/arm64/;s/arm64/arm64/')}"
OUT_DIR="$PROJECT_DIR/dist/release"

PROFILE="${PROFILE:-default}"
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"

COMPOSED="$PROJECT_DIR/$BUILD_DIR/python.composed.wasm"
[ -f "$COMPOSED" ] || { echo "package-release: $COMPOSED missing — run 'make python-composed' first." >&2; exit 1; }

mkdir -p "$OUT_DIR"
NAME="python-wasm-$VERSION-$PLATFORM"
STAGE="$(mktemp -d -t pwasm-release-XXXXXX)"
ROOT="$STAGE/$NAME"
trap 'rm -rf "$STAGE"' EXIT

# Layout
#   bin/python-wasm
#   lib/python.composed.wasm
#   lib/cpython/                 ← stdlib + sysconfig + cpython-ext
#   lib/cpython-ext-base/        ← rebuild source tree (scripts/, cpython-ext/, profiles/)
#   lib/python-wasm/*.sh         ← subcommand helpers
#   share/python-wasm/LICENSE
#   share/python-wasm/README.md
#   VERSION
mkdir -p "$ROOT/bin" "$ROOT/lib" "$ROOT/lib/python-wasm" "$ROOT/share/python-wasm"

# 1. Launcher + helpers
cp "$PROJECT_DIR/packaging/bin/python-wasm"          "$ROOT/bin/"
cp "$PROJECT_DIR/packaging/lib/python-wasm"/*.sh     "$ROOT/lib/python-wasm/"
chmod +x "$ROOT/bin/python-wasm" "$ROOT/lib/python-wasm"/*.sh

# 2. The composed wasm
cp "$COMPOSED" "$ROOT/lib/python.composed.wasm"

# 3. The bundled cpython stdlib tree. We need everything python.composed.wasm
# mounts at runtime: Lib/, cross-build/<triple>/build/lib.wasi-wasm32-<minor>/
# (for the sysconfigdata), Modules/ (for Lib/ shim sources).
CPYTHON_DIR="$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR"
BUNDLED_CPYTHON="$ROOT/lib/cpython"
mkdir -p "$BUNDLED_CPYTHON/Lib" "$BUNDLED_CPYTHON/cross-build/$HOST_TRIPLE/build"

cp -R "$CPYTHON_DIR/Lib"/ "$BUNDLED_CPYTHON/Lib/"
LIBDIR_DIR="$(ls -d "$CPYTHON_DIR"/cross-build/"$HOST_TRIPLE"/build/lib.wasi-wasm32-* | head -1)"
cp -R "$LIBDIR_DIR" "$BUNDLED_CPYTHON/cross-build/$HOST_TRIPLE/build/"

# 4. rebuild source tree — Phase 8 / wheel-MVP need this
EXT_BASE="$ROOT/lib/cpython-ext-base"
mkdir -p "$EXT_BASE"
# Required: cpython-ext/ (sources), scripts/ (build-from-pkgs + rebuild
# helpers), profiles/ (build profiles), patches/ (per-version patches),
# Makefile (target wiring), the wit-bindgen-c sub-trees.
cp -R "$PROJECT_DIR/cpython-ext"  "$EXT_BASE/cpython-ext"
cp -R "$PROJECT_DIR/scripts"      "$EXT_BASE/scripts"
cp -R "$PROJECT_DIR/profiles"     "$EXT_BASE/profiles"
[ -d "$PROJECT_DIR/patches" ] && cp -R "$PROJECT_DIR/patches" "$EXT_BASE/patches"
cp "$PROJECT_DIR/Makefile" "$EXT_BASE/Makefile" 2>/dev/null || true

# 5. Metadata
[ -f "$PROJECT_DIR/LICENSE" ]   && cp "$PROJECT_DIR/LICENSE"   "$ROOT/share/python-wasm/" || true
[ -f "$PROJECT_DIR/README.md" ] && cp "$PROJECT_DIR/README.md" "$ROOT/share/python-wasm/" || true
echo "$VERSION" > "$ROOT/VERSION"

# 6. Tar it up
TARBALL="$OUT_DIR/$NAME.tar.gz"
(cd "$STAGE" && tar czf "$TARBALL" "$NAME")
echo "==> $(du -h "$TARBALL" | cut -f1) $TARBALL"

# 7. Checksums (append-or-create)
SHA256_FILE="$OUT_DIR/checksums.txt"
(cd "$OUT_DIR" && shasum -a 256 "$(basename "$TARBALL")") >> "$SHA256_FILE"
echo "==> checksum recorded in $SHA256_FILE"

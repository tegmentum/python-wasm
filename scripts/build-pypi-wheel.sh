#!/usr/bin/env bash
# Build the pip-installable python-wasm wheel.
#
# D₁d of docs/distribution-channels-plan.md. Stages the bundled
# artifacts under packaging/pypi/src/python_wasm/data/, then runs
# `python -m build --wheel`.
#
# Output: dist/pypi/python_wasm-<version>-py3-none-any.whl
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

VERSION="${1:-0.1.0}"
PACKAGING="$PROJECT_DIR/packaging/pypi"
DATA="$PACKAGING/src/python_wasm/data"
OUT_DIR="$PROJECT_DIR/dist/pypi"

PROFILE="${PROFILE:-default}"
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"

COMPOSED="$PROJECT_DIR/$BUILD_DIR/python.composed.wasm"
[ -f "$COMPOSED" ] || { echo "build-pypi-wheel: $COMPOSED missing — run 'make python-composed' first." >&2; exit 1; }

# Mirror the tarball layout exactly under data/. The launcher then
# resolves prefix=data/ and finds bin/, lib/ at the same offsets it
# would under a tarball install. Also nuke the previous build/ stage
# — setuptools doesn't auto-clean it, and stale files leak into new
# wheels with the old layout.
rm -rf "$DATA" "$PACKAGING/build"
mkdir -p "$DATA/bin" "$DATA/lib/python-wasm" "$DATA/lib/cpython" "$DATA/lib/cpython-ext-base"

# 1. Launcher + helpers
cp "$PROJECT_DIR/packaging/bin/python-wasm"     "$DATA/bin/"
cp "$PROJECT_DIR/packaging/lib/python-wasm"/*.sh "$DATA/lib/python-wasm/"
chmod +x "$DATA/bin/python-wasm" "$DATA/lib/python-wasm"/*.sh

# 2. Composed wasm under lib/ (matches tarball)
cp "$COMPOSED" "$DATA/lib/python.composed.wasm"

# 3. Stdlib tree (Lib/ + sysconfig) under lib/cpython/
CPYTHON_DIR="$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR"
mkdir -p "$DATA/lib/cpython/cross-build/$HOST_TRIPLE/build"
cp -R "$CPYTHON_DIR/Lib"/ "$DATA/lib/cpython/Lib/"
LIBDIR_DIR="$(ls -d "$CPYTHON_DIR"/cross-build/"$HOST_TRIPLE"/build/lib.wasi-wasm32-* | head -1)"
cp -R "$LIBDIR_DIR" "$DATA/lib/cpython/cross-build/$HOST_TRIPLE/build/"

# 4. Rebuild source tree under lib/cpython-ext-base/
cp -R "$PROJECT_DIR/cpython-ext"  "$DATA/lib/cpython-ext-base/cpython-ext"
cp -R "$PROJECT_DIR/scripts"      "$DATA/lib/cpython-ext-base/scripts"
cp -R "$PROJECT_DIR/profiles"     "$DATA/lib/cpython-ext-base/profiles"
[ -d "$PROJECT_DIR/patches" ] && cp -R "$PROJECT_DIR/patches" "$DATA/lib/cpython-ext-base/patches"
cp "$PROJECT_DIR/Makefile" "$DATA/lib/cpython-ext-base/Makefile" 2>/dev/null || true

# 6. Version
echo "$VERSION" > "$DATA/VERSION"

# 7. Update pyproject version
sed -i.bak "s/^version = \".*\"/version = \"$VERSION\"/" "$PACKAGING/pyproject.toml"
rm -f "$PACKAGING/pyproject.toml.bak"

# 8. Build the wheel
mkdir -p "$OUT_DIR"
(cd "$PACKAGING" && python3 -m build --wheel --outdir "$OUT_DIR")
ls -la "$OUT_DIR"/*.whl

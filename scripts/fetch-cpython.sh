#!/usr/bin/env bash
# fetch-cpython.sh: clone a specific CPython version into deps/<source_dir>.
#
# Usage:
#   bash scripts/fetch-cpython.sh                # honors PROFILE (default: default)
#   PROFILE=3.13-current bash scripts/fetch-cpython.sh
#   bash scripts/fetch-cpython.sh 3.13-current   # positional override
#
# Reads PYTHON_VERSION + PYTHON_SOURCE_DIR from the resolved profile. Idempotent:
# if the source tree is already populated, exits 0 without re-cloning.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PROFILE="${1:-${PROFILE:-default}}"
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"

DEPS_DIR="$PROJECT_DIR/deps"
CPYTHON_DIR="$DEPS_DIR/$PYTHON_SOURCE_DIR"
GIT_TAG="v$PYTHON_VERSION"

# Guard: skip if already cloned
if [ -f "$CPYTHON_DIR/configure" ]; then
    echo "CPython source already present at $CPYTHON_DIR ($GIT_TAG)"
    exit 0
fi

mkdir -p "$DEPS_DIR"

echo "Cloning CPython $GIT_TAG into $CPYTHON_DIR..."
git clone --depth 1 --branch "$GIT_TAG" \
    https://github.com/python/cpython.git "$CPYTHON_DIR"

# Apply per-version patches. The CPython tree's structure changes between
# minor versions (Tools/wasm/wasi.py in 3.13, Tools/wasm/wasi/__main__.py
# in 3.14, etc.), so each Python minor version gets its own patches dir.
PY_MINOR="$(printf '%s\n' "$PYTHON_VERSION" | cut -d. -f1-2)"
PATCHES_DIR="$PROJECT_DIR/patches/$PY_MINOR"
if [ -d "$PATCHES_DIR" ] && ls "$PATCHES_DIR"/*.patch >/dev/null 2>&1; then
    echo "Applying $PY_MINOR patches..."
    cd "$CPYTHON_DIR"
    for patch in "$PATCHES_DIR"/*.patch; do
        echo "  Applying $(basename "$patch")..."
        git apply "$patch"
    done
    cd "$PROJECT_DIR"
else
    echo "No patches found for Python $PY_MINOR at $PATCHES_DIR (this is fine if the version doesn't need any)."
fi

# Maintain the back-compat symlink: deps/cpython -> the most recently
# fetched tree. This lets scripts that haven't been profile-refactored yet
# keep working (they'll see the last-fetched version).
if [ ! -e "$DEPS_DIR/cpython" ] || [ -L "$DEPS_DIR/cpython" ]; then
    ln -sfn "$PYTHON_SOURCE_DIR" "$DEPS_DIR/cpython"
    echo "deps/cpython -> $PYTHON_SOURCE_DIR (compat symlink updated)"
fi

echo "CPython $GIT_TAG ready at $CPYTHON_DIR"

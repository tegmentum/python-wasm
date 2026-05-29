#!/usr/bin/env sh
# python-wasm rebuild — recompose python.composed.wasm with any
# pyforge-wheel extensions installed under $PYTHON_WASM_HOME/site-packages.
#
# Delegates to the existing scripts/python-wasm-rebuild.sh living in the
# bundled cpython-ext-base source tree (tarball layout) or the python-wasm
# repo (developer-source-tree layout).
set -eu

PYTHON_WASM_HOME="${PYTHON_WASM_HOME:-$HOME/.python-wasm}"

# Resolve install prefix the same way the launcher does.
script="$0"
while [ -L "$script" ]; do
    target="$(readlink "$script")"
    case "$target" in /*) script="$target" ;; *) script="$(dirname "$script")/$target" ;; esac
done
HELPER_DIR="$(cd "$(dirname "$script")" && pwd)"
PREFIX="$(cd "$HELPER_DIR/../.." && pwd)"

[ -d "$PREFIX/lib/cpython-ext-base" ] || {
    echo "python-wasm rebuild: cpython-ext-base/ not found at $PREFIX/lib/" >&2
    echo "  This install was packaged without rebuild support." >&2
    exit 1
}
SOURCE_ROOT="$PREFIX/lib"

REBUILD_SCRIPT="$SOURCE_ROOT/cpython-ext-base/scripts/python-wasm-rebuild.sh"
[ -x "$REBUILD_SCRIPT" ] || {
    echo "python-wasm rebuild: rebuild helper missing at $REBUILD_SCRIPT" >&2
    exit 1
}

# The underlying script expects PROJECT_DIR to look like the python-wasm
# repo. Reuse the bundled cpython-ext-base as PROJECT_DIR; build outputs
# land at $PYTHON_WASM_HOME/python.composed.wasm as already designed.
exec env PYTHON_WASM_HOME="$PYTHON_WASM_HOME" "$REBUILD_SCRIPT" "$@"

#!/usr/bin/env sh
# python-wasm info — versions, paths, active build digest.
set -eu

PYTHON_WASM_HOME="${PYTHON_WASM_HOME:-$HOME/.python-wasm}"

script="$0"
while [ -L "$script" ]; do
    target="$(readlink "$script")"
    case "$target" in /*) script="$target" ;; *) script="$(dirname "$script")/$target" ;; esac
done
HELPER_DIR="$(cd "$(dirname "$script")" && pwd)"
PREFIX="$(cd "$HELPER_DIR/../.." && pwd)"

[ -f "$PREFIX/VERSION" ]    && VERSION="$(cat "$PREFIX/VERSION")"   || VERSION="(unknown)"
[ -f "$PREFIX/lib/python.composed.wasm" ]   && BUNDLED="$PREFIX/lib/python.composed.wasm"   || BUNDLED="$PREFIX/data/python.composed.wasm"
[ -f "$PYTHON_WASM_HOME/python.composed.wasm" ] && ACTIVE="$PYTHON_WASM_HOME/python.composed.wasm" || ACTIVE="$BUNDLED"

WASMTIME_VERSION="$(wasmtime --version 2>/dev/null || echo '(not on PATH)')"

echo "python-wasm $VERSION"
echo ""
echo "Paths:"
echo "  install prefix:    $PREFIX"
echo "  bundled wasm:      $BUNDLED ($(du -h "$BUNDLED" 2>/dev/null | cut -f1))"
echo "  active wasm:       $ACTIVE ($(du -h "$ACTIVE" 2>/dev/null | cut -f1))"
echo "  user home:         $PYTHON_WASM_HOME"
echo "  site-packages:     $PYTHON_WASM_HOME/site-packages"
echo ""
echo "Toolchain:"
echo "  wasmtime:          $WASMTIME_VERSION"

if [ -d "$PYTHON_WASM_HOME/site-packages" ]; then
    n_ext="$(find "$PYTHON_WASM_HOME/site-packages" -name pyforge-pkg.toml 2>/dev/null | wc -l | tr -d ' ')"
    n_pkg="$(find "$PYTHON_WASM_HOME/site-packages" -maxdepth 1 -name '*.dist-info' -type d 2>/dev/null | wc -l | tr -d ' ')"
    echo ""
    echo "Installed:"
    echo "  pip distributions: $n_pkg"
    echo "  pyforge extensions: $n_ext"
fi

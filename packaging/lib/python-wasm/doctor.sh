#!/usr/bin/env sh
# python-wasm doctor — sanity check the install.
set -eu

PYTHON_WASM_HOME="${PYTHON_WASM_HOME:-$HOME/.python-wasm}"
script="$0"
while [ -L "$script" ]; do
    target="$(readlink "$script")"
    case "$target" in /*) script="$target" ;; *) script="$(dirname "$script")/$target" ;; esac
done
HELPER_DIR="$(cd "$(dirname "$script")" && pwd)"
PREFIX="$(cd "$HELPER_DIR/../.." && pwd)"

OK="\033[32mOK\033[0m"
FAIL="\033[31mFAIL\033[0m"
WARN="\033[33mWARN\033[0m"
fail=0

check() {
    label="$1"; result="$2"; detail="${3:-}"
    printf "  %b %s%s\n" "$result" "$label" "${detail:+   ($detail)}"
    [ "$result" = "$FAIL" ] && fail=$((fail + 1)) || true
}

echo "python-wasm doctor"
echo ""
echo "Install layout:"
[ -f "$PREFIX/lib/python.composed.wasm" ] || [ -f "$PREFIX/data/python.composed.wasm" ] \
    && check "bundled wasm present" "$OK" \
    || check "bundled wasm present" "$FAIL" "no python.composed.wasm under $PREFIX/lib or $PREFIX/data"
[ -d "$PREFIX/lib/cpython" ] || [ -d "$PREFIX/data/cpython" ] \
    && check "stdlib tree present" "$OK" \
    || check "stdlib tree present" "$FAIL" "rebuild + most extensions won't work without it"

echo ""
echo "Runtime:"
if command -v wasmtime >/dev/null 2>&1; then
    check "wasmtime on PATH" "$OK" "$(wasmtime --version | head -1)"
else
    check "wasmtime on PATH" "$FAIL" "install via brew/curl/scoop — see python-wasm --help"
fi

echo ""
echo "User state:"
if [ -d "$PYTHON_WASM_HOME" ]; then
    check "user home exists" "$OK" "$PYTHON_WASM_HOME"
else
    check "user home exists" "$WARN" "$PYTHON_WASM_HOME (will be created on first use)"
fi
if [ -f "$PYTHON_WASM_HOME/python.composed.wasm" ]; then
    check "user-rebuilt wasm" "$OK" "active (overrides bundled)"
else
    check "user-rebuilt wasm" "$WARN" "not present (using bundled)"
fi
if [ -d "$PYTHON_WASM_HOME/site-packages" ]; then
    n="$(find "$PYTHON_WASM_HOME/site-packages" -maxdepth 1 -name '*.dist-info' 2>/dev/null | wc -l | tr -d ' ')"
    check "user site-packages" "$OK" "$n distribution(s)"
else
    check "user site-packages" "$WARN" "not present"
fi

echo ""
if [ $fail -eq 0 ]; then
    echo "All checks passed."
    exit 0
else
    echo "$fail check(s) failed."
    exit 1
fi

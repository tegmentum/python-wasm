#!/usr/bin/env sh
# python-wasm pip — host pip, automatically targeted at
# $PYTHON_WASM_HOME/site-packages so installs are visible to python-wasm.
#
# Pure-Python wheels become importable immediately. Extension wheels
# (anything shipping a pyforge-pkg.toml) require `python-wasm rebuild`
# after install for the bridge to be statically linked.
set -eu

PYTHON_WASM_HOME="${PYTHON_WASM_HOME:-$HOME/.python-wasm}"
TARGET="$PYTHON_WASM_HOME/site-packages"
mkdir -p "$TARGET"

command -v pip3 >/dev/null 2>&1 || command -v pip >/dev/null 2>&1 || {
    echo "python-wasm pip: host pip (pip3 or pip) is required but not on PATH." >&2
    exit 127
}

PIP="$(command -v pip3 2>/dev/null || command -v pip)"

# Always install with --target. Anything else (search, list, show, …)
# gets passed through; only install-shaped subcommands get the --target
# munge.
case "${1:-}" in
    install|download)
        cmd="$1"; shift
        # Detect if --target was already supplied; if so, respect it.
        passthrough=""
        explicit_target=""
        for arg in "$@"; do
            case "$arg" in
                --target=*|--target) explicit_target=1 ;;
            esac
        done
        if [ -n "$explicit_target" ]; then
            exec "$PIP" "$cmd" "$@"
        fi
        exec "$PIP" "$cmd" --target "$TARGET" "$@"
        ;;
    "")
        echo "usage: python-wasm pip <pip-args>..." >&2
        exit 2
        ;;
    *)
        exec "$PIP" "$@"
        ;;
esac

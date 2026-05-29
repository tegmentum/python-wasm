#!/usr/bin/env bash
# Run python.composed.wasm under wasmtime with the standard mount + env layout.
#
# Mounts:
#   - CPython source tree → /              (read-only stdlib + sysconfig)
#   - $PYTHON_WASM_HOME    → /site-packages (writable; pip install lands here)
#
# PYTHONPATH order (first wins):
#   1. bundled pip wheel    (so `python -m pip` works without ensurepip subprocess)
#   2. /site-packages       (installed wheels)
#   3. sysconfig lib dir    (C extension .so files)
#   4. /Lib                 (frozen stdlib backup)
#
# Network flags (-S inherit-network + -S allow-ip-name-lookup) are on by
# default because the cap-routed ssl + sockets paths assume them. Set
# PYTHON_WASM_OFFLINE=1 to drop them.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PROFILE="${PROFILE:-default}"
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"
CPYTHON_DIR="$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR"

# Prefer the composed wasm (caps + stdlib shims wired); fall back to bare
# python.wasm for callers that explicitly want it (no caps).
if [ -n "${PYTHON_WASM_BARE:-}" ]; then
    PYTHON_WASM="$CPYTHON_DIR/cross-build/$HOST_TRIPLE/python.wasm"
else
    PYTHON_WASM="$PROJECT_DIR/$BUILD_DIR/python.composed.wasm"
fi

if [ ! -f "$PYTHON_WASM" ]; then
    echo "ERROR: $PYTHON_WASM not found." >&2
    echo "  Run 'make python-composed PROFILE=$PROFILE' first." >&2
    exit 1
fi

PY_MINOR="$(printf '%s\n' "$PYTHON_VERSION" | cut -d. -f1-2)"
SYSCONFIG_DIR="/cross-build/$HOST_TRIPLE/build/lib.wasi-wasm32-$PY_MINOR"

# Host-side writable site-packages — survives across runs.
PYTHON_WASM_HOME="${PYTHON_WASM_HOME:-$HOME/.python-wasm}"
mkdir -p "$PYTHON_WASM_HOME/site-packages"

# Bundled pip wheel — zipimport handles it on PYTHONPATH.
PIP_WHEEL_PATH="/Lib/ensurepip/_bundled/pip-25.3-py3-none-any.whl"

PYTHONPATH="$PIP_WHEEL_PATH:/site-packages:$SYSCONFIG_DIR:/Lib"

NETWORK_FLAGS=(-S inherit-network -S allow-ip-name-lookup)
if [ -n "${PYTHON_WASM_OFFLINE:-}" ]; then
    NETWORK_FLAGS=()
fi

exec wasmtime run \
    --wasm max-wasm-stack=16777216 \
    "${NETWORK_FLAGS[@]}" \
    --dir "$CPYTHON_DIR::/" \
    --dir "$PYTHON_WASM_HOME/site-packages::/site-packages" \
    --env "PYTHONPATH=$PYTHONPATH" \
    --env "PYTHONUSERBASE=/site-packages" \
    "$PYTHON_WASM" "$@"

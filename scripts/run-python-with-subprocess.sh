#!/usr/bin/env bash
# Run python.composed.wasm with working subprocess support.
#
# Phase 5 of docs/coverage-implementation-plan.md: composes python.wasm with
# ~/git/v86/crates/v86-posix-host instead of the default stub, starts the
# in-tree posix-helper.sh as a sibling host process, mounts a temp mailbox
# dir as /workspace, and exec's the composed wasm. Network flags, the
# writable site-packages mount, and PYTHONPATH layering match
# scripts/run-python.sh.
#
# In a future Phase 5+: the helper moves into the v86 wasmmachine guest
# (~/git/v86/crates/v86-posix-helper) and runs natively under v86 Linux.
# This script's guest-side wiring stays the same — only the backend
# component changes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PROFILE="${PROFILE:-default}"
HOST_WASM="${V86_POSIX_COMPONENT_WASM:-$HOME/git/v86/target/wasm32-wasip2/release/v86_posix_host.wasm}"
HELPER="${POSIX_HELPER_SH:-$HOME/git/v86/crates/v86-posix-host/workspace/posix-helper.sh}"

[ -f "$HOST_WASM" ] || { echo "run-python-with-subprocess: $HOST_WASM not found." >&2; exit 1; }
[ -f "$HELPER" ]    || { echo "run-python-with-subprocess: $HELPER not found." >&2; exit 1; }
command -v wasmtime >/dev/null 2>&1 || { echo "run-python-with-subprocess: 'wasmtime' is required on PATH." >&2; exit 1; }

# Always recompose so the active V86_POSIX_COMPONENT_WASM is what gets
# plugged. Cheap (~1s) and the strip step is idempotent.
V86_POSIX_COMPONENT_WASM="$HOST_WASM" PROFILE="$PROFILE" \
    bash "$SCRIPT_DIR/compose-python-component.sh" >/dev/null

eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"
CPYTHON_DIR="$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR"
COMP="$PROJECT_DIR/$BUILD_DIR/python.composed.wasm"

PY_MINOR="$(printf '%s\n' "$PYTHON_VERSION" | cut -d. -f1-2)"
SYSCONFIG_DIR="/cross-build/$HOST_TRIPLE/build/lib.wasi-wasm32-$PY_MINOR"
PYTHON_WASM_HOME="${PYTHON_WASM_HOME:-$HOME/.python-wasm}"
mkdir -p "$PYTHON_WASM_HOME/site-packages"
PIP_WHEEL_PATH="/Lib/ensurepip/_bundled/pip-25.3-py3-none-any.whl"
PYTHONPATH="$PIP_WHEEL_PATH:/site-packages:$SYSCONFIG_DIR:/Lib"

# Per-run mailbox + helper. Helper runs as a host process that polls
# the mailbox; the in-guest path lands here for free via the --dir mount.
MAILBOX_ROOT="$(mktemp -d -t pwasm-posix-XXXXXX)"
mkdir -p "$MAILBOX_ROOT/posix-mailbox"
HELPER_LOG="$MAILBOX_ROOT/helper.log"

cleanup() {
    [ -n "${HELPER_PID:-}" ] && kill "$HELPER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$MAILBOX_ROOT"
}
trap cleanup EXIT

POSIX_MAILBOX="$MAILBOX_ROOT/posix-mailbox" POSIX_HELPER_LOG="$HELPER_LOG" \
    bash "$HELPER" >>"$HELPER_LOG" 2>&1 &
HELPER_PID=$!
# Brief grace before wasm starts writing requests.
sleep 0.2

NETWORK_FLAGS=(-S inherit-network -S allow-ip-name-lookup)
if [ -n "${PYTHON_WASM_OFFLINE:-}" ]; then
    NETWORK_FLAGS=()
fi

exec wasmtime run \
    --wasm max-wasm-stack=16777216 \
    "${NETWORK_FLAGS[@]}" \
    --dir "$CPYTHON_DIR::/" \
    --dir "$PYTHON_WASM_HOME/site-packages::/site-packages" \
    --dir "$MAILBOX_ROOT::/workspace" \
    --env "PYTHONPATH=$PYTHONPATH" \
    --env "PYTHONUSERBASE=/site-packages" \
    "$COMP" "$@"

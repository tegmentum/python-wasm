#!/usr/bin/env bash
# Local-dev wrapper for ~/git/uv-wasm/dist/uv-dev.wasm.
#
# uv-wasm has three deployable variants (see ~/git/uv-wasm/README.md):
#   - uv.wasm     — needs WasmMachine to provide wasmmachine:command/exec
#   - uv-ipc.wasm — runs on WasmMachine via the /run/wasm/ipc mailbox
#   - uv-dev.wasm — wac-plugged with the host-mock; runs under plain wasmtime
#
# This script invokes the dev variant with the wasmtime flags it needs
# (-S http for fetches; --allow-ip-name-lookup + inherit-network for DNS).
#
# Path A from docs/coverage-implementation-plan.md: uv-wasm is a separate
# WasmMachine app, not embedded in python-wasm. For end-user wheel install
# we delegate via WasmMachine; this script is a developer aid only.
set -euo pipefail

UV_WASM="${UV_WASM:-$HOME/git/uv-wasm/dist/uv-dev.wasm}"

[ -f "$UV_WASM" ] || { echo "uv-dev: $UV_WASM not found — build it with ~/git/uv-wasm/scripts/build.sh" >&2; exit 1; }
command -v wasmtime >/dev/null 2>&1 || { echo "uv-dev: 'wasmtime' is required on PATH." >&2; exit 1; }

exec wasmtime run \
    -S http -S inherit-network -S allow-ip-name-lookup \
    "$UV_WASM" "$@"

#!/usr/bin/env bash
# Phase 0 of docs/coverage-implementation-plan.md.
#
# DNS resolution smoke: socket.getaddrinfo() against the hostnames the wheel
# install / async-HTTP work will hit. Confirms the wasi:sockets/ip-name-lookup
# path is wired end-to-end through python.composed.wasm.
#
# Default-OFF: requires NETWORK=1 (matches scripts/test-ssl-network.sh).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PROFILE="${PROFILE:-default}"
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"
CPYTHON_DIR="$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR"
COMP="$PROJECT_DIR/$BUILD_DIR/python.composed.wasm"

if [ -z "${NETWORK:-}" ] && [ -z "${CI_NETWORK_OK:-}" ]; then
    echo "SKIP: network-gated test. Re-run with NETWORK=1 (or CI_NETWORK_OK=1) to enable." >&2
    exit 0
fi

[ -f "$COMP" ] || { echo "test-dns-resolution: $COMP not found — run scripts/compose-python-component.sh first." >&2; exit 1; }
command -v wasmtime >/dev/null 2>&1 || { echo "test-dns-resolution: 'wasmtime' is required on PATH." >&2; exit 1; }

LIBDIR="$(basename "$(ls -d "$CPYTHON_DIR"/cross-build/"$HOST_TRIPLE"/build/lib.wasi-wasm32-* | head -1)")"

wasmtime run --wasm max-wasm-stack=16777216 \
    -S inherit-network -S allow-ip-name-lookup \
    --dir "$CPYTHON_DIR::/" \
    --env "PYTHONPATH=/cross-build/wasm32-wasip2/build/$LIBDIR:/Lib" \
    "$COMP" -c "
import socket
import sys

failures = 0
def expect(cond, msg):
    global failures
    if cond: print(f'OK   : {msg}')
    else:    print(f'FAIL : {msg}'); failures += 1

print('--- socket.getaddrinfo() smoke ---')
for host in ('pypi.org', 'files.pythonhosted.org', 'example.com'):
    try:
        infos = socket.getaddrinfo(host, 443, type=socket.SOCK_STREAM)
        expect(len(infos) >= 1, f'{host} -> {len(infos)} entries')
    except Exception as e:
        expect(False, f'{host}: {type(e).__name__}: {e}')

print('--- gethostbyname() smoke (legacy IPv4-only API) ---')
for host in ('pypi.org', 'example.com'):
    try:
        addr = socket.gethostbyname(host)
        expect(addr.count('.') == 3, f'{host} -> {addr}')
    except Exception as e:
        expect(False, f'{host}: {type(e).__name__}: {e}')

print(f'\\n{failures} failures' if failures else '\\nall pass')
sys.exit(failures)
"

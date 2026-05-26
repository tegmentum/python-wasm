#!/usr/bin/env bash
# Componentize-python plan, Phase 3c.1: NETWORK-GATED end-to-end TLS smoke.
#
# Performs a real HTTPS request through python.composed.wasm's _ssl_capability
# -> openssl-component -> wasi:sockets/tcp (wasmtime). Requires network access
# to example.com:443.
#
# Default-OFF: must opt in with NETWORK=1 (avoid surprises in CI without
# explicit network grant).
#
# This is the gating decision-point #2 from docs/phase-3-tls.md:
# "Does a real handshake against a real TLS server actually work?"
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMP="$PROJECT_DIR/build/python.composed.wasm"

if [ -z "${NETWORK:-}" ] && [ -z "${CI_NETWORK_OK:-}" ]; then
    echo "SKIP: network-gated test. Re-run with NETWORK=1 (or CI_NETWORK_OK=1) to enable." >&2
    exit 0
fi

[ -f "$COMP" ] || { echo "test-ssl-network: $COMP not found — run scripts/compose-python-component.sh first." >&2; exit 1; }
command -v wasmtime >/dev/null 2>&1 || { echo "test-ssl-network: 'wasmtime' is required on PATH." >&2; exit 1; }

LIBDIR="$(basename "$(ls -d "$PROJECT_DIR"/deps/cpython/cross-build/wasm32-wasip2/build/lib.wasi-wasm32-* | head -1)")"

wasmtime run --wasm max-wasm-stack=16777216 \
    -S inherit-network -S allow-ip-name-lookup \
    --dir "$PROJECT_DIR/deps/cpython::/" \
    --env "PYTHONPATH=/cross-build/wasm32-wasip2/build/$LIBDIR" \
    "$COMP" -c "
import sys
import _ssl_capability

failures = 0
def expect(cond, msg):
    global failures
    if cond: print(f'OK   : {msg}')
    else:    print(f'FAIL : {msg}'); failures += 1

# Phase 3b.4 will load WebPKI roots so CERT_REQUIRED works. For now, the
# v1 smoke runs verify_mode=NONE because the bundled trust store is empty.
print('--- TLS 1.3 handshake to example.com:443 ---')
ctx = _ssl_capability._SSLContext()
ctx.verify_mode = _ssl_capability.CERT_NONE  # TODO 3b.4: ship WebPKI roots
sock = ctx.wrap_socket('example.com', 443, server_hostname='example.com')
expect(sock.version() in ('TLSv1.2', 'TLSv1.3'), f'version (got {sock.version()})')
cipher = sock.cipher()
expect(cipher and len(cipher) == 3, f'cipher tuple (got {cipher})')
expect(sock.server_hostname == 'example.com', 'server_hostname stored')

print('--- HTTPS request ---')
req = b'GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n'
n = sock.write(req)
expect(n == len(req), f'wrote {n}/{len(req)} bytes')

data = b''
while True:
    chunk = sock.read(4096)
    if not chunk: break
    data += chunk
    if len(data) > 65536: break
expect(len(data) > 100, f'response received ({len(data)} bytes)')
first_line = data.split(b'\r\n', 1)[0].decode(errors='replace')
expect(first_line.startswith('HTTP/'), f'looks like HTTP response (first line: {first_line!r})')
expect(b'200' in data.split(b'\r\n', 1)[0], 'HTTP 200')

sock.shutdown()
sock.shutdown()  # idempotent
expect(True, 'shutdown is idempotent')

# Closed socket: subsequent ops raise SSLError
try:
    sock.write(b'x')
    expect(False, 'write-after-shutdown raises')
except _ssl_capability.SSLError:
    expect(True, 'write-after-shutdown raises SSLError')

print('---')
print('PASS' if failures == 0 else f'{failures} FAILURES')
sys.exit(failures)
" \
    && echo "OK: network TLS handshake + HTTPS round trip via openssl-component." \
    || { echo "FAIL: network-gated TLS smoke broke." >&2; exit 1; }

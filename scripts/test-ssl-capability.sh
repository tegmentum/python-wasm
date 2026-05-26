#!/usr/bin/env bash
# Componentize-python plan, Phase 3b: end-to-end smoke of _ssl_capability
# inside the composed python.wasm, currently exercising:
#
#   Phase 3b.1 — module loads, openssl-component imports satisfied
#   Phase 3b.2 — MemoryBIO basic + EOF + FIFO + buffer-protocol semantics
#
# Future phases (3b.3+) extend this script with _SSLContext / _SSLSocket
# tests, real handshake, etc. Comparison against CPython's own
# ssl.MemoryBIO from the static-linked _ssl module is included to make
# any divergence visible.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMP="$PROJECT_DIR/build/python.composed.wasm"

[ -f "$COMP" ] || { echo "test-ssl-capability: $COMP not found — run scripts/compose-python-component.sh first." >&2; exit 1; }
command -v wasmtime >/dev/null 2>&1 || { echo "test-ssl-capability: 'wasmtime' is required on PATH." >&2; exit 1; }

LIBDIR="$(basename "$(ls -d "$PROJECT_DIR"/deps/cpython/cross-build/wasm32-wasip2/build/lib.wasi-wasm32-* | head -1)")"

wasmtime run --wasm max-wasm-stack=16777216 \
    --dir "$PROJECT_DIR/deps/cpython::/" \
    --env "PYTHONPATH=/cross-build/wasm32-wasip2/build/$LIBDIR" \
    "$COMP" -c "
import sys
import _ssl_capability

failures = 0
def expect(cond, msg):
    global failures
    if cond:
        print(f'OK   : {msg}')
    else:
        print(f'FAIL : {msg}')
        failures += 1

print('--- Phase 3b.1: scaffold + openssl-component imports ---')
# probe_imports() was removed in 3b.3; the import is now kept alive by real
# consumers (_SSLContext/_SSLSocket -> openssl:component/tls). The module
# loading at all + the type checks below cover the scaffold gate.
expect(_ssl_capability.MemoryBIO is not None, 'module + types load (3b.1 + 3b.2 wired)')

print('--- Phase 3b.2: MemoryBIO semantics ---')
bio = _ssl_capability.MemoryBIO()
expect(bio.pending == 0, 'fresh BIO empty')
expect(bio.eof is False, 'fresh BIO not at EOF')
expect(bio.read() == b'', 'read empty returns b\"\"')

n = bio.write(b'hello')
expect(n == 5, 'write returns byte count')
expect(bio.pending == 5, 'pending after write')
expect(bio.read(3) == b'hel', 'partial read')
expect(bio.read() == b'lo', 'drain rest')
expect(bio.pending == 0, 'pending == 0 after drain')

bio.write(b'tail'); bio.write_eof()
expect(bio.eof is False, 'EOF not until drained')
expect(bio.read() == b'tail', 'drain tail')
expect(bio.eof is True, 'EOF after drain')

try:
    bio.write(b'x'); expect(False, 'write after EOF raises')
except OSError as e:
    expect('write_eof' in str(e), 'OSError on write-after-EOF mentions write_eof')

print('--- Phase 3b.2: MemoryBIO FIFO vs bytearray reference (100 iter) ---')
bio_cap = _ssl_capability.MemoryBIO()
ref = bytearray()
mismatches = 0
for i in range(100):
    chunk = bytes([i & 0xff]) * 1024
    bio_cap.write(chunk); ref.extend(chunk)
    r_cap = bio_cap.read(512); r_ref = bytes(ref[:512]); del ref[:512]
    if r_cap != r_ref:
        mismatches += 1
expect(mismatches == 0, f'100 pump iters byte-equal vs reference ({mismatches} mismatches)')
final_cap = bio_cap.read(); final_ref = bytes(ref); ref.clear()
expect(final_cap == final_ref, f'final drain matches ({len(final_cap)} bytes)')

print('--- Phase 3b.2: MemoryBIO buffer-protocol inputs ---')
b = _ssl_capability.MemoryBIO()
b.write(bytearray(b'xyz'))
expect(b.read() == b'xyz', 'write(bytearray)')
b.write(memoryview(b'abc'))
expect(b.read() == b'abc', 'write(memoryview)')

# Cross-check against CPython's own ssl.MemoryBIO (currently from static _ssl)
print('--- Phase 3b.2: vs stdlib ssl.MemoryBIO (semantic parity) ---')
try:
    import ssl
    cap = _ssl_capability.MemoryBIO()
    std = ssl.MemoryBIO()
    for op in [
        ('write', b'hello'),
        ('read', 3),
        ('write', b'world'),
        ('read', None),
        ('write_eof', None),
        ('read', None),
    ]:
        if op[0] == 'write':
            r1 = cap.write(op[1]); r2 = std.write(op[1])
        elif op[0] == 'read':
            r1 = cap.read() if op[1] is None else cap.read(op[1])
            r2 = std.read() if op[1] is None else std.read(op[1])
        else:
            cap.write_eof(); std.write_eof(); r1 = r2 = None
        expect(r1 == r2, f'op {op}: cap={r1!r} std={r2!r}')
    expect(cap.eof == std.eof, 'final eof state matches stdlib')
except ImportError:
    print('SKIP : stdlib ssl unavailable in this build')

print('--- Phase 3b.3: _SSLContext + _SSLSocket (no network) ---')
# Type wiring + config shape. The actual TLS handshake requires real network
# (gated separately, see test-ssl-network).
expect(_ssl_capability._SSLContext is not None, '_SSLContext registered')
expect(_ssl_capability._SSLSocket is not None, '_SSLSocket registered')
expect(_ssl_capability.SSLError is not None, 'SSLError registered')
expect(_ssl_capability.CERT_REQUIRED == 2, 'CERT_REQUIRED == 2')
expect(_ssl_capability.CERT_NONE == 0, 'CERT_NONE == 0')

ctx = _ssl_capability._SSLContext()
expect(ctx.verify_mode == _ssl_capability.CERT_REQUIRED, 'default verify_mode is CERT_REQUIRED')
ctx.verify_mode = _ssl_capability.CERT_NONE
expect(ctx.verify_mode == _ssl_capability.CERT_NONE, 'verify_mode setter works')

# ALPN list accepts both str and bytes; coerced to bytes internally
ctx.set_alpn_protocols(['h2', 'http/1.1'])
ctx.set_alpn_protocols([b'h2', b'http/1.1'])
expect(True, 'set_alpn_protocols(str + bytes) accepted')

# CA / client-cert setters take bytes (not paths) in v1
ctx.set_ca_certs(b'(pretend PEM blob)')
ctx.set_client_cert(b'(cert pem)', b'(key pem)')
expect(True, 'set_ca_certs / set_client_cert accepted (bytes-only v1)')

print('---')
print('PASS' if failures == 0 else f'{failures} FAILURES')
sys.exit(failures)
" \
    && echo "OK: _ssl_capability Phase 3b.2 + 3b.3 (no-network) end-to-end." \
    || { echo "FAIL: _ssl_capability test broke." >&2; exit 1; }

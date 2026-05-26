#!/usr/bin/env bash
# Componentize-python plan, Phase 1: end-to-end smoke test of the composed
# python.composed.wasm + _compression extension + compression-multiplexer.
#
# Runs the composed component under wasmtime and exercises:
#   - import _compression
#   - deflate_raw / inflate_raw roundtrip via the multiplexer
#   - cross-compatibility with the static zlib (each can read the other's raw stream)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMP="$PROJECT_DIR/build/python.composed.wasm"

[ -f "$COMP" ] || { echo "test-compression-extension: $COMP not found — run scripts/compose-python-component.sh first." >&2; exit 1; }
command -v wasmtime >/dev/null 2>&1 || { echo "test-compression-extension: 'wasmtime' is required on PATH." >&2; exit 1; }

LIBDIR="$(basename "$(ls -d "$PROJECT_DIR"/deps/cpython/cross-build/wasm32-wasip2/build/lib.wasi-wasm32-* | head -1)")"

wasmtime run --wasm max-wasm-stack=16777216 \
    --dir "$PROJECT_DIR/deps/cpython::/" \
    --env "PYTHONPATH=/cross-build/wasm32-wasip2/build/$LIBDIR" \
    "$COMP" -c "
import sys, zlib, _compression

failures = 0
data = b'The quick brown fox jumps over the lazy dog. ' * 50

# 1) _compression round-trip via the multiplexer
c = _compression.deflate_raw(data, 6)
r = _compression.inflate_raw(c)
print('roundtrip via multiplexer    :', 'OK' if r == data else 'FAIL')
if r != data: failures += 1

# 2) zlib (static libz) can decompress _compression's output (raw DEFLATE)
zr = zlib.decompress(c, -15)
print('static zlib reads multiplexer:', 'OK' if zr == data else 'FAIL')
if zr != data: failures += 1

# 3) _compression can decompress static zlib's raw DEFLATE stream
z = zlib.compress(data, 6)
raw_from_z = z[2:-4]   # strip rfc1950 header + adler32 trailer
cr = _compression.inflate_raw(raw_from_z)
print('multiplexer reads static zlib:', 'OK' if cr == data else 'FAIL')
if cr != data: failures += 1

# 4) Empty / small / boundary cases
for sample in (b'', b'a', b'\x00' * 1024, bytes(range(256)) * 4):
    out = _compression.inflate_raw(_compression.deflate_raw(sample, 6))
    if out != sample:
        print(f'boundary case len={len(sample)}: FAIL')
        failures += 1

sys.exit(failures)
" \
    && echo "OK: _compression + compression-multiplexer end-to-end through python.composed.wasm." \
    || { echo "FAIL: extension or composition broken." >&2; exit 1; }

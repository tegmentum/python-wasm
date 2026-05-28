#!/usr/bin/env bash
# Componentize-python plan, Phase 1: end-to-end smoke test of the composed
# python.composed.wasm + _compress_cap extension + compression-multiplexer.
#
# Runs the composed component under wasmtime and exercises:
#   - import _compress_cap
#   - deflate_raw / inflate_raw roundtrip via the multiplexer
#   - cross-compatibility with the static zlib (each can read the other's raw stream)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PROFILE="${PROFILE:-default}"
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"
CPYTHON_DIR="$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR"
COMP="$PROJECT_DIR/$BUILD_DIR/python.composed.wasm"

[ -f "$COMP" ] || { echo "test-compression-extension: $COMP not found — run scripts/compose-python-component.sh first." >&2; exit 1; }
command -v wasmtime >/dev/null 2>&1 || { echo "test-compression-extension: 'wasmtime' is required on PATH." >&2; exit 1; }

LIBDIR="$(basename "$(ls -d "$CPYTHON_DIR"/cross-build/"$HOST_TRIPLE"/build/lib.wasi-wasm32-* | head -1)")"

wasmtime run --wasm max-wasm-stack=16777216 \
    --dir "$CPYTHON_DIR::/" \
    --env "PYTHONPATH=/cross-build/wasm32-wasip2/build/$LIBDIR" \
    "$COMP" -c "
import sys, zlib, _compress_cap

failures = 0
data = b'The quick brown fox jumps over the lazy dog. ' * 50

# 1) _compress_cap round-trip via the multiplexer
c = _compress_cap.deflate_raw(data, 6)
r = _compress_cap.inflate_raw(c)
print('roundtrip via multiplexer    :', 'OK' if r == data else 'FAIL')
if r != data: failures += 1

# 2) zlib (static libz) can decompress _compress_cap's output (raw DEFLATE)
zr = zlib.decompress(c, -15)
print('static zlib reads multiplexer:', 'OK' if zr == data else 'FAIL')
if zr != data: failures += 1

# 3) _compress_cap can decompress static zlib's raw DEFLATE stream
z = zlib.compress(data, 6)
raw_from_z = z[2:-4]   # strip rfc1950 header + adler32 trailer
cr = _compress_cap.inflate_raw(raw_from_z)
print('multiplexer reads static zlib:', 'OK' if cr == data else 'FAIL')
if cr != data: failures += 1

# 4) Empty / small / boundary cases
for sample in (b'', b'a', b'\x00' * 1024, bytes(range(256)) * 4):
    out = _compress_cap.inflate_raw(_compress_cap.deflate_raw(sample, 6))
    if out != sample:
        print(f'boundary case len={len(sample)}: FAIL')
        failures += 1

sys.exit(failures)
" \
    && echo "OK: _compress_cap + compression-multiplexer end-to-end through python.composed.wasm." \
    || { echo "FAIL: extension or composition broken." >&2; exit 1; }

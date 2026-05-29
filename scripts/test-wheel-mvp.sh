#!/usr/bin/env bash
# Phase C3 of docs/pip-wheel-wrapper-plan.md — end-to-end MVP demo.
#
#   1. Build a stripped python.composed.wasm WITHOUT _zstd_cap.
#   2. Stage the zstd_cap wheel into a temp site-packages dir.
#   3. Run python-wasm-rebuild.sh against that site-packages.
#   4. Verify the rebuilt composed wasm has _zstd_cap importable.
#
# Exit criterion for the MVP: a user without _zstd_cap in their base
# build can pip install zstd-cap → python-wasm rebuild → import succeeds.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

WHEEL="$PROJECT_DIR/dist/wheels/zstd_cap-0.1.0-cp314-cp314-wasm32_wasip2.whl"
[ -f "$WHEEL" ] || { echo "test-wheel-mvp: $WHEEL missing — run scripts/build-extension-wheel.sh first." >&2; exit 1; }

# Use a temp PYTHON_WASM_HOME so we don't disturb the real ~/.python-wasm.
TMP_HOME="$(mktemp -d -t pyforge-mvp-XXXXXX)"
trap 'rm -rf "$TMP_HOME"' EXIT
export PYTHON_WASM_HOME="$TMP_HOME"
mkdir -p "$TMP_HOME/site-packages"

# --- Step 1: build minimal artifact WITHOUT _zstd_cap, and as a sanity
#     check, prove that the resulting composed wasm fails to import
#     _zstd_cap.
echo "==> step 1: build base python.composed.wasm WITHOUT _zstd_cap"
timeout 600 "$SCRIPT_DIR/build-from-pkgs.sh" --variant minimal --exclude zstd-cap >/dev/null 2>&1
COMP_MINIMAL="$PROJECT_DIR/build/default-minimal/python.composed.wasm"
[ -f "$COMP_MINIMAL" ] || { echo "test-wheel-mvp: minimal build did not land at $COMP_MINIMAL" >&2; exit 1; }
cp "$COMP_MINIMAL" "$TMP_HOME/python.composed.wasm"

eval "$(bash "$SCRIPT_DIR/load-profile.sh" default)"
CPYTHON_DIR="$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR"
LIBDIR="$(basename "$(ls -d "$CPYTHON_DIR"/cross-build/"$HOST_TRIPLE"/build/lib.wasi-wasm32-* | head -1)")"
SYSCONF="/cross-build/$HOST_TRIPLE/build/$LIBDIR"

echo "==> verify _zstd_cap is NOT in the minimal build"
if wasmtime run --wasm max-wasm-stack=16777216 \
    --dir "$CPYTHON_DIR::/" \
    --env "PYTHONPATH=$SYSCONF:/Lib" \
    "$TMP_HOME/python.composed.wasm" \
    -c "import _zstd_cap" 2>/dev/null; then
    echo "test-wheel-mvp: minimal build still has _zstd_cap; exclude didn't work." >&2; exit 1
fi
echo "  + minimal build raises ImportError on _zstd_cap (expected)"

# --- Step 2: simulate `pip install --target $PYTHON_WASM_HOME/site-packages zstd-cap`.
#     Pip-on-the-host can't actually install a wasm32_wasip2 wheel (no
#     matching tag for darwin), so we unpack the wheel directly. The
#     end result on disk is identical — the wheel format is plain zip.
echo "==> step 2: stage zstd-cap wheel into $TMP_HOME/site-packages"
unzip -q "$WHEEL" -d "$TMP_HOME/site-packages"
ls "$TMP_HOME/site-packages/" | head -5

# --- Step 3: rebuild with the staged wheel picked up. The rebuild
#     script discovers it via the pyforge-pkg.toml glob.
echo "==> step 3: python-wasm-rebuild.sh"
PYTHON_WASM_HOME="$TMP_HOME" PROFILE=default timeout 600 \
    "$SCRIPT_DIR/python-wasm-rebuild.sh" 2>&1 | tail -5

# --- Step 4: prove _zstd_cap is now importable + works for a real
#     roundtrip.
echo "==> step 4: verify _zstd_cap roundtrip in rebuilt composed wasm"
wasmtime run --wasm max-wasm-stack=16777216 \
    --dir "$CPYTHON_DIR::/" \
    --env "PYTHONPATH=$SYSCONF:/Lib" \
    "$TMP_HOME/python.composed.wasm" \
    -c "
import _zstd_cap
data = b'hello python-wasm extension wheel' * 100
out = _zstd_cap.zstd_decompress(_zstd_cap.zstd_compress(data, 3))
assert out == data, 'roundtrip mismatch'
print('OK: _zstd_cap roundtripped', len(data), 'bytes via wheel-installed extension')
"

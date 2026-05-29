#!/usr/bin/env bash
# Phase 4 of docs/coverage-implementation-plan.md — end-to-end demo of the
# py-offload boundary. Runs a numpy call from inside python.composed.wasm and
# proves it round-trips through a host-side native CPython worker.
#
# Tier 1 today = SubprocessClient-replacing-MailboxClient (file-mailbox
# transport). Tier 1 future = v86 wasmmachine guest. The python-wasm guest
# code is unchanged across the two backends — that's the point of the WIT
# contract.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PROFILE="${PROFILE:-default}"
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"
CPYTHON_DIR="$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR"
COMP="$PROJECT_DIR/$BUILD_DIR/python.composed.wasm"
REF="$PROJECT_DIR/reference-worker"

[ -f "$COMP" ] || { echo "test-offload-numpy: $COMP not found — run 'make python-composed' first." >&2; exit 1; }
[ -d "$REF" ] || { echo "test-offload-numpy: reference-worker/ missing." >&2; exit 1; }
command -v wasmtime >/dev/null 2>&1 || { echo "test-offload-numpy: 'wasmtime' is required on PATH." >&2; exit 1; }

# numpy is a hard requirement of the test; check the host has it before
# spinning up the wasm guest (otherwise the worker's `import numpy` fails
# silently and the guest times out).
python3 -c "import numpy" 2>/dev/null \
    || { echo "test-offload-numpy: host python3 needs numpy installed (pip install numpy)." >&2; exit 1; }

LIBDIR="$(basename "$(ls -d "$CPYTHON_DIR"/cross-build/"$HOST_TRIPLE"/build/lib.wasi-wasm32-* | head -1)")"
SYSCONF="/cross-build/$HOST_TRIPLE/build/$LIBDIR"

RT="$(mktemp -d -t pwasm-offload-XXXXXX)"
WORKER=""
cleanup() {
    [ -n "$WORKER" ] && kill "$WORKER" 2>/dev/null || true
    rm -rf "$RT"
}
trap cleanup EXIT

mkdir -p "$RT/mailbox"

# Host-side worker: serve_mailbox loops over the shared dir; reference-worker's
# worker.run() resolves "numpy.linalg:svd" against the host's numpy install.
PYTHONPATH="$REF" python3 -c "
from py_offload.mailbox import serve_mailbox
serve_mailbox('$RT/mailbox')
" &
WORKER=$!

# Brief grace for the worker to enter its poll loop.
sleep 0.3

# Guest script: import numpy through the offload boundary, run a calc,
# assert the answer.
cat > "$RT/guest.py" <<'PYEOF'
import os

# Sanity: importhook should be installed by sitecustomize when env is set.
assert os.environ.get("OFFLOAD_MAILBOX_DIR"), "missing OFFLOAD_MAILBOX_DIR"
assert "numpy" in os.environ.get("OFFLOAD_PACKAGES", ""), "numpy not listed in OFFLOAD_PACKAGES"

# Direct invocation through the proxy — every attribute access returns a _Call;
# calling it ships (entry, args, kwargs) over the mailbox.
#
# Codec note: Phase 1 msgpack only handles primitive types — ndarrays don't
# cross the wire. So the demo picks numpy entry points whose return values
# are Python scalars (float, complex), which msgpack handles cleanly. arrow
# (Phase 3 in the native-exec plan) is what ndarrays will ride on.
import numpy

# A scalar from a linear-algebra call. The matrix is sent as a list of lists
# (msgpack-friendly); the determinant comes back as a numpy.float64 which
# msgpack treats as a float.
det = numpy.linalg.det([[1.0, 2.0], [3.0, 4.0]])
print(f"det([[1,2],[3,4]]) = {det}")
assert abs(det - -2.0) < 1e-9, f"unexpected determinant: {det!r}"

# A whole-API probe: numpy attribute on the proxy module → callable proxy.
norm = numpy.linalg.norm([3.0, 4.0])
print(f"norm([3,4]) = {norm}")
assert abs(norm - 5.0) < 1e-9, f"unexpected norm: {norm!r}"

# Top-level numpy.sum on a vector.
total = numpy.sum([1.0, 2.0, 3.0, 4.0])
print(f"numpy.sum([1,2,3,4]) = {total}")
assert abs(total - 10.0) < 1e-9, f"unexpected sum: {total!r}"

print("OK: 3 numpy round-trips through offload boundary")
PYEOF

# Guest invocation: pre-existing run-python.sh handles the standard mounts;
# we add the mailbox mount + the env vars sitecustomize watches for.
wasmtime run \
    --wasm max-wasm-stack=16777216 \
    --dir "$CPYTHON_DIR::/" \
    --dir "$RT::/work" \
    --env "PYTHONPATH=$SYSCONF:/Lib" \
    --env "OFFLOAD_MAILBOX_DIR=/work/mailbox" \
    --env "OFFLOAD_PACKAGES=numpy" \
    "$COMP" /work/guest.py

#!/usr/bin/env bash
# End-to-end py-offload demo with the REAL wasip2 interpreter.
#
# A wasip2 python.wasm imports `nativelib` — a package NOT built for wasip2 —
# through the py-offload import hook, which routes its calls over a file mailbox
# to a host worker (serve_mailbox) that runs nativelib on a host interpreter.
# This proves the offload contract executing in the actual target interpreter
# (the Tier-1 host/native-worker shape), not just on host CPython.
#
# Usage: reference-worker/run-wasip2-offload.sh
set -euo pipefail

REF="$(cd "$(dirname "$0")" && pwd)"
PW="$(cd "$REF/.." && pwd)"
PYWASM="$PW/deps/cpython/cross-build/wasm32-wasip2/python.wasm"
if [ ! -f "$PYWASM" ]; then
    echo "python.wasm not found ($PYWASM) — run 'make' in python-wasm first." >&2
    exit 1
fi
LIBDIR="$(basename "$(ls -d "$PW"/deps/cpython/cross-build/wasm32-wasip2/build/lib.wasi-wasm32-* | head -1)")"
SYSCONF="/cross-build/wasm32-wasip2/build/$LIBDIR"

RT="$(mktemp -d)"
WORKER=""
cleanup() { [ -n "$WORKER" ] && kill "$WORKER" 2>/dev/null || true; rm -rf "$RT"; }
trap cleanup EXIT
mkdir -p "$RT/work/mailbox"

# Host worker: serve the mailbox; nativelib + py_offload on its path.
PYTHONPATH="$REF:$REF/examples" python3 -c \
  "from py_offload.mailbox import serve_mailbox; serve_mailbox('$RT/work/mailbox')" &
WORKER=$!
sleep 0.5

# Guest: run the demo inside python.wasm (py_offload mounted at /refworker; the
# mailbox shared at /work/mailbox).
wasmtime run --wasm max-wasm-stack=16777216 \
    --dir "$PW/deps/cpython::/" \
    --dir "$REF::/refworker" \
    --dir "$RT/work::/work" \
    --env "PYTHONPATH=$SYSCONF:/refworker" \
    "$PYWASM" /refworker/examples/offload_guest.py

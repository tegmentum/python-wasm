#!/usr/bin/env bash
# DEPRECATED — componentize-py is retired (see bindings/DEPRECATED.md).
# The Pattern A forge bakes _compression + Lib/zlib.py into python.composed.wasm
# directly; this runner is no longer the path. Kept for historical reference; not
# wired into CI. See docs/componentize-python.md.
#
# Build the py-runner: a wasi:cli/command-shaped CPython component (built with
# componentize-py) whose extra import is the compression-dispatcher capability.
# Composed with the compression-multiplexer via wac, the result is a runnable
# component that executes any Python script with `zlib` routed in-process
# through the wasm multiplexer.
#
# Output: runner.composed.wasm
set -euo pipefail
cd "$(dirname "$0")"
MUX="${COMPRESSION_MULTIPLEXER_WASM:-$HOME/git/compression-multiplexer/target/wasm32-wasip2/release/compression_multiplexer.wasm}"
[ -f "$MUX" ] || { echo "compression_multiplexer.wasm not found ($MUX)"; exit 1; }
echo "==> componentize-py app -> pyrunner.wasm"
componentize-py --wit-path wit --world runner componentize app -o pyrunner.wasm
echo "==> wac plug compression-multiplexer -> runner.composed.wasm"
wac plug pyrunner.wasm --plug "$MUX" -o runner.composed.wasm
echo "==> done: $(du -h runner.composed.wasm | cut -f1) runner.composed.wasm"

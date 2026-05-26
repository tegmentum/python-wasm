#!/usr/bin/env bash
# Stage a v86 workspace that boots Linux + a self-contained x86_64 musl CPython
# running `py_offload.mailbox` over a virtiofs file mailbox. Implements the
# recipe in docs/tier1-v86-integration.md.
#
# Usage:
#   reference-worker/tier1-v86/setup.sh <target-workspace-dir>
#
# Then run v86 with:
#   wasmtime run -S http -S inherit-network --dir <target>::/ \
#       --env V86_MODE=linux --env V86_VIRTIOFS_ROOT=1 v86.wasm
#
# v86's x86 emulator is interpreter-only (~doc §2.2): cold-booting Linux +
# busybox + python takes minutes. Use v86's snapshot/restore to skip cold boot
# once you've captured a worker-ready snapshot.
set -euo pipefail

WS="${1:?usage: $0 <target-workspace-dir>}"
REF="$(cd "$(dirname "$0")/.." && pwd)"
PBS_TAG="${PBS_TAG:-20260510}"
PBS_PY="${PBS_PY:-3.13.13}"
ALPINE="${ALPINE:-https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64/alpine-minirootfs-3.21.0-x86_64.tar.gz}"
PBS_URL="https://github.com/astral-sh/python-build-standalone/releases/download/${PBS_TAG}/cpython-${PBS_PY}+${PBS_TAG}-x86_64-unknown-linux-musl-install_only.tar.gz"

CACHE="${PBS_CACHE:-$HOME/.cache/tier1-v86}"
mkdir -p "$CACHE" "$WS/workspace/opt" "$WS/workspace/lib" "$WS/workspace/run/py-offload"

if [ ! -f "$CACHE/pbs.tar.gz" ]; then
    echo "==> fetch python-build-standalone musl ($PBS_PY+$PBS_TAG)"
    curl -fsSL -o "$CACHE/pbs.tar.gz" "$PBS_URL"
fi
if [ ! -f "$CACHE/alpine.tgz" ]; then
    echo "==> fetch alpine-minirootfs (for the musl loader at /lib/ld-musl-x86_64.so.1)"
    curl -fsSL -o "$CACHE/alpine.tgz" "$ALPINE"
fi

if [ ! -d "$WS/workspace/opt/python" ]; then
    echo "==> extract python -> $WS/workspace/opt/python"
    tar xzf "$CACHE/pbs.tar.gz" -C "$WS/workspace/opt"  # tarball contains `python/`
fi
if [ ! -f "$WS/workspace/lib/ld-musl-x86_64.so.1" ]; then
    echo "==> extract musl loader -> $WS/workspace/lib/"
    tar xzf "$CACHE/alpine.tgz" -C "$CACHE" --strip-components=0 ./lib/ld-musl-x86_64.so.1
    cp "$CACHE/lib/ld-musl-x86_64.so.1" "$WS/workspace/lib/"
    ln -sf ld-musl-x86_64.so.1 "$WS/workspace/lib/libc.musl-x86_64.so.1"
fi

echo "==> stage py_offload -> $WS/workspace/opt/py_offload"
rsync -a --delete "$REF/py_offload/" "$WS/workspace/opt/py_offload/"

echo "==> install init from template"
install -m 0755 "$(dirname "$0")/init.template" "$WS/workspace/init"

echo "==> done. Workspace size: $(du -sh "$WS/workspace" | cut -f1)"
echo "Next: boot v86 with --dir $WS::/ (provide v86's /assets/ at the same mount)."

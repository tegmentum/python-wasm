#!/usr/bin/env bash
# Componentize-python plan, Tier-1 v86: end-to-end Phase 1 round-trip
# against the REAL v86-posix-host (not the stub).
#
# The v86 guest is simulated here by running the in-guest helper script
# (workspace/posix-helper.sh from the v86 repo) directly on the host
# with a shared mailbox dir mounted into the wasmtime invocation.
# Functionally equivalent to a live v86 boot: the v86 emulator's only
# role for spawn is to expose the mailbox dir to the guest via virtiofs,
# which is just a host directory; running the helper as a host process
# exercises the identical file-IPC path.
#
# What this proves:
#   * Recomposed python.composed.wasm with V86_POSIX_COMPONENT_WASM =
#     v86-posix-host (not the default stub) instantiates cleanly.
#   * subprocess.run() / Popen.wait() / check_call() / check_output()
#     reach the helper, get real exit statuses back, and map them onto
#     the right stdlib exception types.
#   * Signal-killed children surface as returncode = -signum per stdlib.
#   * Missing executables map to FileNotFoundError(ENOENT).
#
# Prerequisites:
#   * ~/git/v86 built: cargo build --release --target wasm32-wasip2 \
#         -p v86-posix-host
#   * python-wasm built: make build (composed comes from this script).
#   * bash on PATH (helper script is busybox-compatible POSIX sh; bash
#     is fine for the host simulation here).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
V86_REPO="${V86_REPO:-$HOME/git/v86}"
HOST_WASM="${V86_POSIX_COMPONENT_WASM:-$V86_REPO/target/wasm32-wasip2/release/v86_posix_host.wasm}"
HELPER="${POSIX_HELPER_SH:-$V86_REPO/crates/v86-posix-host/workspace/posix-helper.sh}"
COMP="$PROJECT_DIR/build/python.composed.wasm"

[ -f "$HOST_WASM" ] || { echo "test-v86-posix-roundtrip: $HOST_WASM not found — build v86-posix-host first." >&2; exit 1; }
[ -f "$HELPER" ]   || { echo "test-v86-posix-roundtrip: $HELPER not found." >&2; exit 1; }

# Recompose python.wasm with the real host component plugged in. The
# default compose-python-component.sh plugs the stub; override here so
# the rest of the run hits the real surface.
echo "==> recomposing with V86_POSIX_COMPONENT_WASM=$HOST_WASM"
V86_POSIX_COMPONENT_WASM="$HOST_WASM" \
    bash "$PROJECT_DIR/scripts/compose-python-component.sh" >/dev/null

# Per-run temp mailbox; cleaned up on exit.
SHARED="$(mktemp -d -t v86-spawn-XXXXXX)"
mkdir -p "$SHARED/posix-mailbox"
cleanup() {
    [ -n "${HELPER_PID:-}" ] && kill "$HELPER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$SHARED"
}
trap cleanup EXIT

# Start the helper in the background — the simulated guest.
POSIX_MAILBOX="$SHARED/posix-mailbox" \
POSIX_HELPER_LOG="$SHARED/helper.log" \
    bash "$HELPER" &
HELPER_PID=$!
# Give the helper a tick to enter its poll loop before the wasm starts
# writing requests.
sleep 0.2

# Pick binaries that exist on the host. macOS doesn't ship /bin/true;
# /usr/bin/true does. Linux usually has /bin/true too — prefer it if
# present so the test surfaces shape-issues equally on both.
TRUE_BIN=$([ -x /bin/true ] && echo /bin/true || echo /usr/bin/true)
FALSE_BIN=$([ -x /bin/false ] && echo /bin/false || echo /usr/bin/false)

LIBDIR="$(basename "$(ls -d "$PROJECT_DIR"/deps/cpython/cross-build/wasm32-wasip2/build/lib.wasi-wasm32-* | head -1)")"

wasmtime run --wasm max-wasm-stack=16777216 \
    --dir "$PROJECT_DIR/deps/cpython::/" \
    --dir "$SHARED::/workspace" \
    --env "PYTHONPATH=/cross-build/wasm32-wasip2/build/$LIBDIR" \
    --env "TRUE_BIN=$TRUE_BIN" \
    --env "FALSE_BIN=$FALSE_BIN" \
    --env "SHARED_DIR=$SHARED" \
    "$COMP" -c "
import os, subprocess, sys, errno

TRUE = os.environ['TRUE_BIN']
FALSE = os.environ['FALSE_BIN']

failures = 0

def expect(label, got, want):
    global failures
    if got == want:
        print(f'{label}: OK ({got!r})')
    else:
        print(f'{label}: FAIL — got {got!r}, want {want!r}')
        failures += 1

# 1) returncode 0 from a real binary
expect('subprocess.run(true).returncode',
       subprocess.run([TRUE]).returncode, 0)

# 2) returncode 1 from /usr/bin/false
expect('subprocess.run(false).returncode',
       subprocess.run([FALSE]).returncode, 1)

# 3) custom exit code via /bin/sh
expect('sh -c exit-42',
       subprocess.run(['/bin/sh', '-c', 'exit 42']).returncode, 42)

# 4) signal-killed → returncode = -signum per stdlib
expect('sh -c kill-self-with-9',
       subprocess.run(['/bin/sh', '-c', 'kill -9 \$\$']).returncode, -9)

# 5) missing executable → FileNotFoundError
try:
    subprocess.run(['/nonexistent/binary'])
    print('missing-exec: FAIL — no exception'); failures += 1
except FileNotFoundError as e:
    if e.errno == errno.ENOENT:
        print(f'missing-exec → FileNotFoundError(ENOENT): OK')
    else:
        print(f'missing-exec: FAIL — errno {e.errno}'); failures += 1

# 6) check_call success returns 0
expect('check_call(true)',
       subprocess.check_call([TRUE]), 0)

# 7) check_call failure raises CalledProcessError
try:
    subprocess.check_call([FALSE])
    print('check_call(false): FAIL — no exception'); failures += 1
except subprocess.CalledProcessError as e:
    if e.returncode == 1:
        print(f'check_call(false) → CalledProcessError(returncode=1): OK')
    else:
        print(f'check_call(false): FAIL — returncode {e.returncode}'); failures += 1

# 8) Popen context manager
with subprocess.Popen([TRUE]) as p:
    rc = p.wait()
expect('Popen(...).wait()', rc, 0)

# 9) env + cwd propagate
# Write to a path under /workspace (host: \$SHARED) which is shared
# read-write between this wasm sandbox and the host where the helper
# runs the child. The wasm sandbox's own /tmp would work for the host
# child but isn't readable from python's side, so we use the shared dir.
import pathlib
out_guest_path = '/workspace/posix-mailbox/.envcwd-test'
out_host_path  = os.environ['SHARED_DIR'] + '/posix-mailbox/.envcwd-test'
rc = subprocess.run(['/bin/sh', '-c',
                     f'echo \$MARKER > {out_host_path}; pwd >> {out_host_path}'],
                    env={'MARKER': 'env-was-here'},
                    cwd='/tmp').returncode
expect('env+cwd run.returncode', rc, 0)
contents = pathlib.Path(out_guest_path).read_text().splitlines()
expect('env propagated', contents[0], 'env-was-here')
expect('cwd propagated', contents[1], '/tmp')

print()
print(f'{failures} failure(s)')
sys.exit(failures)
" \
    && echo "OK: _v86_posix + v86-posix-host + helper round-trip end-to-end." \
    || { echo "FAIL: round-trip broken — see helper log at $SHARED/helper.log" >&2; cat "$SHARED/helper.log" >&2; exit 1; }

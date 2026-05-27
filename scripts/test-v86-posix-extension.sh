#!/usr/bin/env bash
# Componentize-python plan, Tier-1 v86: end-to-end smoke test of the composed
# python.composed.wasm + _v86_posix extension + v86-posix-stub.
#
# Runs the composed component under wasmtime and exercises:
#   - import _v86_posix (proves the extension is statically linked + built)
#   - stdio constants present (STDIO_INHERIT / STDIO_PIPED / STDIO_NULL)
#   - exception hierarchy: SpawnError / GuestNotReadyError / SignalError / etc.
#     are subclassed off OSError as documented
#   - _v86_posix.spawn(...) raises GuestNotReadyError (the stub's contract)
#   - the raised exception's chain is correct (isinstance SpawnError, OSError)
#
# This is the moral equivalent of test-compression-extension.sh /
# test-hash-extensions.sh — it proves the wiring is sound. When the real
# v86-component impl replaces the stub the spawn path will start succeeding
# and this test will need an env-gated "guest is up" variant; today, the
# guest-not-ready path IS the contract.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMP="$PROJECT_DIR/build/python.composed.wasm"

[ -f "$COMP" ] || { echo "test-v86-posix-extension: $COMP not found — run scripts/compose-python-component.sh first." >&2; exit 1; }
command -v wasmtime >/dev/null 2>&1 || { echo "test-v86-posix-extension: 'wasmtime' is required on PATH." >&2; exit 1; }

LIBDIR="$(basename "$(ls -d "$PROJECT_DIR"/deps/cpython/cross-build/wasm32-wasip2/build/lib.wasi-wasm32-* | head -1)")"

wasmtime run --wasm max-wasm-stack=16777216 \
    --dir "$PROJECT_DIR/deps/cpython::/" \
    --env "PYTHONPATH=/cross-build/wasm32-wasip2/build/$LIBDIR" \
    "$COMP" -c "
import sys, _v86_posix

failures = 0

# 1) Module surface — constants
for name in ('STDIO_INHERIT', 'STDIO_PIPED', 'STDIO_NULL'):
    if not hasattr(_v86_posix, name):
        print(f'missing constant: {name}: FAIL'); failures += 1
expected_stdio = {'STDIO_INHERIT': 0, 'STDIO_PIPED': 1, 'STDIO_NULL': 2}
for k, v in expected_stdio.items():
    got = getattr(_v86_posix, k, None)
    if got != v:
        print(f'{k} == {got}, expected {v}: FAIL'); failures += 1
print('module constants             :', 'OK' if failures == 0 else 'FAIL')

# 2) Exception hierarchy — every error should subclass OSError per the WIT
#    docstring and the README in cpython-ext/_v86_posix/
ex_tree = {
    'SpawnError'           : OSError,
    'ProgramNotFoundError' : '_v86_posix.SpawnError',
    'ExecFailedError'      : '_v86_posix.SpawnError',
    'TooManyProcessesError': '_v86_posix.SpawnError',
    'InvalidArgumentError' : '_v86_posix.SpawnError',
    'GuestNotReadyError'   : '_v86_posix.SpawnError',
    'SignalError'          : OSError,
    'NoSuchProcessError'   : '_v86_posix.SignalError',
    'InvalidSignalError'   : '_v86_posix.SignalError',
}
hierarchy_failures = 0
for name, parent in ex_tree.items():
    cls = getattr(_v86_posix, name, None)
    if cls is None:
        print(f'missing exception: {name}: FAIL'); hierarchy_failures += 1; continue
    parent_cls = parent if isinstance(parent, type) else getattr(_v86_posix, parent.split('.')[-1], None)
    if parent_cls is None or not issubclass(cls, parent_cls):
        print(f'{name} not a subclass of {parent}: FAIL'); hierarchy_failures += 1
print('exception hierarchy          :', 'OK' if hierarchy_failures == 0 else 'FAIL')
failures += hierarchy_failures

# 3) spawn(...) routes to v86-posix-stub which always returns guest-not-ready
try:
    proc = _v86_posix.spawn('/bin/true', [])
    print(f'spawn returned without raising: {proc!r}: FAIL'); failures += 1
except _v86_posix.GuestNotReadyError as e:
    print('spawn -> GuestNotReadyError  :', 'OK')
    if not isinstance(e, _v86_posix.SpawnError):
        print('  not also SpawnError       : FAIL'); failures += 1
    if not isinstance(e, OSError):
        print('  not also OSError          : FAIL'); failures += 1
except _v86_posix.SpawnError as e:
    print(f'spawn raised SpawnError but NOT GuestNotReadyError: {type(e).__name__}: FAIL')
    failures += 1
except Exception as e:
    print(f'spawn raised unexpected {type(e).__name__}: {e}: FAIL'); failures += 1

# 4) Spawn argument validation — wrong types should fail before the WIT call
arg_failures = 0
for bad in [(123, []), ('/bin/sh', 'not-a-list'), ('/bin/sh', [123])]:
    try:
        _v86_posix.spawn(*bad)
        print(f'bad args {bad!r} accepted: FAIL'); arg_failures += 1
    except TypeError:
        pass
    except _v86_posix.GuestNotReadyError:
        print(f'bad args {bad!r} reached the cap (not validated client-side): FAIL'); arg_failures += 1
print('argument validation          :', 'OK' if arg_failures == 0 else 'FAIL')
failures += arg_failures

# Final tally
sys.exit(failures)
" \
    && echo "OK: _v86_posix + v86-posix-stub end-to-end through python.composed.wasm." \
    || { echo "FAIL: extension or composition broken." >&2; exit 1; }

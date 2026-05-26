# Tier 1 (v86) integration — **superseded**

The model previously documented in this file — *"run a native x86 CPython
executing `py_offload.serve` inside a v86 guest, driven from the host over a
virtiofs file mailbox"* — has been retired. The reference scaffolding it
described (`reference-worker/tier1-v86/setup.sh`, `init.template`, `README.md`)
is removed in the same commit as this note.

## Why it was wrong

Two reasons converged:

1. **It targeted the wrong execution boundary.** v86's role is to be the thin
   POSIX surface python-wasm calls into for things WASI can't express
   (fork/exec, `/proc`, signals, ptrace, …) and to host real Linux processes
   when something genuinely needs its own address space (subprocess.Popen, an
   unported C extension dlopen'd into a hosted process). Putting a *second*
   resident CPython inside the guest to re-serve `py_offload` requests inverted
   that — the guest-resident Python wasn't running user code, it was a relay,
   and the relay had to exist because the boundary was placed at "RPC into v86"
   instead of "POSIX-extension WIT into v86."

2. **The mechanics didn't work anyway.** v86 emulates **x86-32 only** (upstream
   README: "64-bit kernels are not supported"). python-build-standalone dropped
   i686 musl builds long ago — they ship only `x86_64`, `x86_64_v2..v4`, and
   `aarch64`. The PBS python the recipe staged in `opt/python/` never executed
   in the guest: the kernel returned ENOEXEC on the 64-bit ELF and busybox sh
   fell back to shell-interpreting the binary (`/opt/python/bin/python3: line 1: …`
   in `/var/log/py-offload`). The `py-offload-ready` signal was a false positive
   from `kill -0` succeeding against the post-exec-failure zombie. Even with a
   32-bit Python in place, the model would still have been wrong.

## The replacement direction (placeholder — full doc TBD)

python-wasm is now built as a wasm component. The correct shape is component
composition rather than guest-resident relaying:

- **python-wasm (component)** — *the* Python. Runs in wasmtime. Imports a
  POSIX-extension WIT for anything WASI can't give it.
- **v86 (component)** — the thin POSIX surface. Exports that WIT, backed by a
  real Linux kernel inside the guest. Two modes of use:
  - **Cheap syscall-style calls** for things python-wasm can express inline
    (fork/exec, signals, `/proc` reads, …).
  - **Hosted-process spawning** for things that need their own address space
    (subprocesses, dlopen of native `.so`s).
- They **compose**; v86 doesn't host its own Python.

A new design doc will replace this file once the WIT surface and the
component-composition wiring are pinned down.

## What survives from the old model

The transport pieces in `reference-worker/py_offload/{protocol,worker,mailbox,types,codecs,...}.py`
and their tests are **not** v86-specific and are unaffected. The file-mailbox in
`mailbox.py` is a generic transport that can be used wherever a directory-as-channel
makes sense; it just isn't the v86 boundary anymore.

The v86-component fix to preserve a user-supplied `workspace/init`
(`~/git/v86` commit `b62afbc`) remains valid as a general bug-fix — silently
overwriting user files is wrong under any architecture.

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

## The replacement direction

python-wasm is now built as a wasm component. The correct shape is component
composition rather than guest-resident relaying:

- **python-wasm (component)** — *the* Python. Runs in wasmtime. Imports a
  POSIX-extension WIT for anything WASI can't give it.
- **v86 (component)** — the thin POSIX surface. Exports that WIT, backed by a
  real Linux kernel inside the guest. v86's `compute.rs` doctrine spells it
  out: "Never route compute through x86 unless required for compatibility."
- They **compose** (composectl); v86 doesn't host its own Python.

### v0.1.0 — `v86:posix/process`

The first concrete piece is in the v86 repo at `wit/posix.wit`
(see also `docs/posix-extension-wit.md` there). It exports a single
interface, `process`, with:

- a `spawn(opts) -> process` constructor — `opts` carries the program path,
  argv, env, cwd, and per-fd `stdio-spec` (`inherit | piped | null`);
- a `process` resource with `take-stdin/out/err` (returning
  `wasi:io/streams` handles), `pid`, `signal(signum)`, `wait`, and
  `try-wait`;
- typed `spawn-error` and `signal-error` variants, including a transient
  `guest-not-ready` (the guest may not have reached userspace yet).

This is **only** the process-spawning path. Per-component "cheap syscalls
against the caller itself" (signal-to-self, signalfd, ptrace, bare fork
without exec, in-component `/proc`) are deliberately deferred to a sibling
interface and not part of v0.1.0 — see the WIT preamble + the design note
in v86 for why.

### How python-wasm consumes it

The natural integration points on the python-wasm side (none implemented
yet — this section is a sketch of the next concrete step):

- A CPython capability extension under `cpython-ext/_v86_posix/` that
  imports `v86:posix/process` and exposes a small private surface to
  Python (`spawn(opts) -> Process`, methods mirroring the WIT resource).
  Pattern matches the existing `cpython-ext/_compression`, `_crypto_hash`,
  `_ssl`, `_xxhash` extensions: each is `componentize-py`-ish C glue
  around a WIT import, plus a `Lib/<stdlib>.py` shim that exposes the
  CPython-stdlib-compatible API.
- A `Lib/_v86_subprocess.py` (or a patch to `Lib/subprocess.py`) that
  routes `Popen` through `_v86_posix.spawn` when running under a build
  that has the capability imported and `v86:posix/process` available.
- A composectl plan (`plans/python-v86.json`, sibling to the existing
  `plans/python-browser.json`) that plugs the `python-component.wasm`
  import against a `v86-component.wasm` export, producing a composed
  artifact suitable for the `native-v86` backend in
  `tegmentum:py-offload/registry`.

The `tegmentum:py-offload/registry` catalog and `py-offload/offload.run`
boundary already exist (`wit/py-package.wit`, `wit/py-offload.wit`); they
don't change. What changes is **what a `native-v86` backend looks like
under the hood**: the env digest now points at a composed `python-wasm +
v86` artifact rather than a v86 image containing a guest-resident CPython.
The "worker" that fulfils a `py_offload.run` against that env is the
composed component itself, executing the requested entry locally in
python-wasm and shelling out to v86 only when the implementation needs
to (e.g. an unported native package).

## What survives from the old model

The transport pieces in `reference-worker/py_offload/{protocol,worker,mailbox,types,codecs,...}.py`
and their tests are **not** v86-specific and are unaffected. The file-mailbox in
`mailbox.py` is a generic transport that can be used wherever a directory-as-channel
makes sense; it just isn't the v86 boundary anymore.

The v86-component fix to preserve a user-supplied `workspace/init`
(`~/git/v86` commit `b62afbc`) remains valid as a general bug-fix — silently
overwriting user files is wrong under any architecture.

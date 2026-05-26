# Tier 1 (v86) py-offload worker — setup recipe

Implements the recipe in `docs/tier1-v86-integration.md` for running a native
x86_64 CPython inside the v86 guest, serving offloaded calls over a virtiofs
file mailbox.

## What this stages

`setup.sh <workspace-dir>` populates a v86 workspace with the three host-side
pieces the recipe requires:

- `opt/python/` — a self-contained musl CPython (python-build-standalone, install_only),
- `lib/ld-musl-x86_64.so.1` (+ `libc.musl-…` link) — the loader the dynamically-linked
  Python needs at `/lib/`, taken from alpine-minirootfs,
- `opt/py_offload/` — the pure-Python `py_offload` package (rsynced from
  `reference-worker/`),

and installs `init.template` as `workspace/init`, which `exec`s `busybox sh` on
`ttyS0` and **background-launches** `python -m py_offload.mailbox /run/py-offload`.

## Run

```sh
WS=/tmp/tier1-v86
reference-worker/tier1-v86/setup.sh "$WS"
ln -s ~/git/v86/assets "$WS/assets"   # or cp -R; wasmtime needs both /assets + /workspace at the mount
wasmtime run -S http -S inherit-network \
    --dir "$WS::/" --env V86_MODE=linux --env V86_VIRTIOFS_ROOT=1 \
    ~/git/v86/target/wasm32-wasip2/release/v86.wasm
```

To drive offloaded calls from the host, write the framed `req-N.bin` files into
`$WS/workspace/run/py-offload/` (use `py_offload.mailbox.MailboxClient` pointed
at that directory).

## Validation status (honest)

The scaffolding is in place and exercises the documented recipe exactly. **End-
to-end validation is not done here.** v86's x86 emulator is interpreter-only
(`docs/native-execution-and-parallelism.md` §2.2 — "correctness/compatibility
tier, not a fast path"); a cold Linux boot + busybox + Python startup takes
minutes, so the practical validation loop relies on v86's snapshot/restore
(`V86_SNAPSHOT_DIR` / `V86_SNAPSHOT_AFTER`). A worker-ready snapshot must be
captured once against this workspace; subsequent runs restore from it.

The two specific unknowns the recipe inherits from the doc:

1. Whether the musl python-build-standalone binary runs cleanly in v86's
   busybox-only guest (interpreter behavior on emulated x86).
2. The cold-to-ready time before snapshot capture is practical.

Both are settable empirically by booting against this staged workspace and
watching `ttyS0` for the `[init] python ... ok` line.

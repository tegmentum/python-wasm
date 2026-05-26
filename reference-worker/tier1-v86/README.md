# Tier 1 (v86) py-offload worker — setup recipe

Implements the recipe in `../../docs/tier1-v86-integration.md` for running a
native CPython inside the v86 guest, serving offloaded calls over a virtiofs
file mailbox.

## What `setup.sh` stages

`setup.sh <workspace-dir>` populates a v86 workspace with the three host-side
pieces the recipe requires:

- `opt/python/` — self-contained musl CPython (currently python-build-standalone
  `install_only`; see "Open issue: 32-bit Python" below),
- `lib/ld-musl-*.so.1` (+ `libc.musl-…` link) — the loader the dynamically-linked
  Python needs at `/lib/`, taken from alpine-minirootfs,
- `opt/py_offload/` — the pure-Python `py_offload` package (`rsync`ed from `reference-worker/`),

and installs `init.template` as `workspace/init`, which mounts the pseudo-fs,
runs a foreground python smoke test, then background-launches
`python -m py_offload.mailbox /run/py-offload` and prints
`[init] py-offload-ready pid=<N>` to ttyS0 as the snapshot trigger.

## Cold-boot recipe (verified end-to-end through capture)

The v86 emulator is interpreter-only (`docs/native-execution-and-parallelism.md`
§2.2 caveat), but cold-boot-to-shell-ready is bounded — about 3 seconds wall
clock with a warm wasmtime AOT cache (~3 minutes cold), capturing a 12 MB
activation-safe snapshot. Configure wasmtime exactly like this:

    wasmtime run -S http -S inherit-network --dir "$WS::/" \
        --env V86_MODE=linux --env V86_VIRTIOFS_ROOT=1 \
        --env V86_BZIMAGE=/artifacts/kernels/virtiofs/6.8.12/<hash>/bzImage \
        --env V86_MAX_CYCLES=1500000000 \
        --env V86_SNAPSHOT_AFTER=py-offload-ready \
        --env V86_SNAPSHOT_EXIT=1 \
        ~/git/v86/target/wasm32-wasip2/release/v86.wasm

What each env var actually does (every one of them is load-bearing):

- `V86_BZIMAGE=…/virtiofs/6.8.12/…/bzImage` — the virtiofs-tuned kernel; the
  default `/assets/p04-bzimage.bin` is NOT the right one for virtiofs-root boot.
- `V86_VIRTIOFS_ROOT=1` — mount virtiofs as `/`, skip initramfs.
- `V86_MAX_CYCLES=1500000000` — the default cap stops the run before init
  finishes; ~1.5B is enough for kernel boot + sleep 4 + worker startup.
- `V86_SNAPSHOT_AFTER=py-offload-ready` — watches ttyS0 and captures a
  quiescent snapshot when the line appears and the guest is HLT-idle.
- `V86_SNAPSHOT_EXIT=1` — exits at the capture so cold-capture finishes in
  ~boot time and isn't conflated with worker steady-state.

Subsequent runs use `V86_RESTORE_SNAPSHOT=v86-machine/snap-v86-machine-0002` to
skip the cold boot.

### Important: clear stale snapshots before re-capturing

v86's snapshot publisher uses atomic rename and refuses to overwrite an existing
snapshot directory — a previous run's `snap-v86-machine-{0001,0002}` will silently
cause the new capture to fail with "atomic publish failed: snapshot dir already
exists". The capture log + manifest are still written but to the *old* directory,
so it looks like things worked. Before re-capturing:

    rm -rf $WS/snapshots/v86-machine/snap-v86-machine-* $WS/snapshots/v86-machine/.tmp-*

## Investigation findings (2026-05-26)

Two problems blocked the original setup; both are fixed/diagnosed here:

### Fixed: v86 was overwriting `workspace/init` on every boot

v86's `main.rs` had an unconditional `std::fs::write` of a default `/init` script
into the workspace dir before guest boot — any user-authored init was silently
reverted, so the original `init.template` (with its py_offload launch) was being
clobbered before the kernel ever read it. Patched in v86 commit **b62afbc** to
write the default init only when `workspace/init` doesn't already exist:

    let init_path = format!("{}/init", workspace_dir);
    if !std::path::Path::new(&init_path).exists() {
        let _ = std::fs::write(&init_path, init_script);
    }

After the patch, `init.template` survives the boot intact and the snapshot
trigger fires correctly — verified by `[init] py-offload-ready pid=…` on
ttyS0 and a `class: shell-ready (activation-safe: true)` snap-0002 capture.

### Open: 32-bit Python on a 32-bit v86

v86 emulates **x86-32 only** (upstream README: "Linux works pretty well. 64-bit
kernels are not supported"). The PBS musl `x86_64` python in
`opt/python/` cannot execute in the guest — the kernel rejects the 64-bit ELF
with ENOEXEC, busybox sh falls back to shell-interpreting the binary, and
`/var/log/py-offload` ends up containing exactly
`/opt/python/bin/python3: line 1: …`. (`init.template`'s foreground smoke test
catches this honestly now: it prints `py-offload-FAILED — python3 won't
execute (likely 32/64-bit arch mismatch)` and drops to a debug shell without
firing the snapshot trigger.)

python-build-standalone **does not** ship i686 musl builds (only `x86_64`,
`x86_64_v2..v4`, and `aarch64`). To make Tier-1 actually serve requests we need
a 32-bit musl CPython — likely paths:

- pull `python3` from Alpine's `x86` (32-bit) repo (`/alpine/v3.21/main/x86/`)
  and extract its `.apk`s manually (loader + libpython + python binary),
- build a tiny musl-cross-compile CPython for `i686-linux-musl`,
- or extend v86's vendored CPU to support long mode (much larger change,
  out of scope for the worker recipe).

Until that's resolved, the snapshot pipeline + init contract are verified
working end-to-end — the missing piece is a runnable 32-bit Python binary.

## Validation status (honest, post-investigation)

- Cold-boot to a clean `shell-ready` activation-safe snapshot: **verified**
  (snap-v86-machine-0002 at `/tmp/tier1full/snapshots/v86-machine/`, 12 MB,
  5/5 invariants pass, captured at HLT after init).
- `init.template` survives the boot intact: **verified** (v86 commit b62afbc).
- The `py_offload.mailbox` worker actually polling on `/run/py-offload`:
  **not yet** — blocked on the 32/64-bit Python arch mismatch described above.
- Round-trip request through the mailbox (host writes `req-1.bin`, guest writes
  `resp-1.bin`): **not yet** — depends on the above.

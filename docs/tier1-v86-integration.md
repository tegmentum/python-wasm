# Tier 1 (v86) integration plan

Status: **investigation result / plan for Issue #2.** Based on a read-only study
of `~/git/v86`. Goal: run a native x86 CPython executing `py_offload.serve` inside
a v86 guest, driven from the host over a byte channel — binding the
`tegmentum:py-offload` contract to real native package code.

## The guest as it exists today

- **Userspace is busybox-only — no glibc or musl.** `workspace/init` installs
  busybox symlinks and `exec sh </dev/ttyS0 >/dev/ttyS0`. There is no general ELF
  loader / C library present.
- **Rootfs** is either an initramfs (a cpio built at runtime from `workspace/`) or,
  with `V86_VIRTIOFS_ROOT=1`, the host's `workspace/` directory mounted at `/`
  over **virtiofs** (`root=workspace rootfstype=virtiofs rw`).
- **One UART, `ttyS0`, is the console/shell.** There is no second serial port.
- Packages (`packages/*.json`) are **WASM components**, run by a *host-side*
  executor via `wasmtime run` on the guest's behalf (`agent-component/ipc-executor.sh`)
  — the opposite direction from what Tier 1 needs, but proof that a virtiofs
  request/response file mailbox works.
- Snapshot/restore exists and is env-driven (`V86_SNAPSHOT_DIR`,
  `V86_SNAPSHOT_AFTER=<console trigger>`, `V86_RESTORE_SNAPSHOT`,
  `scripts/pack-snapshot.py`).

## Two hard constraints

1. **No libc in the guest.** A normal dynamically-linked `python3` will not run.
   Tier 1 needs a **self-contained x86_64-linux CPython** — a static musl build, or
   a relocatable build (e.g. python-build-standalone) shipped *with* its loader and
   libs. Whether such a build runs cleanly under this busybox guest is the single
   biggest unknown and must be validated first.
2. **No spare host↔guest stream channel.** `ttyS0` is the console. Two options:
   - **virtiofs file mailbox (recommended).** virtiofs *is* a shared host
     directory, so the host can read/write files the guest sees **while it runs —
     with no v86 Rust changes.** Frame requests/responses as files
     (`request.bin` / `response.bin`) with atomic temp+rename and ready/done flags.
   - **Add a second serial port (COM2).** A clean byte stream, but ~hundreds of
     lines of Rust UART + IRQ work *inside the v86 repo* (`v86-devices/src/uart.rs`,
     `v86-component/src/main.rs`).

## Recommended path (minimal / no v86 Rust changes)

1. Place a self-contained CPython tree **and** `py_offload` into `workspace/`
   (plain host content).
2. Tweak `workspace/init` to background-launch a resident worker that speaks the
   framed protocol over a **virtiofs mailbox** instead of stdio (guest-side, host
   content — no Rust).
3. On the host, give `StreamClient` a `MailboxChannel` (a reader/writer over the
   shared dir). The Python side is already transport-agnostic: `serve.py` /
   `protocol.py` / `client.py` work against any reader/writer pair, so only the
   channel is new.
4. Boot with `V86_VIRTIOFS_ROOT=1`; snapshot once the worker prints a ready
   trigger; restore per session so calls skip Linux boot.

This keeps all changes in **host content + Python**; the only v86-repo edit is
`workspace/init` (data, not Rust). COM2 is the fallback if the mailbox's latency
or torn-read handling proves inadequate (it should not for request/response).

## Already in place (python-wasm side)

`reference-worker/py_offload/{serve,protocol,client}.py` — the resident dispatcher,
the length-prefixed framing, and a host client. `StreamClient` abstracts the byte
streams, so the mailbox channel is the only missing transport. The contract and
codecs are proven (Issue #1 + the transport tests).

## Blockers & open decisions

- **B1 — self-contained CPython:** produce and validate an x86_64-linux CPython
  that runs in the busybox guest (static musl vs. relocatable-with-libs). *Main risk.*
- **B2 — channel:** virtiofs mailbox (recommended) vs. COM2 serial.
- **B3 — cross-repo:** the guest wiring (`workspace/init`, and COM2 if chosen)
  lives in `~/git/v86`; decide whether that work happens there.
- **B4 — file-mailbox robustness:** atomic temp+rename + ready/done flags to avoid
  torn reads; one outstanding request at a time (matches the resident loop).

## Next concrete step (no v86 runtime needed)

Implement `MailboxChannel` — a file-based reader/writer that frames requests and
responses through a shared directory — and a matching guest-side serve adapter,
**tested locally with a tmpdir standing in for the virtiofs mount** (exactly as
`SubprocessClient` stands in for the guest today). That advances Tier 1 and stays
fully verifiable before any CPython-in-guest work.

## Key references in `~/git/v86`

- `workspace/init` — guest init/console.
- `crates/v86-component/src/main.rs` — virtiofs-as-root + boot wiring; snapshot triggers.
- `crates/v86-devices/src/uart.rs` — UART (where COM2 would be added).
- `agent-component/ipc-executor.sh` — existing virtiofs JSON file-IPC (proof of pattern).
- `scripts/demo-build.sh`, `scripts/pack-snapshot.py`, `demo/linux-shell/server.py` —
  build, snapshot, and restore commands.

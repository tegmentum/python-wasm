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
- **B2 — channel:** **decided — virtiofs mailbox** (built:
  `reference-worker/py_offload/mailbox.py`). COM2 serial was the fallback; not needed.
- **B3 — cross-repo:** the guest wiring (`workspace/init`) lives in `~/git/v86`.
- **B4 — file-mailbox robustness:** atomic temp+rename + ready/done flags to avoid
  torn reads; one outstanding request at a time (matches the resident loop).
- **B5 — girder-actor ⇄ v86 wrapper:** v86 is **not** a girder actor (it exports
  `wasi:cli/command`; girder hosts only `turn-actor`/`loop-actor`, which import
  `host`). Need a wrapper component — composed via WAC/jco — that exports the actor
  world, imports `wasi:filesystem` for the mailbox, and runs the handoff. The actor
  does *not* call v86's CLI per request.
- **B6 — resident-v86 lifetime owner:** a host-managed instance vs. a girder
  `loop-actor` whose `run()` owns the v86 (since `wasi:cli/command run()` is
  run-to-completion, the resident v86 cannot live inside a `turn-actor.handle`).

## Scope: use #1 vs use #2

- **Use #1 (main) — the POSIX/WASI console runtime** is done *without* the x86
  guest: Python runs as a wasmmachine **component** (`v86/packages/python.json` +
  `agent-component/python-executor.sh`, which mounts the stdlib payload). This
  document is specifically about **use #2** — running genuinely unported native
  packages in the x86 guest.

## Orchestration & handoff (corrected)

- **A separate, girder-managed v86 instance per worker — never the interface
  instance.** A girder actor is its own isolated WASM instance, so a pool of
  native-worker actors is a pool of isolated v86 guests (isolation,
  responsiveness, independent lifecycle, per-actor budgets).
- **Handoff = the shared-directory mailbox, not v86's CLI.** A resident v86 runs
  `python -m py_offload.mailbox <dir>`; the actor writes a request file and reads a
  response file in the virtiofs-shared dir. The actor imports `wasi:filesystem`
  (granted to that dir) + girder `host`; it does **not** import `wasi:cli/command`
  for per-call work.
- **CLI is only the boot mechanism.** `wasi:cli/command run()` is
  run-to-completion, so the resident v86 is owned **outside** the per-call path
  (host-managed, or a `loop-actor` whose `run()` owns its lifetime); the routing
  `turn-actor` only does mailbox I/O against it.

## Status & next steps

- **Done:** the offload contract + codecs, both byte-stream transports, and the
  **virtiofs file-mailbox** (`reference-worker/py_offload/mailbox.py`) — the handoff
  substrate, proven locally; plus Python-as-component (use #1).
- **Next (use #2, needs the v86 runtime):**
  1. **B1** — a self-contained x86_64 CPython that runs in the busybox guest.
  2. The **girder-actor ⇄ v86 wrapper** (B5) doing the mailbox handoff.
  3. Decide the **resident-v86 lifetime owner** (B6).
  4. Wire `workspace/init` to launch the resident mailbox worker; snapshot; bench.

## Key references in `~/git/v86`

- `workspace/init` — guest init/console.
- `crates/v86-component/src/main.rs` — virtiofs-as-root + boot wiring; snapshot triggers.
- `crates/v86-devices/src/uart.rs` — UART (where COM2 would be added).
- `agent-component/ipc-executor.sh` — existing virtiofs JSON file-IPC (proof of pattern).
- `scripts/demo-build.sh`, `scripts/pack-snapshot.py`, `demo/linux-shell/server.py` —
  build, snapshot, and restore commands.

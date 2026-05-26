# Tier 1 (v86) py-offload worker — setup recipe

Implements the recipe in `../../docs/tier1-v86-integration.md` for running a
native x86_64 CPython inside the v86 guest, serving offloaded calls over a
virtiofs file mailbox.

## What `setup.sh` stages

`setup.sh <workspace-dir>` populates a v86 workspace with the three host-side
pieces the recipe requires:

- `opt/python/` — self-contained musl CPython (python-build-standalone, `install_only`),
- `lib/ld-musl-x86_64.so.1` (+ `libc.musl-…` link) — the loader the dynamically-linked
  Python needs at `/lib/`, taken from alpine-minirootfs,
- `opt/py_offload/` — the pure-Python `py_offload` package (`rsync`ed from `reference-worker/`),

and installs `init.template` as `workspace/init`, which `exec`s `busybox sh` on
`ttyS0` and background-launches `python -m py_offload.mailbox /run/py-offload`.

## What v86 boot actually costs (investigation result)

The v86 emulator is interpreter-only (`docs/native-execution-and-parallelism.md`
§2.2 caveat), but **cold boot to the busybox shell is bounded — about 3 minutes
on this machine** when configured the way `~/git/v86/scripts/capture-shell-ready.sh`
configures it (verified end-to-end):

- `V86_BZIMAGE=/artifacts/kernels/virtiofs/6.8.12/.../bzImage` (the virtiofs-tuned
  kernel; the default `/assets/p04-bzimage.bin` is NOT the right one for this loop),
- `V86_MAX_CYCLES=200000000` (without this, the default cap stops the run **before**
  init even gets to run — that was the original "boot doesn't reach userspace"
  symptom),
- `V86_SNAPSHOT_AFTER=<trigger string>` watches `ttyS0` and captures a runtime
  snapshot when the trigger appears and the guest is HLT-idle,
- `V86_SNAPSHOT_EXIT=1` exits at the capture so this finishes in ~boot time.

Subsequent runs use `V86_RESTORE_SNAPSHOT=…` to skip the cold boot.

## How to actually use this — for now: stage into the v86 repo, then capture

There is one open issue: when the wasmtime `--dir` mount is a *separate* host
directory containing the same `workspace/` + `assets/` + `artifacts/` layout
(e.g. `/tmp/tier1`), the standard `capture-shell-ready.sh` recipe does **not**
reach userspace under that mount — even with an unmodified workspace/init the
shell prompt never appears, the snapshot trigger never fires, and the run
terminates at `V86_MAX_CYCLES`. The same workspace contents under
`--dir=$V86_REPO::/` (i.e. mounting the v86 repo itself) DO reach the shell in
the documented ~3 minutes. Root cause TBD — likely something about which paths
must exist alongside `workspace/` in the mount.

Workaround (until that's diagnosed): stage your Tier-1 workspace into the v86
repo itself before capturing — back up `~/git/v86/workspace/init` and the like
first, then run `capture-shell-ready.sh` with `V86_SNAPSHOT_AFTER=py-offload-ready`
(matching the trigger in `init.template`).

## Validation status (honest)

- Standard cold-boot-to-shell on this machine: **verified** (a 12 MB shell-ready
  snapshot at `~/git/v86/snapshots/v86-machine/snap-v86-machine-0002`, ~3 min).
- Tier-1 worker-ready capture for the staged workspace: **not yet** — the
  custom-`--dir` issue above blocks it without the workaround.
- The two upstream unknowns the doc flags — does the musl PBS Python run in the
  busybox guest, and what's the cold-to-ready time — are still empirical, but
  the cycle/snapshot budgeting is no longer the unknown it appeared to be.

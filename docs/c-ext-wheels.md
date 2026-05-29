# C-extension support in python-wasm

Status: **Phase 4** of [`coverage-implementation-plan.md`](coverage-implementation-plan.md).

## The dlopen constraint

WASI Preview 2 has no `dlopen`. A `.cpython-314-wasm32-wasi.so` produced
by `setup.py build_ext` or `maturin build` is unloadable at runtime —
`ctypes.CDLL(None)` raises `NotImplementedError: wasm32-wasip2 has no
native ABI / dlopen`. CPython's wasi build acknowledges this by setting
`CCSHARED=''` in sysconfig, so `Tools/wasm/wasi build` doesn't even
emit `.so` files for the wasi target.

This rules out the conventional `pip install numpy` model entirely.
Two paths remain:

1. **Static link at build time** — `cpython-ext/<name>/` pattern. The
   extension's C/Rust sources land in `Modules/`, get linked into
   `python.wasm` by `Tools/wasm/wasi build`, and become a built-in
   module. Distribution is a **bespoke python.composed.wasm**.
2. **Offload to a sibling process** that holds a real CPython +
   native libs. The boundary is the `tegmentum:py-offload@0.1.0` WIT
   contract; calls serialize over a transport and round-trip a result.

This doc covers Path 2 — that's where Phase 4 made progress.

## Path 2 — py-offload

### Architecture

```
┌────────────────────────────────┐                    ┌─────────────────────────────┐
│ python.composed.wasm           │                    │  host CPython worker        │
│                                │                    │   ├─ numpy, scipy, …        │
│  sitecustomize ─┐              │                    │   └─ py_offload.mailbox     │
│                 ▼              │                    │       .serve_mailbox()      │
│  _offload_shim                 │                    │                             │
│   ├─ importhook (meta_path)    │                    │   loop:                     │
│   │   ├─ proxies "numpy" etc.  │   shared dir       │     read req-<n>.bin        │
│   │   └─ on call:              │ ◄────mailbox────► │     decode, call numpy.X    │
│   │       MailboxClient.run()  │  /work/mailbox/    │     encode result           │
│   │         write req-<n>.bin  │                    │     write resp-<n>.bin     │
│   │         poll resp-<n>.bin  │                    │                             │
│   └─ types, codecs (msgpack)   │                    │                             │
└────────────────────────────────┘                    └─────────────────────────────┘
```

The shim, the transport, and the worker library all come from the
`reference-worker/` Phase-1 implementation. Phase 4 of this repo's plan
vendors the client-side pieces into `cpython-ext/_offload_shim/` and
wires them via `make install-python-shims`. They land at
`Lib/_offload_shim/` and an auto-install hook in `Lib/sitecustomize.py`.

### Activating the hook

Set two env vars before invoking `python.composed.wasm`:

| Variable | Value | Meaning |
|---|---|---|
| `OFFLOAD_MAILBOX_DIR` | mounted path | Where to put `req-<n>.bin` / read `resp-<n>.bin`. The directory must be visible to both the guest (via `--dir`) and the host worker. |
| `OFFLOAD_PACKAGES` | comma list | Module names the importhook should proxy (e.g. `numpy,scipy,cryptography`). Bare imports of these names become proxy modules; attribute calls become offload requests. |

When either is unset, the hook does nothing — the shim is dormant.

### End-to-end demo: numpy

`scripts/test-offload-numpy.sh` is the worked example. It:

1. Creates a temp mailbox directory.
2. Starts a host-side worker (`python3 -c "from py_offload.mailbox import serve_mailbox; serve_mailbox(...)"`).
3. Runs `python.composed.wasm` with `OFFLOAD_MAILBOX_DIR` and `OFFLOAD_PACKAGES=numpy`.
4. The guest script calls `numpy.linalg.det([[1,2],[3,4]])`, `numpy.linalg.norm`, and `numpy.sum` — all return Python scalars that round-trip through msgpack.
5. Validates the numeric results.

Running it locally (requires `numpy` installed on the host Python):

```bash
NETWORK= ./scripts/test-offload-numpy.sh
# det([[1,2],[3,4]]) = -2.0000000000000004
# norm([3,4]) = 5.0
# numpy.sum([1,2,3,4]) = 10.0
# OK: 3 numpy round-trips through offload boundary
```

### What works today

| Category | Status |
|---|---|
| Importing native-only packages by name (the proxy is silent) | ✅ |
| Calling top-level functions with primitive args | ✅ |
| Calling dotted-attribute functions (`numpy.linalg.det`) | ✅ |
| Returning Python scalars (`int`, `float`, `str`, `bool`, `None`) | ✅ (msgpack codec) |
| Returning Python lists/dicts/tuples of scalars | ✅ |
| Exception propagation across the boundary (preserves type) | ✅ |
| `numpy.ndarray` as a return value | ❌ — Phase 3 arrow codec |
| Live-object proxying (callbacks, generators, `__iter__`) | ❌ — Issue #5 of native-exec plan |
| Concurrent calls (more than one in flight) | ❌ — protocol is one outstanding request at a time |

### Codec map

| Codec | Primitive types | numpy.ndarray | Live objects | Status |
|---|---|---|---|---|
| `json` | yes | no | no | ✅ |
| `msgpack` | yes | no | no | ✅ (default) |
| `arrow` | — | yes | no | ⏸ Phase 3 of native-exec plan |
| `pickle` | yes | yes | partial | ⏸ opt-in, same-trust hops only |

### What status of which packages

This list isn't a guarantee that every API of these packages works — it's
what the boundary supports. Anything that goes back to a `ndarray` /
`DataFrame` / `bytes` / custom object today returns "msgpack: cannot
serialize <X>". Phase 3 arrow + Phase 5 live-object proxying close that.

| Package | Where it runs today | Notes |
|---|---|---|
| `numpy`        | host worker (subprocess) | scalars + tolist round-trip. Arrays need arrow codec. |
| `cffi`         | host worker | only sensible inside the boundary anyway — cffi is dlopen+jit |
| `cryptography` | host worker | works for one-shot calls returning bytes |
| `lxml`         | host worker | useful for `lxml.etree.tostring` etc. |
| `pandas`       | host worker | only `to_json` / `to_dict` paths today; DataFrames need arrow |
| `scipy`        | host worker | same scalar/list constraint as numpy |
| `pydantic-core` | host worker | OR static-link via Path 1 — Rust + PyO3 build path |

### Where the host worker comes from

The Phase 1 implementation is `~/git/python-wasm/reference-worker/`. It
exports `serve_mailbox(dir)` (host-side daemon) and a `MailboxClient`
(guest-side caller). Both speak the same `tegmentum:py-offload@0.1.0`
WIT contract:

- WIT: `wit/py-offload.wit`
- Reference impl: `reference-worker/py_offload/`
- This repo's vendored slice (guest-side only): `cpython-ext/_offload_shim/`

For Tier 1 (v86 backend), the same `MailboxClient` and the same
serialization stay; only the worker side moves from a host subprocess
to a resident dispatcher inside a v86 guest. See
[`docs/native-execution-and-parallelism.md`](native-execution-and-parallelism.md)
§4.2 for the Tier-1 mapping.

## Path 1 — cpython-ext static link

For libraries small enough to want lower latency than the offload boundary
provides (a per-call round-trip is ~milliseconds, not microseconds), the
existing `cpython-ext/<name>/` pattern stays available. Worked examples
in this repo:

- `cpython-ext/_zlib_cap/`  — zlib via `zlib:compression@0.1.0`
- `cpython-ext/_ssl_capability/` — TLS via `openssl:component`
- `cpython-ext/_sqlite_capability/` — SQLite via `sqlite:wasm/high-level`
- `cpython-ext/_crypto_hash/` — cap-routed hash module
- `cpython-ext/_kdf_cap/` — argon2/scrypt/pbkdf2 via `password-hash-multiplexer`

Adding a new one is a `wire-cpython-ext.sh` job: drop the directory in
`cpython-ext/`, follow the file-naming convention, re-run
`make build && make python-composed`.

`pydantic-core` is the obvious next candidate if a particular use case
proves offload-latency-bound (Rust + PyO3 + wasi-p2 `cdylib` output via
maturin). Tracking that as future work — not blocking Phase 4 exit.

## See also

- `docs/native-execution-and-parallelism.md` — the full design, including
  Tier-1 (v86) and Tier-P (girder) backends.
- `reference-worker/README.md` — the worker, transports, codecs.
- `wit/py-offload.wit` — the contract.
- `cpython-ext/_offload_shim/` — what python-wasm ships.
- `scripts/test-offload-numpy.sh` — the Phase 4 demo.

# Native package execution & GIL-free parallelism — design

Status: **proposal / investigation**. This document sketches how `python-wasm`
can (a) run native code from Python packages that have *not* been ported to
WebAssembly, and (b) scale Python across cores without being throttled by the
CPython GIL. The boundary in both cases is expressed as **WIT** so the pieces
compose under the component model.

It draws on two sibling repositories:

- `~/git/v86` — a Rust port of the v86 x86 emulator, built to `wasm32-wasip2`
  and packaged as a WebAssembly component (the "wasmmachine").
- `~/git/girder` — a Rust multicore WebAssembly **actor runtime**.

## 1. The problem

`python-wasm` builds CPython 3.14 to `wasm32-wasip2`. Pure-Python packages and C
extensions that are *statically linked into the wasip2 build* work fine. The gap
is third-party packages whose native code is only published as native (x86/arm)
artifacts — numpy, scipy, pandas, pillow, lxml, cryptography, and the long tail
of manylinux wheels. Two distinct needs fall out of this:

1. **Compatibility** — run a package's native code when no `wasm32-wasip2` wheel
   exists, ideally without recompiling the package.
2. **Throughput** — when work *is* CPU-bound Python, run it in parallel across
   cores. In-process this is bounded by the GIL.

The constraint from the brief: **the interface should be WIT**, so each solution
is a typed component boundary rather than an ad-hoc FFI.

## 2. The building blocks

### 2.1 CPython on wasip2 (this repo)
A single-threaded WebAssembly component. Each instance has its own linear memory
and its own GIL. In the browser it runs via jco + `@tegmentum/wasi-polyfill`; on
a server it runs under wasmtime.

### 2.2 v86 wasmmachine (`~/git/v86`)
A full x86 emulator compiled to `wasm32-wasip2` and shipped as a component. It
boots a real Linux guest, so it can execute **unmodified native x86 binaries** —
including a native CPython interpreter with native `.so` extension modules. This
is the key property: a manylinux wheel can run *as-is* inside the guest, with no
WASM port.

Relevant facts for integration:
- Today the component exports **`wasi:cli/command@0.2.3`**. The richer WIT worlds
  (`v86:boot/boot-manager`, `v86:device/*`, `v86:platform/display`) are specified
  under `~/git/v86/wit/` but **not yet wired into the component build**. So the
  practical channel right now is argv + stdio + the emulated serial console;
  the typed boot/device WIT is the future surface.
- Guest I/O paths: serial console (`ttyS0` ⇄ stdout/stdin), an IDE disk image, a
  `virtiofs` mount at `/workspace`, and an NE2K NIC bridged to host networking.
- **`packages/`** are JSON manifests that map artifacts into the guest (e.g.
  `coreutils.json` exposes 101 tools under `/bin`). Packages are either
  *materialized* (files injected into the guest fs) or *invoked*.
- **Snapshots** (`snapshots-b/`) save guest memory + CPU state for fast restore —
  important because cold-booting Linux per call would dominate latency.
- Caveat: x86 emulation runs interpreter-only (JIT disabled for WASM safety), so
  this is a **correctness/compatibility tier, not a fast path**.

### 2.3 girder (`~/git/girder`)
**Not** the Kitware data platform — a different project. A host-side Rust runtime
that runs N isolated WASM instances ("actors") across N cores on a multi-thread
Tokio runtime: no shared linear memory, no atomics, coordination by message
passing. Each actor is one Tokio task + one `wasmtime::Store` + one WASM instance.

- Actor contract is WIT (`~/git/girder/wit/actor.wit`): the `turn-actor` world
  **exports** `init(args)` and `handle(msg) -> result<list<u8>>` and **imports** a
  `host` interface (`send`, `call`, `self-ref`, `log`).
- Execution modes: **Turn** (stack released after each message → thousands of
  idle actors cost ~0 stack), **Loop** (long-lived), and **SIR** (Shared
  Immutable Regions — read-only zero-copy fan-out of a large buffer to all
  actors; reported ~13× vs copying).
- Mature: demonstrated ~7.9× speedup on 12 cores.
- **It is a native host runtime** (Tokio threads). It is *not* a browser
  technology — see §7.

## 3. The core idea: a tiered Python runtime

Route each import/call to the cheapest tier that can satisfy it:

| Tier | Runs in | Use for | Cost |
| ---- | ------- | ------- | ---- |
| **0 — in-WASM** | the wasip2 CPython itself | pure Python + extensions already built for wasip2 | none (in-process) |
| **1 — native-in-v86** | a native CPython inside the v86 guest | packages with native code and no wasip2 wheel | high (emulation + boundary), amortize with batching/snapshots |
| **P — parallel** | many CPython-WASM actors under girder | CPU-bound Python that partitions into shared-nothing work | data-movement cost across actors |

Tiers 1 and P share one mechanism: **submit serialized work across a WIT boundary
and get a serialized result back**. That shared shape is what we standardize.

## 4. The WIT boundary

A single small interface covers "run this Python work somewhere else." The same
shape is implemented by the v86-backed native worker (Tier 1) and, wrapped in
girder's actor world, by the parallel CPython workers (Tier P).

```wit
package tegmentum:py-offload@0.1.0;

interface offload {
  /// Identity of the worker environment: which interpreter + which packages are
  /// installed. Content-addressed, mirroring v86's digest(spec) identity model,
  /// so a request can pin exactly the environment it needs.
  type env-id = string;

  /// How args/return values are encoded on the wire. Arrow is preferred for
  /// ndarray/dataframe payloads (pairs with girder SIR for zero-copy read-only
  /// inputs); pickle is opt-in and only within a trust boundary.
  enum codec { msgpack, arrow, pickle, json }

  record task {
    /// "package.module:callable" resolved inside the worker interpreter.
    entry: string,
    args: list<u8>,
    codec: codec,
  }

  record py-error {
    kind: string,        // exception class, e.g. "ValueError"
    message: string,
    traceback: string,
  }

  variant outcome {
    ok(list<u8>),        // serialized return value (same codec as the task)
    raised(py-error),    // exception that crossed the boundary
  }

  /// One call, synchronous from the caller's perspective.
  run: func(env: env-id, t: task) -> outcome;
}

world py-worker {
  export offload;
}
```

Why this shape:
- **Function-call offload, not transparent object proxying.** A v1 that ships
  `entry + serialized args` and returns `serialized result` is tractable and
  covers the motivating cases (`numpy.linalg.svd(a)`, `pandas.read_parquet(...)`).
  Fully transparent `import numpy as np` with live cross-boundary objects
  (ndarray views, callbacks, `__getattr__` chains) is a research-grade problem and
  explicitly out of scope for v1.
- **`env-id` is content-addressed** to match v86's identity model and to make the
  worker environment reproducible/cacheable.
- **Codec is explicit** so big numeric payloads use Arrow (and can ride girder's
  SIR for read-only broadcast) while small control payloads use msgpack/JSON.

### 4.1 Ergonomic layer (Python side, Tier 0 → Tier 1)
A `meta_path` finder in the WASM CPython intercepts imports of packages marked
"native-only" and returns a **proxy module**. Attribute calls on the proxy
serialize `(entry, args)` and invoke `offload.run` against the v86-backed worker,
deserializing the result. This keeps user code close to normal Python while being
honest that only *call-with-serializable-args* is supported in v1.

### 4.2 Mapping Tier 1 onto v86 today vs. later
- **Today (no wired WIT):** implement `offload.run` as a host shim that drives the
  v86 component via `wasi:cli/command` — write the task to the guest over
  `virtiofs`/serial, run a small resident dispatcher (native CPython) in the
  guest, read the result back. Use a **snapshot** of a booted, package-loaded
  guest so each call skips Linux boot.
- **Later (wired WIT):** when v86 exposes `boot-manager`/device contracts as WIT,
  the host shim becomes a typed component-to-component call and the guest
  dispatcher can be packaged as a v86 `package`.

## 5. Parallelism with girder — does it solve the GIL?

**Short answer: girder does not remove the GIL; it makes the GIL stop mattering
by running many independent interpreters in parallel.**

- girder gives no free-threading and no subinterpreters. Each actor is its own
  WASM instance, so each CPython-WASM actor has its **own private GIL and its own
  linear memory**. Running N of them on girder's multi-thread Tokio runtime yields
  true multicore execution; the GIL serializes work *only within* an actor, never
  *across* actors.
- This is structurally the **shared-nothing, message-passing** model — like
  `multiprocessing` or PEP 684 subinterpreters, except the isolation unit is a
  WASM instance and the scheduler is girder. Compared to OS processes it's
  cheaper (no fork, thousands of idle Turn-actors cost ~0 stack) and it works in
  hosts without process support.
- **Cost:** no shared mutable memory — inputs/outputs move by message passing.
  Mitigate with girder **SIR** for large *read-only* inputs (broadcast a dataset
  once, all actors read it zero-copy) and with **Arrow** payloads.

What it takes to use it: wrap the CPython-WASM component so it **exports girder's
`turn-actor` world** (`init`/`handle`) and dispatches `handle(msg)` to a Python
entrypoint — i.e. the same `offload`-style contract, adapted to girder's actor
ABI. Then a parallel `map` is: spawn K CPython actors, fan tasks across them with
`Runtime::call`, collect results (the pattern in
`~/git/girder/crates/girder-runtime/examples/parallel_mapreduce.rs`).

**Verdict:** girder is a good fit for **GIL-unconstrained, shared-nothing
parallelism** of CPU-bound Python that partitions cleanly (embarrassingly
parallel maps, fan-out/fan-in). It is *not* a fit for fine-grained shared-memory
threading; if you need that, the answer is free-threaded CPython (PEP 703), which
is orthogonal to girder.

## 6. How it composes

```
                         ┌──────────────────────────────────────────────┐
                         │ girder runtime (native host: wasmtime+Tokio)  │
                         │                                               │
   user Python   ─────▶  │   CPython-WASM actor ×N   (Tier 0 + Tier P)   │
   (driver)              │     │  each: own GIL, own memory              │
                         │     │  exports girder turn-actor (init/handle)│
                         │     ▼                                         │
                         │   offload.run(env, task)  ──── Tier 1 ───────┐│
                         └───────────────────────────────────────────── │┘
                                                                         ▼
                                              ┌────────────────────────────────┐
                                              │ v86 wasmmachine (component)     │
                                              │  Linux guest + NATIVE CPython   │
                                              │  + native .so wheels            │
                                              │  resident dispatcher; snapshot- │
                                              │  restored to skip boot          │
                                              └────────────────────────────────┘
```

- girder is the **parallel substrate**: CPython-WASM instances are its actors.
- v86 is the **native-compat escape hatch**, reached through the same `offload`
  WIT shape; a CPython actor that hits a native-only package delegates to it.
- One contract (`tegmentum:py-offload`) spans both fallbacks; girder's
  `wit/actor.wit` is the transport for the parallel case.

## 7. Deployment contexts (important caveat)

This composition is strongest on a **native/server host (wasmtime)**:
- girder needs OS threads (multi-thread Tokio) — it is not a browser runtime.
- v86 is a wasip2 component; running full x86 emulation is heavy and only
  practical server-side.

In the **browser** (the current demo target), neither girder nor v86 applies
directly. The browser-side analogues, if pursued, are **Web Workers** for
parallelism (one CPython-WASM per worker, `postMessage` instead of girder
messaging) and "native packages" being effectively unavailable (or proxied to a
remote server running the stack above). Recommend treating browser parallelism as
a separate, later track that reuses only the `offload` *interface*, not girder
itself.

## 8. Maturity & gaps

| Piece | State | Gap to close |
| ----- | ----- | ------------ |
| CPython wasip2 | working (this repo) | — |
| v86 native execution | emulator mature (364 tests) | richer WIT not wired; need a resident guest-side dispatcher + snapshot workflow; perf is emulation-bound |
| girder parallel actors | mature (7.9× / 12 cores) | CPython must export the `turn-actor` world; need a CPython↔actor adapter |
| `tegmentum:py-offload` WIT | **does not exist yet** | define + version the package; codecs; error mapping |
| Python import-hook / proxy | does not exist | `meta_path` finder + proxy module; serialization rules |

## 9. Recommended phased plan

1. **Define `tegmentum:py-offload@0.1.0`** (this doc's §4) and a tiny reference
   worker that runs `entry(args)` in a normal native CPython, ignoring v86/girder
   — proves the contract and the codecs end-to-end.
2. **Tier 1 via v86 over `wasi:cli/command`:** stand up a resident guest
   dispatcher, snapshot a package-loaded guest, implement `offload.run` as the
   host shim. Benchmark call latency with/without snapshot restore.
3. **Tier P via girder:** build the CPython→`turn-actor` adapter; reproduce the
   `parallel_mapreduce` pattern with a CPython payload; measure scaling and the
   data-movement cost; add Arrow + SIR for large read-only inputs.
4. **Ergonomic import hook** for native-only packages, targeting one real package
   (numpy) as the proof case.
5. **Revisit transparent proxying** only if call-offload proves insufficient.

## 10. Decisions

Resolved 2026-05-23:

- **Serialization — Arrow + msgpack, pickle opt-in.** Arrow IPC for arrays/frames
  (pairs with girder SIR for zero-copy read-only fan-out); msgpack as the default
  for everything else. `pickle` is permitted only on a same-version, same-trust
  hop (e.g. CPython-WASM ↔ CPython-WASM girder actors we own) — never as the wire
  default and never across the v86 boundary. Phase 1 still ships msgpack/json;
  Arrow lands with Phase 3.
- **`env-id` — content digest, tags as aliases.** Identity is the digest of a
  resolved spec (interpreter version + exact wheels), expressed as a v86 package
  manifest; `digest(manifest + resolved artifacts)` = `env-id`. Human tags are
  convenience aliases over digests. First use builds and caches the v86 snapshot
  keyed by the `env-id`.
- **Free-threaded CPython (PEP 703) — deferred.** wasip2 has no usable OS threads
  (the shared-memory `wasi-threads` model is the one girder deliberately avoids),
  so removing the GIL gains nothing in-process and adds extension-ABI cost. The
  parallelism path is girder's multiple isolated instances. Revisit only if a
  native (non-WASM) CPython tier is added or the component model gains usable
  shared-memory threads.
- **Browser — interface-only reuse, build deferred.** Keep Tier 0 (in-WASM
  CPython) working in the browser and make the `offload` boundary
  transport-agnostic so a browser can later adopt it via Web Workers (one
  CPython-WASM per worker) plus a remote endpoint for native packages. No browser
  parallelism work in this design; native packages in-browser are only reachable
  via a remote server running Tiers 1/P.
- **Tier-1 positioning — interactive-capable.** v86 Tier 1 is fast enough for
  interactive use, so the `offload` API and the import hook (#4) may route
  fine-grained calls to it, not only coarse batches. A resident,
  snapshot-restored worker is still required (never boot-per-call), and batching
  large native work remains good practice where it helps. Phase 2 still
  benchmarks warm-call latency — now to characterize the operating envelope
  (where batching pays off, the emulated-CPU multiplier), not as a go/no-go gate
  on interactivity.

## 11. Open questions

None outstanding — every design fork above is resolved as of 2026-05-23. Phase 2
benchmarking (Issue #2) will characterize Tier-1 latency to tune API granularity,
but it no longer gates the design.

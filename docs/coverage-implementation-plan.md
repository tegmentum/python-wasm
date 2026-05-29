# Coverage implementation plan

Unified, phased plan covering:

- **Option (a)** — older Python support (was: [`older-python-support.md`](older-python-support.md)). Paused per 2026-05-28; included here as a deferred phase with a defined entry condition.
- **Option (b)** — deeper coverage of what latest CPython enables (was: [`latest-python-investment-areas.md`](latest-python-investment-areas.md)). Active.

This plan layers work into phases ordered by **prerequisite + payoff**, not by tier number. Each phase has an exit criterion. Phases 1–3 are the active option-(b) track; Phase 7 is option (a).

Two facts shape the new ordering:

1. **uv-wasm is production-grade.** `~/git/uv-wasm/dist/uv.wasm` is a working wasip2 component with native-offload-aware resolution (`tegmentum:py-offload`), real HTTPS, `uv venv` writing the layout directly, and validated PEP 517 sdist builds. Wheel resolution is not new work; integration is.
2. **DoH DNS polyfill is already implemented** in `~/git/wasi-polyfill/src/wasip2/plugins/sockets/ip-name-lookup.ts` (629 lines, three impls — `doh` / `virtual` / `stub`). 1.3 is not new code; it's verification + default selection.

That collapses the original Tier 1 / Tier 1.3 from "new implementation" to "wire and verify."

---

## Phase 0 — Verify what's already shipped (1–2 days)

Cheap and load-bearing. Don't plan further work on top of assumptions.

- [ ] **DoH default in web demo.** Inspect `web/` polyfill config; confirm the sockets plugin selects `doh` (not `stub`). If `stub`, switch the default to `doh` with Cloudflare and document opt-out.
- [ ] **`socket.getaddrinfo('pypi.org')` end-to-end.** Add a one-shot test under `scripts/` that runs in both wasmtime (`--allow-ip-name-lookup`) and the web demo. Expected: returns an A/AAAA list with no flag rituals in the browser.
- [ ] **uv-wasm reachability from python-wasm.** Document the integration shape — uv-wasm runs as a separate WasmMachine app; python-wasm invokes it via the WasmMachine command service rather than embedding it. Confirm by running `wasmtime ~/git/uv-wasm/dist/uv.wasm -- --version`.
- [ ] **Refresh `docs/stdlib-dependency-sweep.md`.** Re-run the import smoke test against the current `build/3.14-current/python.composed.wasm`. This is option (b) 2.3 reordered first because it sets a known baseline for everything that follows.

**Exit criterion.** Updated sweep doc + a one-page note in `docs/` ("verified state 2026-05-28") that lists what works now.

---

## Phase 1 — Wheel install end-to-end via pip + uv (1–2 weeks)

The Tier-1.1 work, revised: pip is already shipped in CPython's stdlib (`Lib/ensurepip/`); the gap is **wiring**, not implementation.

### 1.1 — pip path inside python-wasm

- [ ] Audit whether `python -m ensurepip` succeeds in the composed wasm. If it imports cleanly but fails on a stdlib gap, file the specific gap and patch.
- [ ] Wire writable `site-packages`:
  - **wasmtime CLI**: document `--dir` mount conventions; bake into `scripts/run-python.sh`.
  - **browser**: pick one of (a) IndexedDB-backed FS via wasi-polyfill, (b) in-memory FS scoped to session. Lean toward (a) for the wheel-install story to be useful between page loads.
- [ ] Smoke-test `python -m pip install --no-build-isolation requests`. If it works, snapshot the top-50 pure-Python packages list and run the table.

### 1.2 — uv-wasm integration story

uv-wasm runs as its own WasmMachine app, not embedded in python-wasm. Two integration paths:

- **Path A (delegated)**: user runs `wasmmachine run uv -- pip install <pkg>`; uv-wasm resolves + downloads + writes wheels into a shared filesystem location; python-wasm's `sys.path` picks them up. **Recommended.** No code change in python-wasm; just docs + a wrapper script.
- **Path B (embedded)**: python-wasm imports uv-core via WIT. Not advised — uv-core is large, and embedding it duplicates what the WasmMachine command boundary already gives us.

- [ ] Document Path A in `docs/wheel-install.md`. Worked example: `wasmmachine run uv pip install requests && wasmmachine run python -c "import requests; print(requests.__version__)"`.
- [ ] Confirm uv-wasm's `tegmentum:py-offload` resolution does the right thing when python-wasm itself is the target interpreter (no native C-ext fallback should be attempted — python-wasm can't dlopen).

### 1.3 — DNS verification (already implemented)

- [ ] Confirm Phase 0's DoH wiring covers `pip` / `uv` fetches against PyPI in the browser.
- [ ] Default `enableDoh: true` in the web demo's polyfill instantiation if not already.
- [ ] One CLI helper script that bundles `--allow-ip-name-lookup --inherit-network` so users don't memorize flags.

**Exit criterion.** `pip install requests` and `uv pip install requests` both work in wasmtime CLI; `requests.get('https://example.com')` returns a response. Top-50 pure-Python packages table merged into `docs/stdlib-dependency-sweep.md`.

---

## Phase 2 — asyncio + TLS battle-test (3–5 days)

Tier 2.2. Independent of Phase 1; can run in parallel.

- [ ] `aiohttp.ClientSession().get('https://example.com')` smoke test under `scripts/test-asyncio-tls.sh`.
- [ ] `httpx.AsyncClient().get(...)` same.
- [ ] Capture any edge case at the `wasi:io/poll` → asyncio event loop boundary. If a fix is needed in `_ssl_capability` or the polyfill's pollable subscription, file and patch.

**Exit criterion.** A passing `test-asyncio-tls.sh` plus a one-paragraph note in `docs/stdlib-dependency-sweep.md` on async-HTTP support status.

---

## Phase 3 — Stripping for size (1 week)

Tier 3.3 promoted because Phase 1's wheel install path makes browser delivery weight matter more (wheels add to the bundle).

- [ ] Strip unused stdlib (`test/`, `idlelib/`, `tkinter/`, `turtle/`, `lib2to3/`).
- [ ] Strip CPython compiled-in test modules.
- [ ] `-Os` on CPython itself (cap fleet already does this).
- [ ] Measure: target ≤ 30 MB uncompressed, ≤ 10 MB gzipped.

**Exit criterion.** Numbers in `docs/build-profiles.md`; new `profiles/3.14-slim.toml` profile for the stripped variant.

---

## Phase 4 — C-extension story (revised 2026-05-29 to align with the native-exec plan)

The original "build `.so` wheels, `pip install` them" framing assumed
dlopen; wasi-p2 has none, so wheel-produced `.so` artifacts are
unloadable (`ctypes.CDLL(None)` raises NotImplementedError; `CCSHARED=''`
in the wasi build's sysconfig).

The project's canonical answer for native packages already exists in
[`docs/native-execution-and-parallelism.md`](native-execution-and-parallelism.md)
and the `reference-worker/` Phase-1 implementation: **run native code in
v86 (Tier 1) or in a girder actor (Tier P), reached through the
`tegmentum:py-offload@0.1.0` WIT boundary.** A `meta_path` finder in
python-wasm intercepts imports of native-only packages, serializes
function calls, and round-trips them through a backend that holds a
native CPython.

This phase's job is to wire python-wasm's side of that plan, not invent
a new path:

- [ ] **Integrate `reference-worker/py_offload/importhook.py` into
      python-wasm.** Ship as `cpython-ext/_offload_shim/` (pure-Python,
      installed via `make install-python-shims`) so the meta_path finder
      registers at interpreter startup when a backend env is configured.
- [ ] **Define the catalog format** (which packages are native-only, what
      backend env to use). Lightweight TOML at
      `Lib/_offload_catalog.toml` — the importhook reads it. Aligns with
      §4.3's `wit/py-package.wit` registry without requiring composectl
      yet.
- [ ] **Wire a Tier-1-today backend** — the `SubprocessClient` from
      `reference-worker/py_offload/client.py` driving a host-side native
      CPython under wasmtime is the proof-of-life today. Later this is
      replaced by the v86 wasmmachine.
- [ ] **Demo end-to-end:** `import numpy as np; np.linalg.svd(...)` in
      python-wasm routes through the offload boundary to a native CPython
      and returns the right answer. (numpy isn't ported to wasm; this
      proves the path.)
- [ ] **Document in `docs/c-ext-wheels.md`** — what works today
      (subprocess backend), the v86 path to come, the cpython-ext
      static-link pattern as the alternative for libraries small enough
      to want it (e.g. pydantic-core via Rust+PyO3+wasi-p2 cdylib).

Status of the four originally-named top-5 packages under this framing:

| Package        | Today via offload subprocess | Future via v86 (Tier 1) |
|----------------|------------------------------|--------------------------|
| `numpy`        | works (host has numpy)       | works (linux i686 wheel) |
| `cffi`         | works (host has cffi)        | works                    |
| `cryptography` | works (host has cryptography)| works                    |
| `lxml`         | works (host has lxml)        | works                    |
| `pydantic-core`| works                        | works (or static-link via cpython-ext as the lighter option) |

The static-link path (cpython-ext) stays available for size- or
latency-sensitive packages that have a Rust+PyO3 build (or a small C
surface). pydantic-core is the obvious worked example to add later if
the offload boundary's per-call latency proves prohibitive for that
specific use case.

**Exit criterion.** `docs/c-ext-wheels.md` lands; the importhook ships
as a cpython-ext-installed shim with a `SubprocessClient`-driven
backend; one demo (numpy.linalg or similar) routes call → result
through the offload boundary in `scripts/test-offload-numpy.sh`.

---

## Phase 5 — Subprocess via v86 ✅ DONE (2026-05-29)

Tier 2.1. The plan assumed upstream-blocked, but `~/git/v86/` had shipped
the real impl: `v86-posix-host.wasm` exports `v86:posix/process@0.1.0`
backed by a virtiofs-style file mailbox + a host-or-guest helper that
runs `fork`/`execve` (Phase 1 spawn+wait → Phase 3c interactive stdin via
the deferred-spawn shim).

What landed on this repo's side:

- **Bug fix** in `scripts/load-profile.sh`. The loader documentation
  promised caller-set env vars win over profile values, but the emit
  function unconditionally `KEY=value`'d everything, so the v86 roundtrip
  script's `V86_POSIX_COMPONENT_WASM=…/v86_posix_host.wasm` was being
  silently clobbered by the profile's stub path. Fix: emit
  `KEY="${KEY:-value}"` so eval honors caller overrides. Side effect:
  every other profile key now also honors env overrides, which is what
  the doc claimed all along.
- **Compatibility symlink** `build/python.composed.wasm → 3.14-current/python.composed.wasm`
  so the v86-side test script (which predates the profile dir layout)
  finds the right artifact.
- **`scripts/run-python-with-subprocess.sh`** — single-command wrapper
  that recomposes with `v86-posix-host`, starts the helper, mounts the
  mailbox, and execs the composed wasm. Same network/site-packages
  wiring as `run-python.sh`. End-user equivalent of the v86 roundtrip
  test setup.

What was verified end-to-end against `~/git/v86/scripts/test-v86-posix-roundtrip.sh`:

- `subprocess.run([TRUE])` / `[FALSE]` exit codes
- `sh -c exit 42` custom exit codes
- Signal-killed children surface as `returncode = -signum`
- `FileNotFoundError(ENOENT)` for missing executables
- `subprocess.check_call`, `check_output`
- `Popen` context manager
- `env=` + `cwd=` propagation
- `capture_output=True` for stdout + stderr (50 KiB payloads tested)
- `Popen.terminate()` / `kill()` / `send_signal(15)` → correct `-signum` returncodes
- Send-signal on dead child is silent no-op
- Two parallel `sleep 1`s finish in ~1.2s (helper backgrounds spawns)
- `subprocess.run(input=...)` via shim-side shell-redirect wrapper
- 256-byte binary input survives unmangled
- Interactive `Popen(stdin=PIPE).stdin.write()` loops via deferred-spawn
- `communicate(input=...)` merges with already-buffered bytes
- `with Popen(stdin=PIPE): pass` clean lifecycle

35+ assertions; **0 failures**.

What's still implicit:

- Default profile still wires the stub component (`v86_posix_stub.wasm`).
  Opt in to the real impl by running `run-python-with-subprocess.sh`,
  or by setting `V86_POSIX_COMPONENT_WASM` in the environment before
  `make python-composed`. Documented gating, not a default change —
  the stub fail-fast on `Popen` is useful for callers who haven't yet
  wired the helper.
- The helper today is a host-side bash/native binary. In a future
  Phase 5+, it moves into a v86 wasmmachine guest's userspace
  ([`~/git/v86/docs/posix-spawn-impl.md`](https://github.com/.../v86/blob/main/docs/posix-spawn-impl.md)).
  The guest-side wiring stays the same — only the backend component
  changes — because v86-posix-host abstracts the helper location
  behind the same `v86:posix/process` interface.

---

## Phase 6 — multiprocessing via reference-worker ✅ DONE (2026-05-29)

Tier 3.2. Built on Phase 4's offload boundary + Phase 5's subprocess
support; no upstream blocking.

What landed:

- **`cpython-ext/_offload_shim/pool.py`** — `OffloadPool` class with the
  multiprocessing.Pool surface (map, imap, apply, close/terminate/join,
  context manager). Backed by N `MailboxClient` instances, each pointing
  at its own host-side worker process.
- **`cpython-ext/_offload_shim/mailbox.py`** — split `MailboxClient.run()`
  into `_submit()` + `_wait_for()`. Lets the pool fan all N requests out
  before waiting on any of them, so host workers run concurrently. The
  old `run()` stays as a convenience that calls both.
- **`scripts/serve-offload-pool.sh`** — host side. Spawns N native python
  `serve_mailbox` processes, each watching its own `mailbox-<i>/` dir
  under a shared root. Lifecycle is via signal handlers + pid files.
- **`scripts/test-offload-pool.sh`** — end-to-end smoke. Stands up a
  4-worker pool + drives 5 scenarios from inside python.composed.wasm:
    - **Parallelism:** 8 × `time.sleep(0.5)` across 4 workers → wall-clock
      ~1.0s (vs. 4s serial).
    - **numpy round-trip:** 4 × `numpy.linalg.det(...)` returns correct
      values.
    - **Exception propagation:** `math.sqrt(-1)` raises ValueError on the
      host, surfaces as ValueError in the guest.
    - **apply:** single-shot `pool.apply("builtins:len", [[1,2,3,4,5]])`.
    - **multiprocessing.Pool wiring:** `multiprocessing.Pool(4).map(...)`
      drops through to OffloadPool transparently.
- **`cpython-ext/_posix_user_shim/sitecustomize.py`** — when
  `OFFLOAD_POOL_DIR` is set in the env, replaces `multiprocessing.Pool`
  with a factory that returns an `OffloadPool` rooted at that dir. Stock
  Python code using `multiprocessing.Pool` works unchanged.

Wall-clock from a real run:

```
PARALLEL: 8 x sleep(0.5s) on 4 workers took 1.04s
NUMPY:    4 x numpy.linalg.det -> [-2.0, 4.0, 1.0, -2.0]
EXC:      ValueError propagated through pool: math domain error
APPLY:    pool.apply('builtins:len', [[1,2,3,4,5]]) -> 5
OK: Phase 6 — multiprocessing-shaped parallelism via offload pool
MP.POOL:  multiprocessing.Pool routed through OffloadPool -> [-2.0…, 4.0]
```

What's in scope vs out:

- ✅ `Pool.map`, `Pool.apply`, `Pool.imap`, exception propagation, with-block,
  multiprocessing.Pool drop-in
- ❌ `initializer=`/`initargs=` — workers are pre-spawned host processes; the
  init step on the host hasn't been mapped onto the offload contract yet
- ❌ ndarrays as args / returns — Phase 3 arrow codec from the native-exec
  plan (msgpack handles only primitives + lists/dicts)
- ❌ `Pool.imap_unordered` — would need response-arrival event from the
  mailbox transport (currently FIFO per worker)
- ❌ girder backend — same OffloadPool, different client_factory. Lands when
  girder integrates per `docs/native-execution-and-parallelism.md` §5

**Exit criterion met.** `multiprocessing.Pool().map(...)` returns correct
results with real parallelism — verified at 4x speedup for 8 tasks across
4 workers.

---

## Phase 7 — Older Python support ✅ DONE (2026-05-29)

Path A from the original framing: backport 3.13's `Tools/wasm/wasi.py`
into 3.12 + one configure glob fix. Smaller than the plan estimated.

What landed:

- **`patches/3.12/0001-backport-wasi-py.patch`** — drops 3.13's
  `Tools/wasm/wasi.py` verbatim into 3.12's `Tools/wasm/`. The copy
  already has both wasip2-targeting fixes we'd already landed on 3.13
  (explicit `--target=wasm32-wasip2`, non-fatal smoke test), so it's
  one self-contained patch instead of three.
- **`patches/3.12/0002-configure-accept-wasip2.patch`** — relaxes
  3.12's host check from `*-*-wasi)` (exact) to `*-*-wasi*)` (glob —
  3.13's form). One character in `configure` + `configure.ac`. Without
  this, configure aborts with "cross build not supported for
  wasm32-unknown-wasip2" before the wasi.py orchestration runs.
- **shim path fallback** in `cpython-ext/_compression/bz2.py` and
  `lzma.py`. 3.14 moved `Lib/_compression.py` →
  `Lib/compression/_common/_streams.py`. The shims now try the new
  path first, fall back to the old one. `compression.zstd` is auto-
  skipped on 3.12 by `install-python-shims` (the conditional was
  already there — it's a 3.14+ stdlib module).
- **`profiles/3.12-current.toml`** — STATUS comment flipped from "NOT
  BUILDABLE" to BUILDABLE with the two patches' rationale.

End-to-end verified:

```
$ PROFILE=3.12-current ./scripts/run-python.sh -c '
  import sys, hashlib, sqlite3, asyncio, gzip, bz2, lzma
  print("Python:", sys.version.split()[0])
  print("sha256:", hashlib.sha256(b"x").hexdigest()[:16])
  print("pbkdf2:", hashlib.pbkdf2_hmac("sha256", b"pw", b"salt", 1000, 16).hex())
  print("sqlite:", sqlite3.connect(":memory:").execute("SELECT sqlite_version()").fetchone()[0])
  print("gzip rt:", gzip.decompress(gzip.compress(b"hello")) == b"hello")
  print("bz2  rt:", bz2.decompress(bz2.compress(b"hello")) == b"hello")
  print("lzma rt:", lzma.decompress(lzma.compress(b"hello")) == b"hello")
  async def f(): await asyncio.sleep(0); return 42
  print("asyncio:", asyncio.run(f()))
  '
Python: 3.12.13
sha256: 2d711642b726b044
pbkdf2: 0a38253555ce37f5c72a6b703f996814
sqlite: 3.53.1
gzip rt: True
bz2  rt: True
lzma rt: True
asyncio: 42
```

3.13 and 3.14 reverified concurrently — the shim's fallback doesn't
regress them (both still build, compose, and run all the same probes).

**Exit criterion met.** `make python-composed PROFILE=3.12-current`
produces an importable composed wasm; `sys.version` reports 3.12.13.

3.11 and earlier stay unsupported. Their `Tools/wasm/wasm_build.py`
hardcodes wasip1 (core wasm + WASI imports); our cap composition
needs Preview 2 components. The wasm-tools component-adapter path
(Path B in the original plan) is still an option if a 3.11 ask
emerges, but hasn't been attempted.

---

## Phase 8 — Recipe-driven extension install ✅ DONE (2026-05-29)

Tier 3.5, revised for the dlopen reality. The original framing
("`pip install _zlib_cap`, import succeeds") is unachievable on wasi-p2
for the same reason Phase 4's wheel pivot was: no dlopen. What's
deliverable is a **recipe-driven rebuild** model — declare extensions
via `pyforge-pkg.toml`, point a build script at the recipes, get a
custom python.composed.wasm with exactly those extensions statically
linked. Same end-user outcome as a pip install ("the module shows up
on `sys.builtin_module_names`"), different mechanism.

What landed:

- **`pyforge-pkg.toml` for all 18 `cpython-ext/<dir>/`**. The spec
  ([`pyforge-pkg-spec.md`](pyforge-pkg-spec.md)) had been on disk for a
  while but only 7 of 18 dirs declared themselves. Now all 18 do —
  11 caps with C extensions + 7 shim-only directories (overlays for
  ctypes, mmap, threading, sitecustomize, …).
- **`scripts/wire-cpython-ext.sh`** — was a hand-edited `EXTS=`
  table; now derives the same list by globbing
  `cpython-ext/*/pyforge-pkg.toml` and parsing `[extension]`. Single
  source of truth per the spec.
- **`scripts/pyforge-pkg-verify.sh` (new)** — validates the manifests
  against on-disk reality: schema, name/version, srcdir matches dir
  name, referenced `c_file`/`gen_import_c`/`gen_import_obj`/`shim`
  paths exist, no orphan directories. Run pre-commit / in CI.
  Currently 18 of 18 pass.
- **`scripts/build-from-pkgs.sh` (new)** — recipe-driven build.
  `--include name1,name2,…` or `--exclude name1,name2,…` opts in or
  out of specific packages; `--variant <name>` lands the output at
  `build/<profile>-<variant>/python.composed.wasm`. The exclusion
  step physically drops the extension's `Modules/<srcdir>/` from
  the wasi cross-build tree, so the next `Tools/wasm/wasi build`
  link actually omits them.
- **`docs/extension-recipe.md` (new)** — covers the install model,
  the tooling, an end-to-end example, and a sketch of what a future
  wheel-shaped distribution wrapper would look like.

Verified end-to-end:

```
$ ./scripts/build-from-pkgs.sh --variant minimal \
      --exclude lz4-cap,openzl-cap,zstd-cap
==>  13M build/default-minimal/python.composed.wasm  (stripped)

$ wasmtime … build/default-minimal/python.composed.wasm -c \
      "for m in '_zstd_cap','_lz4_cap','_openzl_cap': … __import__(m)"
MISS _zstd_cap
MISS _lz4_cap
MISS _openzl_cap
$ wasmtime … build/3.14-current/python.composed.wasm  -c \
      "for m in '_zstd_cap','_lz4_cap','_openzl_cap': … __import__(m)"
HAVE _zstd_cap
HAVE _lz4_cap
HAVE _openzl_cap
```

The 13 MiB variant vs. 16 MiB default = 3 MiB of code that was just the
three excluded codec extensions; their absence is literal, not stub.

**Exit criterion met (revised).** Recipe-driven build produces a custom
python.composed.wasm with a chosen subset of cpython-ext extensions
statically linked. The wheel-shaped pip distribution wrapper is the
next layer up — sketched in `extension-recipe.md`, not blocking.

Out of scope for Phase 8:

- The pip install wrapper itself (the `mypackage-…whl` skeleton in
  `extension-recipe.md`). Would land sources under
  `~/.python-wasm/extensions/` and trigger build-from-pkgs.
- Composectl-emit integration. `compose-python-component.sh` is the
  wac-plug fast path today; `composectl emit build` is still gapped
  upstream per `memory/composectl-is-the-substrate.md`.
- Pattern N (offload-to-v86 native packages) recipes. The spec exists
  ([`pyforge-pkg-native-spec.md`](pyforge-pkg-native-spec.md)); no
  declared packages today.

---

## Suggested execution order

```
Phase 0  (verify)         ─┐
Phase 1  (wheel install)   ├─ active first
Phase 2  (asyncio+TLS)     ┘   (Phase 2 runs in parallel with 1)
Phase 3  (size strip)         ─ after Phase 1, before broadly promoting
Phase 4  (C-ext top 5)        ─ after Phase 1
Phase 5  (v86 subprocess)     ─ upstream-driven
Phase 6  (multiprocessing)    ─ concurrent with Phase 4
Phase 7  (older Python)       ─ deferred; entry on user signal
Phase 8  (pip cpython-ext)    ─ end-state polish
```

**If we have 2–3 weeks** — do Phase 0 + Phase 1 + Phase 2. That's the "pure-Python wheel ecosystem works + async HTTP works + canonical doc is current" stack and it's all bounded.

**If we have 6 weeks** — add Phase 3 (strip) and Phase 4's first two packages (`cffi`, `pydantic-core`).

**If a user blocks on numpy** — that's the Phase 4 conversation in earnest; cost is real and worth pre-discussing scope.

## Out of scope

- Tier 3.4 (tkinter/curses/GUI). Indefinite defer.
- Tier 3.1 (wasi-threads). Upstream-blocked; revisit when the proposal stabilizes.
- Anything requiring `dlopen` in wasip2 directly. Use cpython-ext static linking or `tegmentum:py-offload` instead.

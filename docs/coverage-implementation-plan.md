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

## Phase 4 — C-extension wheel path, scoped narrowly (3–6 weeks for top-5)

Tier 1.2. Open-ended in general; this phase is **scoped to a top-5 list** so it has a clear exit.

Picking the list against what users actually hit:

1. `cffi` (used by many packages, including cryptography)
2. `pydantic-core` (Rust extension — pydantic is everywhere)
3. `cryptography` (depends on `cffi`)
4. `lxml` (libxml2/libxslt)
5. `numpy` (largest payoff but largest cost — may slip)

For each:

- [ ] Build against `wasi-sdk-33` and the python-wasm CPython config; produce `.whl` with the matching ABI tag.
- [ ] Publish to a local `find-links` wheel index under `dist/wheels/`.
- [ ] Add to the smoke-test table.

The `cffi` and `cryptography` pair is the most leveraged starting point; `numpy` may need to fall to Phase 6 if the libopenblas-on-wasm story isn't there.

**Exit criterion.** ≥ 3 of the 5 install + import cleanly via `pip install --find-links dist/wheels/ <pkg>`.

---

## Phase 5 — Subprocess via v86 (track upstream)

Tier 2.1. Upstream-blocked on `~/git/v86/` shipping the real component. Our side is ~1 day once it lands.

- [ ] Watch `~/git/v86/` for component output replacing the stub.
- [ ] Swap `V86_POSIX_COMPONENT_WASM` in the active profile.
- [ ] Verify `subprocess.run(['python', '-c', '...'])` works inside python-wasm.

**Exit criterion.** Stub replaced; `subprocess` tests pass.

---

## Phase 6 — multiprocessing via reference-worker (concurrent with Phase 4)

Tier 3.2. The `reference-worker/` impl of `tegmentum:py-offload` is already underway. Tracking issues #1–#5 in this repo.

- [ ] Land Phase-1+ of `reference-worker/` per `docs/native-execution-and-parallelism.md`.
- [ ] Wire `multiprocessing.Pool` to route through it.

**Exit criterion.** `multiprocessing.Pool().map(...)` returns correct results with real parallelism.

---

## Phase 7 — Older Python support (deferred; option (a))

Per the 2026-05-28 decision: not investing today. Enter this phase only when triggered.

**Entry condition.** A specific user signal that 3.12 (or older) is blocking adoption — not speculative coverage.

**Two paths**, pick one:

- **Path A — backport 3.13's `Tools/wasm/wasi.py` to 3.12.** Self-contained single file, but depends on `configure.ac` and `config.site` shapes that differ across versions. Per-version maintenance.
- **Path B — wasm-tools component adapter.** Build 3.12 as wasip1 with `Tools/wasm/wasm_build.py`, then wrap with `wasm-tools component new <module>.wasm --adapt wasi_snapshot_preview1.command.wasm`. Composition mostly works; some runtime semantics differ.

A third option only worth recording for completeness: **Path C — write a new wasip2 build orchestrator targeting 3.12+.** High cost; rejected unless ≥ 2 versions need it.

`profiles/3.12-current.toml` already exists as the placeholder; pick a path, follow `docs/build-profiles.md` "Per-version patches" section, and add `patches/3.12/*.patch`.

**Exit criterion.** `make python-composed PROFILE=3.12-current` produces an importable composed wasm whose `python -c "print(sys.version)"` reports 3.12.x.

---

## Phase 8 — pip-installable cpython-ext wheels (2 weeks; end-state polish)

Tier 3.5. Closes the loop: the static cpython-ext modules we ship (e.g., `_zlib_cap`) become wheel-installable so external users can opt in.

- [ ] Decide one Pattern-A extension as the pilot — likely `_zstd_cap` since it has the largest API surface.
- [ ] Produce a pyforge manifest and pylon-forge artifact for it.
- [ ] Verify `pip install --find-links … _zstd_cap` against a vanilla python-wasm without it baked in.

**Exit criterion.** One cpython-ext extension installs as a wheel into a python-wasm runtime that didn't have it pre-baked, and imports successfully.

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

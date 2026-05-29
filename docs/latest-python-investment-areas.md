# Investment areas for "deeper coverage of what latest CPython enables"

The alternative to adding older Python versions (see [`older-python-support.md`](older-python-support.md)) is investing where the user-visible gaps actually are on the *latest* version we already ship. This doc inventories those gaps and ranks them by ratio of (user-visible payoff) to (engineering cost).

The frame: most python-wasm adoption friction today isn't "wrong Python version" — it's specific stdlib modules / wheel ecosystem behaviors that don't yet work. Each section below names a gap, why it matters, what the work is, and what unblocks afterward.

## Tier 1 — Highest leverage

### 1.1 — Pure-Python wheel install path

**Gap.** `pip install <pure-Python wheel>` doesn't work transparently in python-wasm today. There's no `pip` in the build, no PyPI fetch path with TLS verification, and no install location wired into `sys.path`. Users have to manually copy wheels into the filesystem and unzip them.

**Why it matters.** A huge fraction of useful Python packages are pure-Python (`requests`, `pyyaml`, `jinja2`, `click`, `pydantic-core` is the exception not the rule). Making `pip install` work transparently for that subset turns python-wasm from "an experimental REPL" into "a Python distribution you can actually run code on."

**What the work is.**
1. Ship `pip` in the composed wasm (already-installed-by-CPython but not on the path the cap-routed `_hashlib.py` understands; need to audit).
2. Verify `pip install <pure-Python wheel>` via `urllib.request.urlopen` over our cap-routed TLS works end-to-end against `pypi.org`.
3. Wire a writable site-packages directory the wasi filesystem can persist (in browser: IndexedDB-backed; in wasmtime CLI: `--dir` mount).
4. Smoke-test against the 50 most-installed pure-Python packages on PyPI.

**Effort.** ~1-2 weeks.

**Unblocks.** Most Python developer workflows where the package ecosystem in scope is pure-Python.

### 1.2 — C-extension wheel compatibility (the big one)

**Gap.** `pip install numpy` fails — and so does anything else with C extensions (lxml, cryptography, Pillow, pyarrow, …). This is the same problem Pyodide spent ~5 years on; it's where most "Python-in-wasm" effort has gone industry-wide.

**Why it matters.** Without C extension support, python-wasm covers a narrow slice of the ecosystem. Data science, web frameworks with C deps, anything touching native I/O — none of it works.

**What the work is.** Roughly three sub-paths, increasing in scope:

1. **Adopt a Pyodide-style wheel build pipeline.** Build precompiled `numpy-*-wasm32-wasip2.whl` etc. that match the CPython ABI we ship. Then `pip install --find-links our-wheel-index numpy` works. Requires building the C extensions against wasi-sdk-33 and our specific CPython config (matching ABI tag). Per-package effort: hours to days depending on dependency surface (some packages need their C deps prebuilt for wasm too).
2. **Build a cpython-ext bridge for the popular packages.** Wrap commonly-needed native APIs as `tegmentum:*` capability components (a `tegmentum:linalg` cap that numpy's C extension can route through). Smaller per-package effort if the native API is small; large if the API is large (numpy's C surface is hundreds of functions).
3. **Long term: WASIX or similar.** A community-driven "wasm distribution of Python wheels" — pyodide is one attempt; py2wasm is another. Track upstream rather than reinvent.

**Effort.** Open-ended. 1-3 months for path (1) covering the top-20 packages. 6+ months for (2) per major package. (3) is mostly waiting + integration.

**Unblocks.** The actual hard ask: "I can run my Python code with my dependencies in wasm." Without this, python-wasm is positioned as "Python for users who only need stdlib + pure-Python deps."

### 1.3 — DNS without wasmtime CLI flags / browser polyfill

**Gap.** Today, `urllib.request.urlopen('https://example.com/')` requires `-S allow-ip-name-lookup` on the wasmtime CLI. In the browser, DNS doesn't work at all (no `wasi:sockets/ip-name-lookup` polyfill that actually resolves names).

**Why it matters.** Without name resolution, most network code fails — even before TLS gets a chance to do its job.

**What the work is.**
- Browser: implement `wasi:sockets/ip-name-lookup` in wasi-polyfill via DNS-over-HTTPS (Cloudflare 1.1.1.1, Google 8.8.8.8). One file, ~100 lines.
- Wasmtime CLI: document the flag, or wrap our `make run` / `scripts/run-python.sh` to pass it by default (already required by `test-ssl-network.sh`).

**Effort.** ~1 day.

**Unblocks.** Make HTTPS just-work without per-runtime flag rituals.

## Tier 2 — Moderate leverage

### 2.1 — Real subprocess via v86-component

**Gap.** `subprocess.Popen` raises `_v86_posix.GuestNotReady`. The v86-posix-stub is a placeholder; the real v86 component isn't shipped yet.

**Why it matters.** Anything that shells out — `git`, build tools, language services like `ruff` / `black`, anything with a CLI tool dep — fails. The number of Python programs that don't shell out is smaller than you'd think.

**What the work is.** Track the v86-component work (in `~/git/v86/`; see `docs/tier1-v86-integration.md`). When the real component lands, swap `V86_POSIX_COMPONENT_WASM` in our profiles and verify.

**Effort.** Few hours on our side once v86 ships. Open-ended on the v86 side.

**Unblocks.** Subprocess-dependent Python tools (most build tools, most language servers).

### 2.2 — asyncio + TLS end-to-end battle-testing

**Gap.** `asyncio` works basically (`asyncio.sleep`, `asyncio.run`, coroutines), and TLS works via `ssl_capability`. Their combination — `asyncio` with TLS via `aiohttp` / `httpx` — is untested. There may be edge cases at the wasi:io/poll → asyncio event loop boundary.

**Why it matters.** Most modern Python web code uses async HTTP. If it works, that's a big chunk of the ecosystem.

**What the work is.**
- Smoke-test `aiohttp.ClientSession()` with TLS.
- Smoke-test `httpx.AsyncClient()`.
- Document gaps; if any, file fixes against the asyncio/ssl boundary.

**Effort.** ~3-5 days.

**Unblocks.** Async Python network code.

### 2.3 — Test sweep refresh (`docs/stdlib-dependency-sweep.md`)

**Gap.** The stdlib coverage audit was last refreshed during the original `componentize-python` plan and reflects pre-Phase-5 state. Phase-5+ wasn't reflected; the deferred items doc says "163 of 169" stdlib modules import, but those numbers are stale.

**Why it matters.** The audit is the canonical "what works, what doesn't" doc users consult. Out-of-date numbers undercut trust.

**What the work is.** Refresh the table, retest each module, capture current state. Possibly find new gaps surfaced by the cap migration work this session.

**Effort.** ~1-2 days.

**Unblocks.** Better self-documentation; possibly surfaces small gaps to fix.

## Tier 3 — Lower leverage (defer until specific ask)

### 3.1 — Threading via wasi-threads

WASI threads is a proposal still stabilizing. Once it lands in wasmtime + wasi-sdk + the runtime story is clear, integrate. Currently our `threading` shim runs `Thread.start()` inline; that's enough for *correctness* of most code (it doesn't deadlock), just not *parallelism*. Users who need actual parallelism are probably using `multiprocessing` (broken) or `concurrent.futures.ProcessPoolExecutor` (broken) — neither of which the wasi-threads work fixes.

Effort: open-ended, mostly upstream-blocked.

### 3.2 — `multiprocessing` via reference-worker

The `reference-worker/` impl of `tegmentum:py-offload` is the start of this story — separate worker processes communicating via WIT capability. Tracking issues #1-#5 in the repo. Once landed, `multiprocessing.Pool` could route through it.

Effort: medium; mostly already in motion.

### 3.3 — Performance and footprint

The composed `python.composed.wasm` is 43 MB. Compressed (`brotli`/`gzip`) it's ~15 MB, which is meh for browser delivery and fine for server runtimes. Reductions worth exploring:

- Strip unused stdlib (test, idlelib, tkinter, turtle, …)
- Strip CPython's compiled-in test modules
- `-Os` everything (cap fleet uses `opt-level = "z"` already; CPython itself doesn't)
- Lazy-load infrequent modules

Effort: ~1 week for a focused size pass; ongoing maintenance to keep it small.

### 3.4 — Tkinter / curses / GUI

Tkinter (Tcl/Tk), curses (terminal), tty/termios — none have realistic wasm paths. Defer indefinitely; they're terminal-emulator / GUI-toolkit asks that don't have a clear runtime home in a wasm-component world.

### 3.5 — Pip-installable wheels from `cpython-ext`

We already ship Pattern A cpython-ext extensions (e.g., `_zlib_cap`) that are statically linked. Long-term, the pyforge spec calls for these to be installable as wheels (`pyforge-pkg-native-spec.md`) so wheel users can opt into them. The infra is partly there (pyforge manifests, pylon-forge tooling) but the package-install-via-pip path isn't yet end-to-end.

Effort: ~2 weeks for a minimal "you can pip install one of our cpython-ext extensions" flow.

## Suggested first investment

If the question is "where do we put 2-3 weeks next," my pick is **1.1 (pure-Python wheel install path) → 1.3 (DNS polyfill) → 2.3 (test sweep refresh)**, in that order. That stack:

- Turns python-wasm into a useful Python distribution for the large pure-Python ecosystem (1.1).
- Removes a per-invocation friction (1.3).
- Refreshes the canonical "what works" doc that consumers reference (2.3).

That's ~2-3 weeks total, all bounded scope, all delivering visible user value. None of it requires upstream coordination.

After that lands, the next investment is determined by what users actually hit. If "I need numpy" is the loud complaint, that's the path-1.2 conversation — a much bigger commitment. If it's "I need to shell out," that's path-2.1 (wait for v86). If it's something we haven't enumerated, that surfaces from real use.

## What this doc is *not*

- A roadmap. It's an inventory of options with rough effort estimates; prioritization should respond to actual user signals, not list ordering.
- A complete list. Other gaps exist (locale, internationalization, signal handling on wasi, …); those felt sub-Tier-3 worth-mentioning.
- A commitment. Each entry is a "if we did this, here's what it would take." Picking which to do is a separate decision.

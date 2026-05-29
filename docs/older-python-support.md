# Supporting older CPython versions in python-wasm

> **Update 2026-05-29 — Phase 7 of [`coverage-implementation-plan.md`](coverage-implementation-plan.md):**
> 3.12 is now buildable via two `patches/3.12/*.patch` files (backport
> of 3.13's `Tools/wasm/wasi.py` + a one-character configure glob
> relaxation). The "3.13 is the floor" framing below predates this
> work; the actual floor is now 3.12.13.

A decision doc on whether to invest in building CPython 3.11 / 3.12 (and below) into the python-wasm cap-composition pipeline. Captured 2026-05-28 after a walk-back attempt found the floor at 3.13.

## TL;DR

**Recommended position: support 3.13 + 3.14 (already shipped), don't invest in older versions today.** The engineering cost is real and recurring; the user benefit is narrow and gated by a much larger separately-blocked problem (wheel-with-C-extension compatibility on wasm).

If a specific user need surfaces — e.g., a partner has an existing 3.11 codebase they need to run unchanged — revisit. The infrastructure to add a version is in place (`profiles/<py>-current.toml`, `patches/<py-minor>/`, version-detecting Makefile rules); only the per-version backport work is missing.

## Status today

| Version | Build orchestrator           | wasip2 support               | python-wasm |
|---------|------------------------------|------------------------------|----|
| 3.14+   | `Tools/wasm/wasi/__main__.py`| yes (with ~15-line patch)    | **builds, default profile** |
| 3.13    | `Tools/wasm/wasi.py`         | yes (with ~15-line patch)    | **builds, `3.13-current` profile** |
| 3.12    | `Tools/wasm/wasm_build.py`   | **no** — hardcodes wasip1    | not buildable |
| 3.11    | `Tools/wasm/wasm_build.py`   | **no** — hardcodes wasip1    | not buildable |
| ≤3.10   | (no WASI tooling)            | n/a                          | not buildable |

The break between 3.13 and 3.12 is structural: 3.12 uses a *different* build orchestrator (`wasm_build.py`) that hardcodes `wasm32-unknown-wasi` (Preview 1, core wasm + WASI imports). Our cap composition pipeline (`wac plug` → component model) needs Preview 2 components and `wasm_build.py` doesn't emit them.

## Paths to add older versions (not recommended; documented for completeness)

1. **Backport 3.13's `wasi.py` as a per-version patch.** The script is self-contained (~370 lines), but depends on `configure.ac` and `config.site` shapes that may have drifted across minors. Per-version effort: ~1-2 days for 3.12 (close enough to 3.13), more for 3.11.

2. **Build as wasip1 + adapter.** Use the older `wasm_build.py` unmodified, then wrap the resulting wasip1 core module via `wasm-tools component new <module>.wasm --adapt wasi_snapshot_preview1.command.wasm`. The wasi-preview1 → preview2 adapter ships with wasi-sdk. Cap composition works mostly; some WASI APIs the adapter polyfills may have subtly different runtime semantics. Per-version effort: ~half a day to a day each, but the runtime-semantics edge cases are an open question per call site.

3. **Bypass the orchestrator entirely.** Drive `configure + make` directly with the right env vars and clang flags. CPython's `configure` is vanilla autoconf — it doesn't fundamentally know wasip2 from wasip1; it just uses whatever clang gets passed in. Effort: similar to (1) but doesn't require a backport patch.

For each of those, the per-version cost is just the build orchestrator. Then there's a tail of:

- C-API drift in `cpython-ext/` extensions (likely small, recent CPython has been stable here)
- `Lib/` shim shape divergence (e.g., `Lib/_compression.py` moved between 3.13 and 3.14 — older versions have *their own* layouts that the shims need to match)
- Stdlib modules our shims overlay that don't yet exist in the older version (`compression.zstd` is 3.14-only; install already skips on 3.13, would skip on 3.12 too)
- wasi-sdk-33 clang vs older CPython C source compatibility (likely small at 3.12/3.11, possibly bigger at 3.10 and below)

## Why not invest

1. **C-extension wheel compatibility is the bigger blocker by far.** Even if 3.11-wasm ships tomorrow, `pip install numpy` doesn't work in either 3.11-wasm or 3.14-wasm. Wheel-with-C-extensions on wasm is a separate, much larger problem (Pyodide has spent years on it). The PyPI compatibility window argument for older Python only holds if you can install C wheels — which you can't, regardless of Python version.

2. **EOL clock.** 3.10 EOL Oct 2026, 3.11 EOL Oct 2027, 3.12 EOL Oct 2028, 3.13 EOL Oct 2029. The work pays off for a few years before each version retires; matrix testing is permanent.

3. **Most python-wasm use cases are forward-looking.** Educational REPLs, in-browser data viz, CLI tools, cap-based-architecture exploration. None have a sharp dependency on 3.11.

4. **Stdlib coverage degrades going back.** `compression.zstd` is 3.14-only. Various `hashlib` additions are 3.12+. PEP 612 / 654 / others. The shim install skips gracefully, but the user sees a stdlib that's incrementally missing things.

5. **The differentiator is the cap architecture, not "any Python.**" Users adopting WIT-typed capability composition are probably also adopting the latest tooling. Pairing a cutting-edge cap fleet with a pinned-old Python is an odd combination.

## Why one might invest (the legitimate cases)

- **Specific partner / consumer ask.** Someone has an existing codebase pinned to 3.11 and wants to run it in wasm without porting. Real, addressable.
- **Pylon-forge's multi-runtime story.** `pylon-pyforge.md` already treats CPython-version as one identity axis, alongside implementation (cpython/pypy/graalpython). Supporting multiple CPython minors fits that frame and is a natural extension of the manifest schema work that's already shipped.
- **Long-tail enterprise.** RHEL 9 ships 3.9; Debian 12 ships 3.11. If python-wasm aims to be a drop-in for system Python, matching what runs on the host matters.

Note that even in these cases, the actual implementation work is the same; only the prioritization changes.

## Decision framework

If the question "should we support 3.11?" comes up:

```
Does someone have a concrete use case that REQUIRES 3.11 specifically,
that they would adopt python-wasm for if it shipped?
├── yes  → invest in 3.11 (then probably 3.12 by extension)
└── no   → keep the floor at 3.13; revisit in a year as 3.14 ages out
```

The infrastructure already absorbs new versions cleanly. The decision is *not* about "can we?" — it's about whether the maintenance cost is justified by a real consumer ask.

## What was attempted, what was kept

- A 3.12 fetch path works (`make fetch-deps PROFILE=3.12-current` clones CPython 3.12.13).
- `profiles/3.12-current.toml` is on disk as a marker + future starting point, with a header noting the not-buildable status.
- No `patches/3.12/` directory yet — that's where the backport would land.
- The Makefile already handles the `wasi.py` vs `wasi/__main__.py` shape detection; it would need one more branch for the `wasm_build.py` shape if we did the wasip1+adapter path.

## What investing in *latest* Python instead would entail

See [`docs/latest-python-investment-areas.md`](latest-python-investment-areas.md) for the alternative.

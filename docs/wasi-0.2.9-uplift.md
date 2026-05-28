# Findings: cap fleet can already use `wasip2 = "1.0.3"` (wasi:*@0.2.9)

This doc started as a 5-phase plan for upgrading the cap fleet to the newer wasip2 Rust crate (emits `wasi:*@0.2.9`). Phase 0 (investigation) found that **the uplift was already supported by every layer of the stack** — wasmtime, jco, and the wasi-polyfill all dispatch @0.2.9 cleanly. The plan's premise — that @0.2.9 imports trap because wasmtime doesn't serve them — was wrong.

The original "trap" symptom was caused entirely by an inverted-bool bug in `cpython-ext/_kdf_cap` (`_kdf_capmodule.c:mod_derive`) that made `PyBytes_FromStringAndSize` dereference uninitialized `out.ptr/out.len`. Once that bug was fixed (`392f9c0`), @0.2.9 cap imports run end-to-end.

This doc now records what was measured, so future cap work doesn't repeat the mistake.

## Measured behavior (2026-05-28, wasmtime 45.0.0, jco 1.8.x)

| Layer | Versions tested | Result |
|---|---|---|
| wasmtime CLI | 45.0.0 | Serves `wasi:*@0.2.0`, `@0.2.4`, `@0.2.6`, `@0.2.9` simultaneously in one composed component. All 22 assertions in `scripts/test-ssl-network.sh` pass with @0.2.9 imports in the binary. |
| jco transpile | `@bytecodealliance/jco@^1.8.0` | Accepts components importing `wasi:random/random@0.2.9` etc. Output `python.js` keys imports under the version-stripped form (`imports['wasi:random/random']`), with per-call-site version tracking in the lift/lower wrappers. |
| wasi-polyfill | `@tegmentum/wasi-polyfill@0.1.0` (current main) | `PluginRegistry` keys by `package/name` (no version) and `Polyfill.getImports({ jcoCompat: true })` returns keys matching jco's expectation. The crypto-random / cli / io / clocks plugins satisfy every @0.2.x caller through the same instance. Verified by `test/integration/python-wasm-0.2.9-coverage.mjs`. |

## Current state of the composed binary

```
python.composed.wasm  wasi imports:  @0.2.0 ×2, @0.2.4 ×25, @0.2.6 ×42, @0.2.9 ×10
  - python.wasm:                      @0.2.6 (wasi-sdk-33 fixed)
  - openssl-component, sqlite-wasm:   @0.2.4
  - compression / crypto-hash / hashing multiplexers:
                                      @0.2.4
  - password-hash-multiplexer:        @0.2.9 (default, no wasip2 pin)
```

End-to-end functional verification at @0.2.9:

- `_kdf_cap.derive('pbkdf2', pw, salt, 32)` → 546 ms, correct output
- `_kdf_cap.derive('scrypt', pw, salt, 32)` → 1680 ms, correct output
- `_kdf_cap.derive('argon2id', pw, salt, 32)` → 22 ms, correct output
- `ssl.get_server_certificate(('example.com', 443))` → 1384-byte PEM
- `urllib.request.urlopen('https://example.com')` → 200 OK
- All existing `make test-*` targets pass

## What the plan got wrong

| Phase | Plan said | Actual |
|---|---|---|
| 0 — investigation | Bisect wasmtime versions; expect a known gap | wasmtime 45 already serves @0.2.9 |
| 1 — wasmtime bump | Bump to a version with @0.2.9 | No bump needed |
| 2 — polyfill @0.2.9 (3-5 days) | Add parallel `src/wasip2-0.2.9/` implementations | Polyfill is version-agnostic by design; zero new code |
| 3 — jco verify | Test transpile + browser load | jco handles @0.2.9 transparently |
| 4 — drop cap pins (2 days) | Walk every cap, remove pins | Only one pin existed (password-hash-mux); single Cargo.toml edit |
| 5 — CPython uplift | Wait for wasi-sdk 34+ | Not needed for cap uplift; multi-version coexistence works |

## What's actually left

Nothing for the @0.2.9 cap uplift itself — done in:

- `password-hash-multiplexer@ea6fce5` (drop `wasip2 = "=1.0.1"` pin)
- `python-wasm@710258c` + `python-wasm@392f9c0` (the inverted-bool fixes that caused the original misdiagnosis)
- `wasi-polyfill@3e46fcd` (integration test pinning the cross-repo behavior so future cap rebuilds don't re-trip the same wrong diagnosis)

## Lessons captured

The cap-vs-shim memory note (`wasm-cap-vs-shim-decision.md`) and the wit-bindgen-c bool-convention note (`wit-bindgen-c-result-bool-convention.md`) were saved during the original work that led here. The takeaway specifically for *this* uplift work goes in this doc so it's findable when someone considers upgrading wasi minor versions across other caps:

**Before assuming a wasi version gap when a cap traps, check the cap's own C/Rust glue for inverted ok/err conventions, uninitialized buffer reads, or other classic UB that masquerades as wasi version mismatch.** The "undefined element: out of bounds table access" trap signature is what `call_indirect` produces when the function table at a given index is empty — but it's also what reading garbage `out.ptr/out.len` into a PyBytes allocation will produce when the bytes-from-string call walks off the end of memory. The wasi version mismatch was the convenient hypothesis; the real bug was simpler.

## Future work (not in scope for this doc)

- **wasi-sdk 34+ when it ships** — would let python.wasm itself emit @0.2.7+ and eventually let us collapse the composed binary to a single wasi version. Track upstream; no urgency since multi-version coexistence is fine.
- **WASI Preview 3 (`wasip3`)** — separate, larger track. The polyfill already has a `src/wasip3/` skeleton.
- **Aligning all caps to a single wasi version** — purely a binary-size / dep-hygiene cleanup; not blocking anything. Defer until there's a forcing function.

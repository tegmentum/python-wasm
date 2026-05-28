# Plan: fix the per-codec compression caps

Prerequisite for the compression-multiplexer-removal track. The four target caps (`zlib-wasm`, `bzip2-wasm`, `lzma-wasm`, `zstd-wasm`) need to ship working, WIT-typed component artifacts whose exports python-wasm can consume directly through the already-scaffolded `cpython-ext/_zlib_cap` pattern.

Goal: each cap, when built, satisfies its declared world by exporting the WIT interfaces it claims to. `wac plug` should resolve cleanly against python-wasm's import list.

## Current state of each cap

| Cap         | Built? | WIT export wiring | Underlying impl     | Notes |
|-------------|--------|-------------------|---------------------|-------|
| zlib-wasm   | yes    | **broken** (empty world) | C + upstream zlib (vendored) | Build does `wasm-tools component new` on core wasm, never embeds WIT |
| bzip2-wasm  | no     | unknown           | C + libbzip2?       | Repo exists, WIT is `tegmentum:compression-algorithm` shape (LCD) |
| lzma-wasm   | no     | unknown           | unknown             | Same WIT shape as bzip2-wasm |
| zstd-wasm   | no     | unknown           | C + zstd-c?         | Different WIT (`component:zstd-wasm`), flat funcs no resources |

The current WITs of bzip2-wasm and lzma-wasm (`tegmentum:compression-algorithm/compression-provider`) are the LCD shape that motivated the multiplexer-removal in the first place; using them gains nothing over the multiplexer. zstd-wasm's shape is yet another design. **None of the four caps' WITs are stdlib-equivalent today.**

## Phases

### Phase A — WIT standardization across the four codecs (design pass, ~2 days)

Pick a single per-codec shape and apply it to all four cap repos. Recommendation: model after zlib-wasm's existing structure (`simple` + `streaming` + `info`, plus codec-specific extras).

Each codec cap declares its WIT under one umbrella package per codec, with interfaces named consistently:

```
zlib:compression@0.1.0/      simple, streaming, checksum, info
bzip2:compression@0.1.0/     simple, streaming, info
lzma:compression@0.1.0/      simple, streaming, info, advanced  (format/check/preset/filters/memlimit)
zstd:compression@0.1.0/      simple, streaming, info, advanced  (dict/training/params)
```

The `simple` interface is identical across codecs (one-shot compress/decompress + compress-bound). The `streaming` interface uses a `compressor`/`decompressor` resource pair with `compress-chunk`/`decompress-chunk`/`total-in`/`total-out`/`reset`. Codec-specific options go in a per-codec `advanced` interface.

**Deliverable**: a `wit/` tree per cap with the agreed shape, vendored consistently into `cpython-ext/_<codec>_cap/wit/deps/`.

### Phase B — Fix zlib-wasm component build (~1 day)

Most contained because the impl already exists; only the WIT wrapping is broken.

1. Generate canonical-ABI bindings in the cap repo:
   ```sh
   cd ~/git/zlib-wasm
   wit-bindgen c --world zlib-component wit/ -o gen/
   ```
2. Adapt `src/bindings.c` to satisfy the `gen/` signatures, OR replace it with a thin adapter layer that calls the existing zlib API behind the wit-bindgen entrypoints.
3. Update `CMakeLists.txt` to compile `gen/zlib_component.c` and `gen/zlib_component_component_type.o` alongside `src/bindings.c`.
4. Drop the bare `wasm-tools component new` step in `scripts/build.sh` — wit-bindgen already produces a proper component when the generated `_component_type.o` is linked in.
5. Verify: `wasm-tools component wit build/bin/zlib.component.wasm` shows a non-empty world matching the WIT.
6. Verify: `wac plug python.wasm --plug zlib.component.wasm -o composed.wasm` resolves the `zlib:compression/*` imports.

**Exit criteria**: `cpython-ext/_zlib_cap` (already scaffolded) plugs in cleanly, the integration test in `scripts/test-compression-extension.sh` (extended with a `_zlib_cap.deflate_compress` path) passes.

### Phase C — Build bzip2-wasm with the standardized WIT (~2-3 days)

Greenfield in everything except the codec choice. Recommendation: **rewrite as Rust + cargo-component** (rather than salvaging the current C build). cargo-component handles the WIT export wiring automatically; the bzip2-rs / bzip2 crate provides the codec.

1. New `src/lib.rs` in `~/git/bzip2-wasm/` with `wit_bindgen::generate!` against the Phase A WIT.
2. Implement the trait the WIT generates, backing each method with `bzip2::*` crate calls.
3. Replicate the wasi-sdk-33 cargo config from compression-multiplexer (so the resulting wasm has compatible wasi version imports).
4. `cargo component build --release --target wasm32-wasip2` → component output.
5. Verify with `wasm-tools component wit`.

**Exit criteria**: bzip2-wasm component exports the WIT, plugs cleanly into a test composition.

### Phase D — Build lzma-wasm (~2-3 days)

Same shape as Phase C but using the `xz2` or `liblzma-sys` crate. lzma has substantial advanced-parameter surface (format/check/preset/filters/memlimit); these go in the `advanced` interface defined in Phase A.

The `xz2` crate covers compress/decompress on the standard XZ container. For raw LZMA / LZMA-alone format, the impl falls back to the lower-level `lzma-sys` bindings.

### Phase E — Build zstd-wasm (~2-3 days)

Same shape with the `zstd` crate (or `zstd-safe`). Zstd has the richest codec-specific surface: dictionaries (`ZstdDict`), dictionary training, advanced parameters. The current zstd-wasm repo's flat func shape doesn't have any of this — full rewrite.

The current `compression-multiplexer` already exposes some of this via its `zstd-extras` interface (compress-with-dict, decompress-with-dict, train-dict, finalize-dict, get-frame-size, compress-advanced, decompress-advanced, compress-advanced-with-dict, decompress-advanced-with-dict). Lift those signatures into the new zstd-wasm `advanced` interface.

### Phase F — Migrate python-wasm to per-codec caps (~1 day, parallelizes across codecs)

Once each cap ships a working component:

1. Uncomment `_zlib_cap` in `wire-cpython-ext.sh` (already scaffolded).
2. Re-enable `ZLIB_COMPONENT` in `compose-python-component.sh`.
3. Repeat for `_bz2_cap`, `_lzma_cap`, `_zstd_cap` (each a clone of `_zlib_cap`'s shape with a different cap-side WIT vendored).
4. Update `cpython-ext/_compression/zlib.py` / `bz2.py` / `lzma.py` / `zstd.py` shims to call the per-codec extensions.
5. Once all four codecs have migrated: delete the `_compression/_compressionmodule.c` extension and remove `compression-multiplexer` from the plug list.

### Phase G — Retire the compression-multiplexer plug (~½ day, last)

When no cpython-ext extension imports `tegmentum:compression-multiplexer` anymore:

1. Remove `MUX_COMPRESSION` from `compose-python-component.sh`.
2. Remove `compression_multiplexer` from `profiles/*.toml`.
3. Update `docs/cap-fidelity-audit.md` and the relevant memory notes.

The cap repo (`~/git/compression-multiplexer/`) stays intact for other consumers; this is just python-wasm decoupling from it.

## External blockers (parking lot)

| Blocker | Who fixes | Workaround |
|---|---|---|
| zlib-wasm component-build pipeline | upstream cap maintainer | Phase B fix |
| bzip2/lzma/zstd-wasm not built | upstream cap maintainer or us | Phases C/D/E |
| WIT design across the four caps | needs alignment | Phase A |

## Effort estimate

| Phase | Effort | Critical path |
|---|---|---|
| A — WIT standardization | 2 days | yes (gates B-E) |
| B — fix zlib-wasm | 1 day | yes (first proof-of-concept) |
| C — build bzip2-wasm | 2-3 days | no (parallel with D/E) |
| D — build lzma-wasm | 2-3 days | no |
| E — build zstd-wasm | 2-3 days | no |
| F — python-wasm migration | 1 day (per codec, parallelizable) | last |
| G — retire compression-mux | ½ day | last |

Total to fully decouple from compression-multiplexer: **~2 calendar weeks** with sequential cap work, ~1 week with parallel cap builds.

## Realistic scope alternatives

1. **All four codecs** (above) — ~2 weeks, full multiplexer retirement for compression.
2. **zlib only** (Phases A+B+F partial) — ~3 days, ships the proof-of-concept and migrates `import zlib` / `import gzip` / `import zipfile` / `import tarfile` (which are the high-traffic stdlib consumers); `import bz2`/`lzma`/`compression.zstd` continue going through the multiplexer until C/D/E catch up.
3. **Skip the migration** — accept the multiplexer's LCD fidelity for now; defer until there's a concrete stdlib-parity ask.

Option 2 (zlib-first) is the highest-value-per-day path if the goal is "make stdlib zlib/gzip work properly without losing CPython fidelity." Option 1 is the right scope if the goal is "remove the compression-multiplexer entirely."

## Suggested next move

Phase A and B as a single workstream: design the standardized WIT shape, then immediately validate it by getting zlib-wasm to ship its WIT-typed exports. That produces a working end-to-end demo (multiplexer → zlib-wasm direct) we can show before committing to the bz2/lzma/zstd rebuilds.

If Phase B reveals more upstream-cap-side breakage than expected, the project re-scopes to "fix one cap first" rather than committing to all four in parallel.

# py-runner — DEPRECATED (componentize-py model retired)

> **Superseded by the Pattern A forge itself.** The composed python.wasm
> forge IS the runner: `_compression`, `_crypto_hash`, `_xxhash`, `_ssl`,
> `_sqlite_capability`, and `_v86_posix` are all baked in via cpython-ext
> static linkage, and user scripts get them via plain `import zlib` /
> `import hashlib` / etc. See [`docs/componentize-python.md`](../../docs/componentize-python.md)
> and [`../DEPRECATED.md`](../DEPRECATED.md).
>
> This binding is kept as the original proof that a componentize-py
> CPython component could route stdlib-style imports through a composed
> wasm capability. It no longer builds in CI.

## What this was

A general script runner that exposed the wasm `zlib` capability as if it
were the stdlib `zlib`. Any Python program run through it got
`import zlib` routed **in-process** through the compression-multiplexer
wasm component — Tier 0, all WebAssembly, no host execution.

`bindings/pyzlib` proved the model for a single fixed app
(`composed.wasm` ran its bundled demo). `py-runner` generalized that to
*arbitrary* user scripts: componentize-py built a CPython component with
a `wasi:cli/command`-shaped world that ALSO imported the compression
dispatcher; the runner installed `pyzlib` as `zlib` in `sys.modules`,
then `exec`'d the user's script. So any program's `import zlib` was
served by the wasm multiplexer (Tier 0).

## Historical build + run

```sh
./build.sh                                  # produced runner.composed.wasm
wasmtime run --dir /tmp/pyrun::/work \
    runner.composed.wasm /work/your-script.py [args...]
```

`build.sh` honored `COMPRESSION_MULTIPLEXER_WASM` if the multiplexer
lived somewhere other than the default
`~/git/compression-multiplexer/.../release/`.

## Why this is now obsolete

The Pattern A forge bakes `_compression` + `Lib/zlib.py` (and the other
capability extensions) into `python.composed.wasm` directly. There's
nothing for py-runner to install into `sys.modules` — the import path
already works. The forge runs arbitrary Python scripts via `wasi:cli/run`
and every capability-backed stdlib module is present from import zero.

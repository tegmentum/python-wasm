# py-runner — Tier-0 in-wasm offload for arbitrary Python scripts

A general script runner that exposes the wasm `zlib` capability as if it were
the stdlib `zlib`. Any Python program run through it gets `import zlib` routed
**in-process** through the compression-multiplexer wasm component — Tier 0, all
WebAssembly, no host execution.

## Why

`bindings/pyzlib` proves the model for a single fixed app (`composed.wasm` runs
its bundled demo). `py-runner` generalizes that to *arbitrary* user scripts:
componentize-py builds a CPython component with a `wasi:cli/command`-shaped
world that ALSO imports the compression dispatcher; the runner installs `pyzlib`
as `zlib` in `sys.modules`, then `exec`s the user's script. So any program's
`import zlib` is served by the wasm multiplexer (Tier 0).

## Build + run

```sh
./build.sh                                  # produces runner.composed.wasm
wasmtime run --dir /tmp/pyrun::/work \
    runner.composed.wasm /work/your-script.py [args...]
```

`build.sh` honors `COMPRESSION_MULTIPLEXER_WASM` if the multiplexer lives
somewhere other than the default `~/git/compression-multiplexer/.../release/`.

## Example

`examples/zlib_demo.py` calls `zlib.compress`/`decompress`/`crc32` and prints
the deflate header (`789c` confirms the shim, not a builtin, did the work):

```
runner on wasi (py 3.14.0)
  original   : 296 bytes
  compressed : 52 bytes, header=0x789c
  roundtrip  : True
  crc32      : 0x9a35eae0
  zlib.__name__ = pyzlib
```

## How

- `wit/world.wit` — `world runner { import compression-dispatcher; export wasi:cli/run }`
- `pyzlib.py` — the multiplexer-backed `zlib` shim (copied from `bindings/pyzlib`)
- `app.py` — installs `sys.modules["zlib"] = pyzlib`, then `exec()`s argv[1]
- `build.sh` — componentize-py + wac plug

This is the Tier-0 generalization companion to `bindings/pyzlib`'s
single-app worker: same compression-dispatcher capability, same in-process
WIT path, but a runnable interpreter for arbitrary user code.

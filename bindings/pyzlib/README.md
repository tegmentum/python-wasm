# pyzlib — DEPRECATED (componentize-py model retired)

> **Superseded by `cpython-ext/_compression/` + `Lib/zlib.py` shim** baked
> into the forge. See [`docs/componentize-python.md`](../../docs/componentize-python.md)
> and [`../DEPRECATED.md`](../DEPRECATED.md).
>
> The python-package registry (`registry/packages.json`) used to point at
> `composed.wasm` here; it now points at the composed python.wasm forge
> artifact that has `_compression` statically linked. This binding is kept
> as the original correctness oracle and as the historical proof that the
> capability-composition model worked end-to-end.

## What this was

python-wasm's `zlib` support without statically linking libz: a
stdlib-compatible `zlib` module (`pyzlib.py`, installed as `zlib`) that
routed DEFLATE to the **compression multiplexer** capability component
(`tegmentum:compression-multiplexer/compression-dispatcher`) over WIT.
Mirrored `bindings/pyhash` (which consumed the hashing multiplexer).

`compress` emitted genuine zlib format (RFC 1950: 2-byte header, raw
DEFLATE, Adler-32 trailer); `decompress` read zlib or raw DEFLATE
(negative wbits). `crc32` and `adler32` were provided too.

## Historical build

```sh
componentize-py -d wit -w zlib-demo componentize app -o pyzlib.wasm
wac plug pyzlib.wasm \
  --plug ~/git/compression-multiplexer/target/wasm32-wasip2/release/compression_multiplexer.wasm \
  -o composed.wasm
```

`composed.wasm`'s sha256 was the in-wasm backend `env` originally
registered for `zlib`. Verified via `jco` (round-trip + raw-deflate
path; `header=789c` confirmed the shim, not the builtin zlib, did the
work).

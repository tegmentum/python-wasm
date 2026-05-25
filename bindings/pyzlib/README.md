# pyzlib — `zlib` over the compression multiplexer

python-wasm's `zlib` support without statically linking libz: a stdlib-compatible
`zlib` module (`pyzlib.py`, installed as `zlib`) that routes DEFLATE to the
already-built **compression multiplexer** capability component
(`tegmentum:compression-multiplexer/compression-dispatcher`) over WIT. Mirrors
`bindings/pyhash` (which consumes the hashing multiplexer).

`compress` emits genuine zlib format (RFC 1950: 2-byte header, raw DEFLATE,
Adler-32 trailer); `decompress` reads zlib or raw DEFLATE (negative wbits). `crc32`
and `adler32` are provided too.

## Build

```sh
componentize-py -d wit -w zlib-demo componentize app -o pyzlib.wasm
wac plug pyzlib.wasm \
  --plug ~/git/compression-multiplexer/target/wasm32-wasip2/release/compression_multiplexer.wasm \
  -o composed.wasm
```

`composed.wasm` is the self-contained worker; its sha256 is the in-wasm backend
`env` registered for `zlib` in [`registry/packages.json`](../../registry/packages.json).
Verified via `jco` (round-trip + raw-deflate path; `header=789c` confirms the shim,
not the builtin zlib, did the work).

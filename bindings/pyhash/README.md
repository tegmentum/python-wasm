# pyhash — DEPRECATED (componentize-py model retired)

> **Superseded by `cpython-ext/_xxhash/` and `cpython-ext/_crypto_hash/`**
> baked into the forge. See [`docs/componentize-python.md`](../../docs/componentize-python.md)
> and [`../DEPRECATED.md`](../DEPRECATED.md).
>
> This binding is kept as the original proof of the
> hashing-multiplexer + Python integration. It no longer builds in CI.

## What this was

A componentize-py CPython worker importing
`tegmentum:hashing-multiplexer/hashing-dispatcher`, exposing the
multiplexer's algorithms (`xxh64`, `crc32`, `murmur3`, `blake3`, plus a
streaming `Hasher`) as a Python `pyhash` module. Mirrored
`bindings/pyzlib`'s shape: `app.py` was the wit-world entrypoint
(`wit_world.WitWorld.run()`), `pyhash.py` adapted the WIT-generated
client into a stdlib-feeling API, and `wac plug` composed
`pyhash.wasm` with the hashing multiplexer into `composed.wasm`.

## Successors in the Pattern A forge

| Capability | cpython-ext extension | Python surface |
|---|---|---|
| xxhash / blake3 / crc32 / murmur (non-crypto) | `cpython-ext/_xxhash/` | new `xxhash` module |
| sha256 / sha512 / blake2 (cryptographic) | `cpython-ext/_crypto_hash/` | replaces `_hashlib` behind `hashlib` |

Both are statically linked into `python.composed.wasm`; user code uses
them via `import hashlib` / `import xxhash` with zero awareness of the
underlying capability composition.

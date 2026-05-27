# Python Package Registry (materialized)

Data materialization of the `tegmentum:py-offload` **`registry`** catalog
(`wit/py-package.wit`): maps a Python package name to the worker **backends** that
can serve it — each identified by the **composectl content digest** of the composed
worker artifact — plus the offload-able **entries**. This is the python-facing
source of truth a driver consults before building a composectl plan; composition
and execution remain composectl's job.

`packages.json` mirrors `registry.list-packages()` (`list<manifest>`).

## Backend `env` semantics

Per `wit/py-package.wit`, `env` is `sha256(composed-worker-artifact)`. Two
patterns produce that artifact:

* **Pattern A — cpython-ext static linkage (canonical).** The cap is
  consumed by a hand-written C extension under `cpython-ext/_<cap>/`
  (with wit-bindgen-c bindings) and the Python surface ships as a
  `Lib/<module>.py` shim baked into the forge at build time. The
  "composed worker artifact" is the full `python.composed.wasm` —
  the interpreter IS the worker, and a single forge build serves
  many entries. `env` = `sha256(python.composed.wasm)`. Examples:
  `_compression` → `zlib`, `_crypto_hash` → `hashlib`, `_ssl` → `ssl`,
  `_sqlite_capability` → `sqlite3`, `_xxhash` → `xxhash`.

* **Native-tier offload — composectl worker.** A native package runs in
  a v86 guest (or remote endpoint) composed by composectl from a plan
  describing the v86 component + the package's venv. The "composed
  worker artifact" is composectl's emit output; `env` is its sha256.

The retired Pattern B (componentize-py composed worker per package) is no
longer used — see [`../bindings/DEPRECATED.md`](../bindings/DEPRECATED.md)
and [`../docs/componentize-python.md`](../docs/componentize-python.md).

## Available packages

### `zlib`

python-wasm's `zlib` support, provided by the already-built **compression
multiplexer** capability composed into the forge. The Python surface is
`Lib/zlib.py` (the stdlib API) routing DEFLATE (with RFC 1950 framing +
Adler-32) through `cpython-ext/_compression`, which calls
`tegmentum:compression-multiplexer/compression-dispatcher` over wit-bindgen-c
bindings. No static libz.

- in-wasm backend `env` = `sha256(build/python.composed.wasm)` of the
  current browser forge build.
- A v86 backend will be added when the v86 variant's `python.composed.wasm`
  is built and digested.
- Adding a package here is a catalog edit performed after the forge build
  whose artifact hash you want to register has been produced.

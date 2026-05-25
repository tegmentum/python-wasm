# Python Package Registry (materialized)

Data materialization of the `tegmentum:py-offload` **`registry`** catalog
(`wit/py-package.wit`): maps a Python package name to the worker **backends** that
can serve it — each identified by the **composectl content digest** of the composed
worker artifact — plus the offload-able **entries**. This is the python-facing
source of truth a driver consults before building a composectl plan; composition
and execution remain composectl's job.

`packages.json` mirrors `registry.list-packages()` (`list<manifest>`).

## Available packages

### `zlib`
python-wasm's `zlib` support, provided by the already-built **compression
multiplexer** rather than a statically linked libz. The `bindings/pyzlib`
componentize-py binding installs a stdlib-style `zlib` module that routes DEFLATE
(with RFC 1950 framing + Adler-32) to
`tegmentum:compression-multiplexer/compression-dispatcher`, composed in via `wac`.

- in-wasm backend `env` = `sha256(bindings/pyzlib/composed.wasm)`.
- Add a package here as a catalog edit when its composed worker is built.

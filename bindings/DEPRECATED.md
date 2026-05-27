# `bindings/` — DEPRECATED

This directory contains **componentize-py-based** Python worker bindings
that were superseded by the Pattern A (cpython-ext static linkage) model
documented in [`docs/componentize-python.md`](../docs/componentize-python.md).

## Why retired

1. `componentize-py` pins its bundled CPython version. We control which
   CPython version we ship; staying free of any tool's CPython pin was an
   explicit driving objective (see `docs/componentize-python.md` §"Two
   driving objectives").
2. The Pattern A model (cpython-ext static linkage + `Lib/` shim baked
   into the forge) is the canonical way for Python to consume tegmentum
   capabilities. Examples:
   - `cpython-ext/_compression/` → backs stdlib `zlib`
   - `cpython-ext/_crypto_hash/` → backs `hashlib`
   - `cpython-ext/_xxhash/` → new `xxhash` module
   - `cpython-ext/_ssl/` → backs `ssl`
   - `cpython-ext/_sqlite_capability/` → backs `sqlite3`
   - `cpython-ext/_v86_posix/` → v86 PosIX bridge

## What lives here

| Dir | What it was | Pattern A successor |
|---|---|---|
| `pyzlib/` | `zlib` over compression-multiplexer via componentize-py | `cpython-ext/_compression/` + `Lib/zlib.py` shim baked into the forge |
| `pyhash/` | hashing-multiplexer demo via componentize-py | `cpython-ext/_xxhash/` (and `_crypto_hash/` for stdlib hashes) |
| `py-runner/` | generalized Python script runner (componentize-py) | The forge IS the runner; capabilities are baked in |

## Status

Files kept for historical reference and as the original byte-for-byte
correctness oracles that proved the capability composition model. They
no longer build in CI and should not be referenced by new work. Plan to
remove after one release confirms no consumer needs them.

The python-package registry (`registry/packages.json`) no longer points
at any artifact under this directory — see `registry/README.md` for the
Pattern A registry model.

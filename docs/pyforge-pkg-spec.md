# `pyforge-pkg.toml` — capability-backed Python package spec

A per-extension declarative manifest describing a Python package that is
served by a tegmentum wasm capability via **Pattern A** (cpython-ext
static linkage + `Lib/` shim baked into the forge). One file per
extension at `cpython-ext/<srcdir>/pyforge-pkg.toml`.

This file is **the single source of truth** for the extension. The
pylon-forge manifest's `[[stdlib_overlay]]` and `[[capabilities.required]]`
arrays, the `scripts/wire-cpython-ext.sh` `EXTS` table, and the
`registry/packages.json` entries are all **derivable from (or validated
against)** the collection of `pyforge-pkg.toml` files in `cpython-ext/`.

Pattern B (componentize-py per-package worker) is retired; see
[`../bindings/DEPRECATED.md`](../bindings/DEPRECATED.md). Native-tier
(v86 venv worker) packages use a separate spec described in §6.

## 1. Why this exists

Today, adding a new capability-backed stdlib module means hand-editing
three places:

1. `scripts/wire-cpython-ext.sh` — wire the C extension into the CPython
   build via `Modules/Setup.local`.
2. `pylon-forge/manifests/python-wasm-*.toml` — add `[[stdlib_overlay]]`
   for each Lib/ shim and `[[capabilities.required]]` for each WIT import.
3. `registry/packages.json` — add a `dist` entry with `name`, `version`,
   `backends`, and `entries` for offload visibility.

All three encode the same underlying information: "this extension
provides these Python modules, requires these capabilities, and exposes
these offload entries." `pyforge-pkg.toml` consolidates that into one
file, alongside the extension's source.

## 2. File location and discovery

Pylon tooling discovers extensions by globbing
`cpython-ext/*/pyforge-pkg.toml` from the python-wasm repo root. One
file per extension. Subdirectories are not recursed.

A `cpython-ext/<srcdir>/` directory without `pyforge-pkg.toml` is
flagged by `pylon pkg verify` — every extension shipped in the forge
must declare itself.

## 3. Schema

### 3.1 Top-level

```toml
schema = "tegmentum:pylon-pyforge/pkg@0.1.0"
```

A string identifying the schema version. Tooling rejects unknown
schemas with a clear error and a doc link. Current version: `0.1.0`.

### 3.2 `[package]` — identity

```toml
[package]
name = "compression"
version = "0.1.0"
description = "Stdlib compression modules over the compression-multiplexer capability"
pattern = "A"
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `name` | string (kebab-case) | yes | Human-friendly id for the **extension as a unit**. Not the Python module name (one extension can provide several). |
| `version` | string (PEP 440) | yes | Version of the extension + its shims. Bumped when the shim API or extension semantics change. |
| `description` | string | yes | One-line summary. |
| `pattern` | `"A"` | yes | Currently only `"A"` is defined for in-wasm-tier packages. Reserved for future patterns. |

### 3.3 `[extension]` — build integration

How the C extension gets statically linked into `python.wasm`. Mirrors
the `EXTS` table in `scripts/wire-cpython-ext.sh` so the script can be
generated from this section.

```toml
[extension]
srcdir = "_compression"
module_name = "_compress_cap"
c_file = "_compressionmodule.c"
wit_dir = "wit"
gen_dir = "gen"
gen_import_c = "compression_import.c"
gen_import_obj = "compression_import_component_type.o"
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `srcdir` | string | yes | This directory's basename under `cpython-ext/`. Used as the symlink name in `deps/cpython/Modules/`. |
| `module_name` | string | yes | The C symbol `PyInit_<module_name>` exports. May differ from `srcdir` (e.g., `_ssl` → `_ssl_capability`). |
| `c_file` | string | yes | Path under `srcdir/` to the main `.c` file. Relative. |
| `wit_dir` | string | optional, default `"wit"` | Subdir holding hand-written `world.wit` + `deps/`. |
| `gen_dir` | string | optional, default `"gen"` | Subdir holding cached wit-bindgen-c output (committed). |
| `gen_import_c` | string | yes | Filename under `gen_dir/` of the generated `<world>_import.c`. |
| `gen_import_obj` | string | yes | Filename under `gen_dir/` of the precompiled `<world>_import_component_type.o`. |

Convention: for a srcdir `_<name>`, the canonical names are
`<name>_import.c` and `<name>_import_component_type.o`. Tooling
validates that the declared names follow convention unless a
`gen_naming = "explicit"` escape hatch is set (not defined in 0.1.0).

### 3.4 `[[capabilities.required]]` — WIT imports

```toml
[[capabilities.required]]
interface = "tegmentum:compression-multiplexer/compression-dispatcher"
version = "0.1.0"

[[capabilities.required]]
interface = "tegmentum:compression-multiplexer/zstd-extras"
version = "0.1.0"
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `interface` | string | yes | Fully-qualified WIT interface name: `<package>:<world>/<interface>`. Same shape as `wit/world.wit`'s `import` lines. |
| `version` | string | yes | Semver of the WIT interface (matches the `@<version>` in WIT). |

`pylon pkg verify` cross-checks each declared capability against the
extension's `wit/world.wit` — every `import` must appear here, and
every entry here must appear as an `import` in the WIT.

Forge composition (pylon-forge + composectl) takes the union of all
extensions' required capabilities to populate the forge manifest's
`[[capabilities.required]]` array.

### 3.5 `[[provides]]` — Python surface

Stdlib modules this extension installs into the CPython tree via a
`Lib/` shim. Each entry generates one `[[stdlib_overlay]]` in the
pylon-forge manifest AND one `dist` entry in `registry/packages.json`.

```toml
[[provides]]
module = "zlib"
shim = "zlib.py"
dest = "Lib/zlib.py"
purpose = "Tier A: cap-route zlib through _compress_cap (retires static libz)"
version = "0.1.0"
offload = [
  { callable = "compress",   doc = "Compress bytes to zlib format (RFC 1950) via the DEFLATE codec.", codecs = ["msgpack", "json"] },
  { callable = "decompress", doc = "Decompress zlib (RFC 1950) or raw DEFLATE (negative wbits) bytes.", codecs = ["msgpack", "json"] },
  { callable = "crc32",      doc = "CRC-32 checksum.",   codecs = ["msgpack", "json"] },
  { callable = "adler32",    doc = "Adler-32 checksum.", codecs = ["msgpack", "json"] },
]
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `module` | string | yes | Importable Python module name as the user types it: `zlib`, `bz2`, `compression.zstd`. Becomes `dist.name` in `registry/packages.json`. |
| `shim` | string | conditional | Filename of the shim under this extension's directory. Omit when the module is the C extension itself (see "direct-C provides" below). |
| `dest` | string | conditional | Path in the CPython tree, relative to `deps/cpython/`. Usually `Lib/<module>.py` or `Lib/<pkg>/__init__.py`. Omit alongside `shim`. |
| `purpose` | string | yes | One-line note; copied verbatim into the forge manifest's overlay entry (or describes the direct-C surface). |
| `version` | string (PEP 440) | yes | Version reported in the registry's `dist.version`. May differ from `[package].version` if a shim's external API has its own cadence. |
| `offload` | array of inline tables | optional | Each entry = one offload-able callable for the py-offload registry. |

**Direct-C provides.** When `shim` and `dest` are both omitted, the
extension exposes its Python surface directly via `PyInit_<module_name>` —
no `Lib/` overlay is generated. In that case `module` MUST equal
`[extension].module_name`. `_xxhash` is the reference example: users
`import _xxhash` and the C extension answers without any Python shim.
No `[[stdlib_overlay]]` entry is generated for direct-C provides; the
`packages.json` registry entry still gets generated (module name +
optional offload entries).

#### Offload sub-entries

Each `offload` table:

| Field | Type | Required | Notes |
|---|---|---|---|
| `callable` | string | yes | The Python callable in `module`. Becomes `entries[].entry = "<module>:<callable>"` in `registry/packages.json`. |
| `doc` | string | yes | One-line docstring; copied to the registry entry. |
| `codecs` | array of strings | yes | Accepted codecs (`"msgpack"`, `"json"`, `"arrow"`, `"pickle"`). Empty array = any codec the worker implements. |

Omitting the `offload` array means the package is in-process-only — no
registry-visible offload entries. Reasonable for packages where every
call is trivially in-process (e.g., `bz2` with no remote demand yet).

### 3.6 `[gating]` — forge inclusion

```toml
[gating]
include_forges = []
exclude_forges = []
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `include_forges` | array of strings | optional, default `[]` | When non-empty, only forges with one of these names ship this extension. Empty = all forges. Forge name comes from the manifest filename: `python-wasm-browser.toml` → `"python-wasm-browser"`. |
| `exclude_forges` | array of strings | optional, default `[]` | Forges to skip. Useful for env-conditional caps (e.g., `_v86_posix` excludes `python-wasm-browser`). |

Both can be set; `exclude_forges` wins on collision.

## 4. Derivation rules

`pylon pkg materialize` walks `cpython-ext/*/pyforge-pkg.toml` and
produces three downstream artifacts.

### 4.1 `scripts/wire-cpython-ext.sh` `EXTS` table

For every extension passing forge gating:
```
"<srcdir>|<module_name>|<c_file>|<gen_import_c>|<gen_import_obj>"
```

### 4.2 Pylon-forge manifest

For every passing extension:
- One `[[stdlib_overlay]]` per `[[provides]]` entry, with `src` = `cpython-ext/<srcdir>/<shim>`, `dest` = `<dest>`, `purpose` = `<purpose>`, `sha256` computed at build time from the shim file content.
- The union of `[[capabilities.required]]` across all passing extensions.

### 4.3 `registry/packages.json`

For every passing extension, for every `[[provides]]` entry:
- One `package` entry with:
  - `dist.name` = `<module>`
  - `dist.version` = `<version>`
  - `dist.backends` = `[{tier: "in-wasm", env: sha256(python.composed.wasm)}]` for the forge this materialization targets.
  - `entries` = the `offload` array translated to `{entry: "<module>:<callable>", doc, codecs}`.

When the forge has both a browser and v86 build with different
`python.composed.wasm` digests, the entry's `backends` list contains
one entry per forge variant — same `tier: in-wasm` per the WIT contract.

## 5. Worked example

[`cpython-ext/_compression/pyforge-pkg.toml`](../cpython-ext/_compression/pyforge-pkg.toml)
is the reference implementation. It declares:

- `[package].name = "compression"`, providing zlib + bz2 + lzma +
  compression.zstd.
- `[extension]` matching the existing `wire-cpython-ext.sh` entry
  byte-for-byte.
- Two required capabilities matching `cpython-ext/_compression/wit/world.wit`.
- Four `[[provides]]` entries, one per stdlib module overlaid.
- Offload entries on the `zlib` provide; the others are in-process-only.

## 6. Native-tier (not in 0.1.0)

Packages that run native code in v86 (numpy, scipy, pillow, …) use a
different declarative shape covering the v86 venv layout, the
composectl plan for the worker, and the offload entries the worker
exports. That spec is part of Phase 5.3 (worker composition for the
native path) and lives separately at `docs/pyforge-pkg-native-spec.md`.

A future schema revision (`@0.2.0`) may unify both under one document
with a `pattern` discriminator. For 0.1.0, `pattern = "A"` is the only
defined value and Pattern B / native-tier specs are separate documents.

## 7. Versioning

Document version: `0.1.0`.

- Breaking schema changes increment the major (`@1.0.0`).
- Additive optional fields increment the minor (`@0.2.0`).
- Doc clarifications and worked-example updates increment the patch.

The `schema` top-level field on each pyforge-pkg.toml MUST match the
major+minor that tooling expects, or tooling refuses to materialize.

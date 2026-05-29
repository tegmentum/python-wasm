# Extension recipes — the Phase 8 install model

Status: **Phase 8** of [`coverage-implementation-plan.md`](coverage-implementation-plan.md).

## Why this isn't `pip install`

WASI Preview 2 has no `dlopen`. The standard pip model — "download
`numpy-x.y.z-cp314-cp314-wasm32-wasip2.whl`, drop a `.so` on `sys.path`,
import succeeds" — doesn't work here. Documented in
[`c-ext-wheels.md`](c-ext-wheels.md) and verified end-to-end:
`ctypes.CDLL(None)` raises NotImplementedError; `Tools/wasm/wasi build`
sets `CCSHARED=''` and the wasi cross-build directory contains zero
`.so` files.

The realistic install model for a wasi-p2 extension is **rebuild
python.composed.wasm with the extension statically linked in**. The
inputs to that rebuild are declarative:

1. A `pyforge-pkg.toml` per extension — the [single source of
   truth](pyforge-pkg-spec.md).
2. A driver that selects which extensions to include and runs the
   build/compose pipeline.

This doc covers what the recipes look like, where they live, and how
to use them.

## The recipe = `pyforge-pkg.toml`

Each `cpython-ext/<srcdir>/pyforge-pkg.toml` declares one extension's
identity:

- **`[package]`** — name, version, description, pattern (`A` =
  cpython-ext static link / `N` = native v86 backend / `shim-only` =
  no C extension, just `Lib/` overlays).
- **`[extension]`** — the C source layout (`srcdir`, `module_name`,
  `c_file`, `gen_import_c`, `gen_import_obj`). Absent for `shim-only`
  packages.
- **`[[capabilities.required]]`** — the WIT interfaces the extension
  imports (e.g. `zlib:compression/simple@0.1.0`).
- **`[[provides]]`** — Python modules the extension provides + the
  `Lib/` shim paths that overlay stdlib modules.

All 18 of this repo's `cpython-ext/*/` directories now have one:

```
cpython-ext/
├── _bz2_cap/pyforge-pkg.toml
├── _compression/pyforge-pkg.toml          (shim-only)
├── _crypto_hash/pyforge-pkg.toml
├── _ctypes_shim/pyforge-pkg.toml          (shim-only)
├── _kdf_cap/pyforge-pkg.toml
├── _lz4_cap/pyforge-pkg.toml
├── _lzma_cap/pyforge-pkg.toml
├── _mmap_shim/pyforge-pkg.toml            (shim-only)
├── _offload_shim/pyforge-pkg.toml         (shim-only)
├── _openzl_cap/pyforge-pkg.toml
├── _posix_user_shim/pyforge-pkg.toml      (shim-only)
├── _sqlite_capability/pyforge-pkg.toml
├── _ssl/pyforge-pkg.toml
├── _threading_shim/pyforge-pkg.toml       (shim-only)
├── _v86_posix/pyforge-pkg.toml
├── _xxhash/pyforge-pkg.toml
├── _zlib_cap/pyforge-pkg.toml
└── _zstd_cap/pyforge-pkg.toml
```

## Tooling

### `scripts/wire-cpython-ext.sh`

The build's wiring script. Walks `cpython-ext/*/pyforge-pkg.toml`,
extracts the `[extension]` block, and writes `Modules/Setup.local`
+ `Modules/<srcdir>` symlinks so the next CPython build statically
links each declared extension.

Two filters honor the env:

- `PYFORGE_PKGS_INCLUDE=name1,name2,…` — only wire these
  `[package].name` values.
- `PYFORGE_PKGS_EXCLUDE=name1,name2,…` — wire everything except
  these.

When neither is set, the build includes every extension that has a
`[extension]` block — the default behavior, matching every shipped
profile.

### `scripts/pyforge-pkg-verify.sh`

Validates the 18 manifests for consistency. Checks:

- Schema is known (`tegmentum:pylon-pyforge/pkg@0.1.0`).
- `[package].name`/`.version`/`.pattern` present + valid.
- `[extension].srcdir` matches the containing directory's name.
- Every referenced file (`c_file`, `gen_import_c`, `gen_import_obj`,
  `[[provides]].shim`) exists on disk.
- No orphan `cpython-ext/<dir>/` without a manifest.

Run pre-commit / CI to catch drift between code and declarations.

### `scripts/build-from-pkgs.sh`

The "install" surface. Takes `--include` / `--exclude` lists and a
`--variant` name, runs wire + build + compose, lands the result in
`build/<profile>-<variant>/python.composed.wasm`.

```bash
# A python.composed.wasm without lz4/openzl/zstd codec support.
# Saves ~3 MiB by dropping their static-linked code.
./scripts/build-from-pkgs.sh --variant minimal \
    --exclude lz4-cap,openzl-cap,zstd-cap

# A python.composed.wasm with only zlib + ssl + sqlite (no other caps).
./scripts/build-from-pkgs.sh --variant trio \
    --include zlib-cap,ssl_capability,sqlite-cap

# Default: rebuild build/<profile>/ with everything (= today's behavior).
./scripts/build-from-pkgs.sh
```

The `--exclude` path drops the extension's link inputs (`Modules/<srcdir>/`
under the wasi cross-build dir) before rebuilding so excluded extensions
actually leave the artifact.

## End-to-end example: minimal variant

```bash
$ ./scripts/build-from-pkgs.sh --variant minimal \
      --exclude lz4-cap,openzl-cap,zstd-cap
==> variant:  minimal
==> include:  (all)
==> exclude:  lz4-cap,openzl-cap,zstd-cap
==> output:   build/default-minimal/python.composed.wasm
…
==>  13M /Users/.../build/default-minimal/python.composed.wasm  (stripped)

$ ls -la build/default-minimal/python.composed.wasm build/3.14-current/python.composed.wasm
… 13 MiB minimal
… 16 MiB default

$ wasmtime run … build/default-minimal/python.composed.wasm -c "
for m in ['_zlib_cap','_bz2_cap','_lzma_cap','_zstd_cap','_lz4_cap','_openzl_cap']:
    try: __import__(m); print(f'HAVE {m}')
    except ImportError: print(f'MISS {m}')
"
HAVE _zlib_cap
HAVE _bz2_cap
HAVE _lzma_cap
MISS _zstd_cap
MISS _lz4_cap
MISS _openzl_cap
```

## What about pip integration?

The wheel-shape distribution wrapper is still a future deliverable.
A skeleton would be:

```
mypackage-0.1.0-cp314-cp314-wasm32-wasip2.whl
├── mypackage/__init__.py            (the Lib/ shim, if any)
├── _mypackage_cap/                   (the cpython-ext sources)
│   ├── _mypackage_capmodule.c
│   ├── gen/…
│   └── pyforge-pkg.toml
└── mypackage-0.1.0.dist-info/
    └── METADATA + RECORD
```

A `pip install mypackage-…whl` would land the sources under
`~/.python-wasm/extensions/_mypackage_cap/`, and a follow-up
`python-wasm build-from-pkgs --include …` would consume them. The
install itself **does not** make the extension importable in the
existing python.composed.wasm — that requires the rebuild.

This skeleton hasn't been wired yet. The Phase 8 deliverable is the
recipe-driven build path itself + verification; the wheel-shaped
distributor is a thin wrapper on top.

## See also

- [`pyforge-pkg-spec.md`](pyforge-pkg-spec.md) — the manifest schema.
- [`c-ext-wheels.md`](c-ext-wheels.md) — why dlopen-based wheels don't
  work + the offload alternative.
- [`pylon-pyforge.md`](pylon-pyforge.md) — the build-output (post-compose)
  manifest, content-addressed digests for the composed artifact.
- [`native-execution-and-parallelism.md`](native-execution-and-parallelism.md) — the
  Pattern-N (offload-to-v86) variant the recipes will eventually
  cover too.

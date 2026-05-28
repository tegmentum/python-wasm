# Build profiles

A "profile" is the named set of options applied to a build. Adding a new Python version, a new variant (browser vs CLI vs v86), or any other build-time toggle is now done by writing a new `profiles/*.toml` rather than editing the Makefile.

This doc covers:

- What a profile contains
- How to build with a non-default profile
- How to add a new profile
- The contract between profiles (build inputs) and pylon-forge manifests (build outputs)

## Layout

```
python-wasm/
├── profiles/
│   ├── default.toml          # symlink → 3.14-current.toml
│   ├── 3.14-current.toml     # CPython 3.14.3, v86-enabled, cap-routed
│   ├── 3.13-current.toml     # CPython 3.13.9, otherwise same as 3.14-current
│   └── (future: 3.14-browser.toml, 3.15-current.toml, 3.13-browser.toml, ...)
├── patches/
│   ├── 3.13/                 # patches applied when PYTHON_VERSION is 3.13.x
│   │   ├── 0001-wasip2-target.patch
│   │   └── 0002-non-fatal-smoke-test.patch
│   └── 3.14/
│       └── 0001-wasi-sdk-33-wasip2.patch
├── deps/
│   ├── cpython-3.13/         # per-version source trees
│   ├── cpython-3.14/         #   (each profile points at one of these)
│   └── cpython → cpython-3.14   # back-compat symlink, updated by fetch-cpython.sh
└── build/
    ├── 3.13-current/         # per-profile output dirs
    │   └── python.composed.wasm
    └── 3.14-current/
        └── python.composed.wasm
```

## Using a profile

```bash
make python-composed                            # default profile (3.14-current)
make python-composed PROFILE=3.13-current       # CPython 3.13.9 build
make test-ssl-capability PROFILE=3.13-current   # tests run against build/3.13-current/

make show-profile PROFILE=3.13-current          # debug: dump resolved variables
```

Every script under `scripts/` honors the `PROFILE` env var. Direct invocation works too:

```bash
PROFILE=3.13-current bash scripts/test-ssl-network.sh
```

## What a profile contains

Each profile is a TOML file with five sections:

```toml
schema = "tegmentum:python-wasm/profile@0.1.0"

[meta]
description = "Default python-wasm build — CPython 3.14.3, v86-posix enabled."

[python]
implementation = "cpython"
version        = "3.14.3"
git_tag        = "v3.14.3"      # used by scripts/fetch-cpython.sh
source_dir     = "cpython-3.14" # under deps/

[toolchain]
wasi_sdk_version = "33.0"
wasi_sdk_dir     = "wasi-sdk-33.0-arm64-macos"
host_triple      = "wasm32-wasip2"

[build]
static_openssl  = false   # use cap-routed _ssl_capability instead
static_zlib     = false   # use cap-routed zlib.py shim instead
with_v86_posix  = true    # wire _v86_posix ext + plug v86-posix-stub at compose

[capabilities]
# Override paths to any cap component the compose step plugs in.
compression_multiplexer   = "${HOME}/git/compression-multiplexer/target/.../compression_multiplexer.wasm"
# ... (one per cap)

[output]
build_dir = "build/3.14-current"   # where python.composed.wasm lands
```

`${HOME}` and similar env vars in string values are expanded against the calling shell at load time (see `scripts/load-profile.sh`).

## Adding a new profile

### Same Python, different flags (e.g., browser variant)

1. Copy an existing profile: `cp profiles/3.14-current.toml profiles/3.14-browser.toml`
2. Edit the new file:
   - Change `[meta].description`
   - Set `[build].with_v86_posix = false` (or whatever toggles differ)
   - Change `[output].build_dir = "build/3.14-browser"`
3. Build: `make python-composed PROFILE=3.14-browser`

The same CPython source tree is reused. Only the build flags + compose-step plugs differ.

### New Python version

1. Pick a version (e.g., `3.15.0`).
2. Write `profiles/3.15-current.toml` — change `[python].version`, `[python].git_tag`, `[python].source_dir` (e.g. `cpython-3.15`), and `[output].build_dir`.
3. If the upstream CPython tree's structure differs (which is common across minor versions — see "Per-version patches" below), add `patches/3.15/*.patch` for the corrections.
4. Build:
   ```bash
   make fetch-deps PROFILE=3.15-current   # clones python/cpython @ git_tag into deps/cpython-3.15
   make python-composed PROFILE=3.15-current
   ```

### Per-version patches

CPython's WASI build tooling has been reshuffled multiple times:

| Version | WASI build entrypoint        | Notes                          |
|---------|------------------------------|--------------------------------|
| 3.13    | `Tools/wasm/wasi.py`         | single-file module             |
| 3.14+   | `Tools/wasm/wasi/__main__.py`| package with `__main__.py`     |

Likewise the stdlib `Lib/` reshuffles between minors (e.g., `compression.zstd` added in 3.14; `Lib/_compression.py` moved to `Lib/compression/_common/_streams.py`). So patches and shim install rules are version-keyed:

- Patches live under `patches/<py-minor>/` and are applied by `fetch-cpython.sh` based on the profile's `[python].version`.
- The `install-python-shims` Make target checks for the existence of stdlib target directories before installing a shim, so 3.13 silently skips the zstd shim that only makes sense in 3.14+.

## Relationship to pylon-forge manifests

`profiles/` are *build inputs* — they declare what to build. `~/git/pylon-forge/manifests/python-wasm-<py>-<variant>.toml` are *build outputs* — they describe the resulting artifact's content-addressed identity, with sha256s of every input and the produced `python.composed.wasm`. One profile yields many manifests over time, one per build invocation.

| Profile (input)                  | Manifest (output)                                   |
|----------------------------------|-----------------------------------------------------|
| `profiles/3.14-current.toml`     | `pylon-forge/manifests/python-wasm-3.14-current.toml` |
| `profiles/3.14-browser.toml`     | `pylon-forge/manifests/python-wasm-3.14-browser.toml` |
| `profiles/3.13-current.toml`     | `pylon-forge/manifests/python-wasm-3.13-current.toml` (when emitted) |

After a successful build:

```bash
pylon emit-manifest ./build/3.13-current/python.composed.wasm \
    > ~/git/pylon-forge/manifests/python-wasm-3.13-current.toml
```

See `docs/pylon-pyforge.md` for the manifest schema and `~/git/pylon-forge/README.md` for the emitter.

## Debugging

- `make show-profile [PROFILE=...]` — print every variable the profile resolves to.
- `bash scripts/load-profile.sh <name>` — same thing, as raw `KEY=VALUE` lines.
- `make show-profile PROFILE=3.13-current` followed by checking `$BUILD_DIR/python.composed.wasm` exists — fastest way to verify a profile is reaching the build step.

If a build fails partway through, the most common causes are:

1. The CPython source tree hasn't been fetched for that profile. Run `make fetch-deps PROFILE=...`.
2. The patches don't apply cleanly to the source tree (probably because CPython upstream moved a file the patch targets). Open the failed `.patch` and adapt the diff to match the tree.
3. A cap component the profile expects isn't built at the path the profile gives. Set the matching `${COMPRESSION_MULTIPLEXER_WASM}` etc. env var, or rebuild the sibling cap repo.

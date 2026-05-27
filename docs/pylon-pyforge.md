# Pylon / PyForge — Python-as-Forged-Substrate

A design note for treating a CPython build as a first-class, versioned,
content-addressed substrate, the way `fijivm` treats a JVM build. The
practical motivator is python-wasm: every `python.composed.wasm` we ship is
already a forged artifact in this sense — assembled from a specific
wasi-sdk + specific CPython source + N capability components at specific
digests + a Python-level shim overlay. Today that identity is implicit,
scattered across `Makefile`, `scripts/compose-python-component.sh`, the
`cpython-ext/` tree, and several sibling cap repos. This doc sketches the
explicit version.

## Motivation: why Python needs more granularity than Java

`fijivm` can mostly identify a JVM by `vendor + version + arch`. The Java
ecosystem stabilized its module boundary (the JDK, with `java.*` /
`javax.*` packages and a closed-world JNI ABI) long enough that two
`temurin-21.0.4-aarch64-mac` builds from different sources are effectively
interchangeable for nearly all consumers. The JVM hides its internals
behind a stable runtime interface; bytecode compiled against one
matching JDK runs on another matching one without ceremony.

Python doesn't have that property. Python's "module boundary" leaks:

* **Stdlib layout changes between minor versions, sometimes between
  patches.** Concrete example caught during this codebase's compression
  work: 3.13 had `Lib/_compression.py` (BaseStream helpers used by
  `bz2`/`gzip`/`lzma`). 3.14 deleted it and moved the helpers to
  `Lib/compression/_common/_streams.py`. Code that does
  `from _compression import BaseStream` works on 3.13, breaks on 3.14.
  No deprecation cycle. So an artifact pinned at "cpython-3.14" without
  the specific patch identity can silently desync from a shim layer
  built against the corresponding stdlib shape.

* **ABI tags don't capture build flags.** `cp314-cp314-wasi_wasm32` says
  the major thing but not whether `--with-pydebug`, `--enable-shared`,
  `--without-gil`, `--with-mimalloc`, or `--with-tail-call-interp` was
  in force. A native extension compiled against one set of build flags
  isn't guaranteed to load into a CPython with a different set, even
  when the platform tag matches.

* **The "stdlib" is a moving target across implementations.** PyPy,
  GraalPython, MicroPython, and our cap-routed python-wasm all claim to
  be Python 3.14 but ship different `Lib/` trees, different `_ssl`
  semantics, different `os.fork` availability. A wheel that imports
  `_socket` and assumes blocking semantics will work on three of those
  four. The forge has to know which.

* **Capability imports become part of the runtime contract.**
  Specifically for python-wasm: a wheel that does
  `from compression import zstd` requires that the runtime is composed
  with a component satisfying
  `tegmentum:compression-multiplexer/zstd-extras@0.1.0`. That's not
  reflected in any current PEP 425 tag.

So: a Python build's identity is wider than a JVM build's identity, and
the forge has to expose that width. The point of this doc is to nail
down which axes matter and write them into a manifest schema everything
else can resolve against.

## What counts as substrate identity

Eight axes. Two artifacts with all eight matching are interchangeable;
any difference is a different forged Python.

| Axis | Why it matters | Where it shows up |
|---|---|---|
| **Implementation** | cpython / pypy / graalpython / micropython have observable behavior differences (GIL, module set, exception messages). | `sys.implementation.name` |
| **Exact source identity** | Patch-level minor versions ship `Lib/` reshuffles (`_compression` → `compression._common`) and C ABI shifts. Hash the source tarball or vendored tree. | `sha256(source-tarball)` |
| **ABI tag** | Whether `cp314` matches `cp314t` (free-threading), debug builds, etc. Extensions link against this. | `sys.implementation.cache_tag`, `sysconfig.get_config_var('SOABI')` |
| **Platform tag** | `wasm32_wasip1` vs `wasm32_wasip2` vs `wasm32_wasi_component` — same ABI tag, mutually incompatible artifacts. | `packaging.tags.sys_tags()` |
| **Build flags** | `--with-pydebug`, `--without-gil`, `--enable-shared`, JIT/tail-call interp, mimalloc. Affect extension-load compatibility and observable behavior (allocator pattern, refcount semantics). | `sysconfig.get_config_vars()` |
| **Stdlib layout** | A patch-level move (3.14's `_compression.py` relocation) breaks shims that hardcode paths. Hash the actual `Lib/` tree. | `sha256(Lib/)` post-overlay |
| **Extension ABI** | `Py_LIMITED_API` level, `PEP 657` instruction-level frame data, free-threading C-API compatibility. Native wheels embed this. | `Py_GetVersion`, `Py_LIMITED_API` macro |
| **Capability set** (python-wasm specific) | Which WIT imports the runtime expects to have plugged in. A wheel doing `import sqlite3` needs the `sqlite:wasm/high-level` cap composed in. | `wasm-tools component wit python.wasm` |

Things that are NOT substrate identity:

* The wheel cache directory — that's per-installation, not per-substrate.
* Per-user `site-packages` — installed on top of the substrate.
* `PYTHONPATH` env — runtime configuration, not identity.

## Manifest schema

The forge writes one of these per build. The hash of this file IS the
forge's content-address for the artifact.

```toml
# pyforge-manifest.toml

[python]
implementation       = "cpython"
version              = "3.14.0"
source_tarball_url   = "https://www.python.org/ftp/python/3.14.0/Python-3.14.0.tar.xz"
source_sha256        = "..."
# Forge-side patches applied on top of upstream source; pinned by content.
patches = [
  { path = "patches/0001-wasi-pthread-stubs.patch", sha256 = "..." },
]

[python.config]
abi_tag        = "cp314"
cache_tag      = "cpython-314"
platform_tag   = "wasm32_wasi_component"
configure_args = [
  "--host=wasm32-wasip2",
  "--build=aarch64-apple-darwin",
  "--with-build-python=...",
  "--disable-test-modules",
]
# Free-threading / debug / mimalloc / JIT / tail-call: each affects ABI.
features = {
  free_threading   = false,
  debug            = false,
  mimalloc         = true,
  tail_call_interp = false,
  jit              = false,
}
soabi          = "cpython-314-wasm32-wasi"
ext_suffix     = ".cpython-314-wasm32-wasi.so"

[toolchain]
# Identity of the build toolchain. A different wasi-sdk produces a
# different wasm even from the same source.
wasi_sdk_version = "33.0"
wasi_sdk_sha256  = "..."
clang_version    = "22.0.0"

[wasm]
target              = "wasm32-wasip2"
component_model     = true
wasi_profile        = "0.2.6"
# wasm-component-ld adapter, if any
adapter_sha256      = ""
# Memory budgets baked into the link
initial_memory      = 41943040
max_stack_size      = 16777216

[stdlib]
# Hash of Python's bundled Lib/ as shipped, BEFORE any overlay.
layout_sha256        = "..."
frozen_modules_sha256 = "..."
# Frozen module set: `python -X frozen_modules`
frozen_modules       = ["abc", "codecs", "_collections_abc", ...]

# Forge-installed Python files that shadow stdlib. These are NOT
# pip-installable; they're part of "what this Python is."
[[stdlib_overlay]]
src     = "shims/bz2.py"
dest    = "Lib/bz2.py"
sha256  = "..."
purpose = "Tier A: cap-route bz2 through _compress_cap"

[[stdlib_overlay]]
src     = "shims/sqlite3/__init__.py"
dest    = "Lib/sqlite3/__init__.py"
sha256  = "..."
purpose = "Tier B: cap-route sqlite3 through _sqlite_cap"

# (...one entry per shim)

[capabilities]
# WIT imports the composed runtime expects to have plugged in. The
# resolver uses this set when matching wheels.
required = [
  { interface = "wasi:cli/environment",                              version = "0.2.6" },
  { interface = "wasi:io/streams",                                   version = "0.2.6" },
  { interface = "tegmentum:compression-multiplexer/compression-dispatcher", version = "0.1.0" },
  { interface = "tegmentum:compression-multiplexer/zstd-extras",     version = "0.1.0" },
  { interface = "sqlite:wasm/high-level",                            version = "0.1.0" },
  { interface = "openssl:component/tls",                             version = "0.1.0" },
  # ...
]

# The actual cap-side artifacts composed in. CAS-addressed: a different
# digest is a different substrate (even if the WIT shape matches).
[[capabilities.bound]]
interface  = "tegmentum:compression-multiplexer/compression-dispatcher"
version    = "0.1.0"
component  = "compression_multiplexer.wasm"
sha256     = "..."
source     = "git+ssh://github.com/tegmentum/compression-multiplexer.git#12bb76e"

[[capabilities.bound]]
interface  = "sqlite:wasm/high-level"
version    = "0.1.0"
component  = "sqlite-core.wasm"
sha256     = "..."
source     = "git+ssh://github.com/tegmentum/sqlite-wasm.git#abc1234"

[packaging]
# Wheel-policy: the resolver matches wheels with this tag.
wheel_policy           = "pylon-component-wheel-v1"
native_extension_policy = "external-wasm-component"
# Extra platform tags this forge accepts.
extra_platform_tags    = ["any", "wasm32_wasi_component_v1_pylon1"]

[artifacts]
# Output bundle. Each artifact gets its own content address.
python_wasm        = { path = "python.wasm",         sha256 = "..." }
python_composed    = { path = "python.composed.wasm", sha256 = "..." }
forge_identity     = "..."   # sha256 of this manifest with `forge_identity` cleared
```

The `forge_identity` field is the canonical content-address of the
whole substrate. Two manifests with the same `forge_identity` describe
interchangeable artifacts.

## Directory layout

```
pylon-forge/
├── forges/                      # one dir per forged substrate
│   ├── cpython-3.14.0-wasm32-wasip2-pylon1/
│   │   ├── pyforge-manifest.toml
│   │   ├── artifacts/
│   │   │   ├── python.wasm
│   │   │   └── python.composed.wasm
│   │   └── shims/               # forge-installed Lib/ overlay sources
│   │       ├── bz2.py
│   │       ├── lzma.py
│   │       ├── compression/zstd/__init__.py
│   │       ├── sqlite3/__init__.py
│   │       └── ...
│   ├── cpython-3.13.5-wasm32-wasip2-pylon1/
│   └── cpython-3.12.7-wasm32-wasip1-pylon0/
│
├── interpreters/                # raw CPython source caches
│   ├── cpython/
│   │   ├── 3.12.7/
│   │   ├── 3.13.5/
│   │   └── 3.14.0/
│   └── pypy/
│       └── 7.3.17/
│
├── toolchains/
│   ├── wasi-sdk-33.0/
│   └── wasi-sdk-34.0/
│
├── components/                  # CAS-addressed cap components
│   ├── sha256/
│   │   ├── ab/cd/abcd...wasm
│   │   └── ...
│   └── by-interface/
│       └── tegmentum:compression-multiplexer/
│           ├── compression-dispatcher@0.1.0/
│           │   ├── 12bb76e.json -> ../../../sha256/...
│           │   └── 7e63663.json -> ../../../sha256/...
│
├── wheels/                      # forge-bound wheel cache
│   └── cpython-3.14.0-wasm32-wasip2-pylon1/
│       ├── numpy-2.x-cp314-...whl
│       └── ...
│
├── locks/                       # consumer-side lockfiles
│   └── python-wasm-web-demo.lock.toml
│
└── bin/
    └── pylon                    # CLI
```

Cap components live in a single CAS keyed by sha256, with by-interface
symlinks for fast lookup. Multiple cap versions of the same interface
coexist; the forge that needs them references by sha256.

## CLI

```
pylon forge build cpython 3.14.0 --target wasm32-wasip2 --profile pylon1
pylon forge list
pylon forge show cpython-3.14.0-wasm32-wasip2-pylon1
pylon forge pin   cpython-3.14.0-wasm32-wasip2-pylon1 > lock.toml
pylon forge materialize lock.toml --out ./build/python.composed.wasm
pylon forge gc    --keep-latest=3        # prune unreferenced forges
pylon forge diff  forgeA forgeB          # show what makes them different
pylon resolver wheel-tag cpython-3.14.0-wasm32-wasip2-pylon1
pylon resolver compatible wheel.whl cpython-3.14.0-wasm32-wasip2-pylon1
```

The `forge materialize` verb is the centerpiece for python-wasm. Today,
reproducing the artifact this repo produces requires:

1. Check out python-wasm at commit X
2. Have `~/git/compression-multiplexer` at commit Y locally
3. Have `~/git/sqlite-wasm` at commit Z locally
4. Have `~/git/openssl-wasm/build/openssl-component.wasm` present
5. Run `make build && make python-composed`

`pylon forge materialize lock.toml` should collapse that to one
command, fetching every input from the CAS and producing a binary
identical to the original.

## Resolver contract

A consumer (e.g. a `requirements.lock` resolver) asks the forge:

```
pylon resolver compatible <wheel> <forge_identity>
```

The forge says yes iff:

1. The wheel's `Requires-Python` matches the forge's `python.version`.
2. The wheel's tag matches `cpython-3.14.0-wasm32-wasip2-pylon1`
   (using packaging.tags compat ordering).
3. For wheels with native extensions: the wheel was built against the
   same `(abi_tag, platform_tag, features)` triple.
4. For wheels declaring `Required-Capabilities` in their metadata: every
   declared capability is in the forge's `capabilities.required` set
   AND has a version that satisfies the wheel's constraint.

Point 4 is new and doesn't exist in PEP 425 / 600 land. Proposed
metadata extension for wheels targeting cap-routed Python runtimes:

```
# inside the wheel's METADATA
Required-Capability: tegmentum:compression-multiplexer/zstd-extras (>=0.1.0,<0.2.0)
Required-Capability: sqlite:wasm/high-level (>=0.1.0,<0.2.0)
```

The resolver only matches a wheel to a forge that declares satisfying
imports. A wheel published before its required cap interface exists in
ANY forge can't be installed, which is the correct failure mode.

## Forge build pipeline

For each new forge:

1. **Fetch source.** Download `Python-X.Y.Z.tar.xz`, verify sha256, apply
   pinned patch set.
2. **Materialize toolchain.** Ensure pinned wasi-sdk is in `toolchains/`.
3. **Configure CPython.** Run `./configure` with the manifest's
   `configure_args`. Hash the resulting `pyconfig.h` to detect
   nondeterministic configure outputs (e.g. timestamp injection).
4. **Wire forge-side C extensions.** Symlink `cpython-ext/_<name>/` →
   `Modules/<modname>/`; rewrite `Setup.local` with the per-extension
   entries plus `*disabled*` directives for retired static modules.
   (This is what `scripts/wire-cpython-ext.sh` does today; the forge
   pipeline absorbs it.)
5. **Build CPython.** `Tools/wasm/wasi build`. Produces `python.wasm`.
6. **Compose capability components.** For each
   `capabilities.required` entry that has a `bound` mapping, pull the
   component from the CAS and `wac plug` it. Produces
   `python.composed.wasm`.
7. **Install stdlib overlay.** Copy every `stdlib_overlay` entry into
   the artifact's `Lib/`.
8. **Verify.** Re-hash the manifest's `python_composed`, confirm it
   matches the expected sha256 if pinning; bail otherwise.
9. **Test.** Run the substrate-bound smoke suite (see "Forge-aware
   testing" below).
10. **Publish.** Move the artifact into `forges/<id>/`, append to the
    CAS for the capability components and `python.composed.wasm`.

Steps 1–8 are deterministic given the manifest. Step 9 is the gate;
step 10 is the side effect.

## Federated substrate manager

A forge is not self-contained — it references cap components built in
separate repos. The forge manifest pins those by sha256, but the forge
build still needs to either:

* Find the component in the local CAS, OR
* Build it from source (recursively a forge problem for the cap),

so the forge tool has to know how to drive cap-side builds too. Two
options:

A) **Black-box.** Caps are inputs; the forge fails if a required digest
   isn't in the CAS. A separate tool (`pylon cap build`) populates the
   CAS. Forge gets simpler; cap building is unmanaged.

B) **Recursive forge.** A cap is itself a forge of `(source, rust
   toolchain, wit-bindgen version, wasi target)`. `pylon forge build
   tegmentum:compression-multiplexer 0.1.0` produces the component +
   its own manifest. Forge resolves the recursive build graph.

Recommend B. Today's reality is that `compression-multiplexer` is built
manually with `cargo component build --release --target wasm32-wasip2`
against an absolute-pathed `.cargo/config.toml` (which is why that file
is gitignored). That's already a forge-shaped problem; making it
explicit is cheaper than maintaining two parallel build systems.

## Forge-aware testing

A forge build's smoke suite should be CAS-addressed and run as part of
`pylon forge build`. The suite tests *behavior* against the manifest.
Concrete examples from the python-wasm session that motivated this doc:

* "decompress with dict roundtrips 4500 bytes" — would have caught the
  20× output-buffer undersize bug in `decompress-with-dict` immediately
  on the first cap rebuild, instead of only when a separate test
  happened to use the right input size.

* "lzma.compress(data)[:6] == .xz magic" — would have caught the
  `LzmaWriter` (.lzma legacy) → `XzWriter` (.xz) switch as a behavioral
  delta the moment the cap manifest changed.

* "import bz2; bz2.compress(b'x')" — would have caught the
  `_compression` → `_compress_cap` rename that broke the stdlib's
  `from _compression import BaseStream` path.

The substrate-bound test format:

```toml
# pyforge-tests.toml
[[test]]
id   = "compression.zstd.dict-roundtrip"
gate = "tier-a"
run  = "python -c 'import sys; …'"
expect_exit_code = 0
required_capabilities = [
  "tegmentum:compression-multiplexer/zstd-extras",
]

[[test]]
id   = "sqlite3.file-backed-persists-500"
gate = "tier-b"
run  = "python -c '...'"
required_capabilities = ["sqlite:wasm/high-level"]
```

Tests gate the build. A test that requires a capability the forge
doesn't declare is skipped (not failed).

## Coexistence and selection

Multiple forges live side by side. A consumer selects:

```
$ pylon use cpython-3.14.0-wasm32-wasip2-pylon1
$ python --version
Python 3.14.0 (pylon-forged: ...)
```

Selection sets `PYLON_FORGE` env var; the wrapping `python` binary in
`pylon-forge/bin/` reads it and execs the right `python.composed.wasm`
under the right `wasmtime` (or web runtime) profile.

Per-project pinning:

```
# project root: .pylonrc
[forge]
identity = "cpython-3.14.0-wasm32-wasip2-pylon1"
```

When the resolver materializes `requirements.lock`, it does so against
that forge's `forge_identity`. Different projects on the same machine
can lock to different forges without conflict.

## Mapping to today's python-wasm

Here's what each axis IS today, implicitly, for the artifact this repo
produces:

| Axis | Today's implicit value | Where it's pinned |
|---|---|---|
| implementation | cpython | `deps/cpython/configure.ac` |
| source | 3.14.0 + N forge patches | `scripts/fetch-cpython.sh` (unpinned commit) |
| ABI tag | cp314 | derived from CPython build |
| platform tag | wasm32_wasi_component | implicit in component-new output |
| build flags | wasi-sdk default, mimalloc on | `Tools/wasm/wasi build` defaults |
| stdlib layout | upstream 3.14.0 Lib/ | `deps/cpython/Lib/` |
| stdlib overlay | 6 shim files | `cpython-ext/_*/sqlite3.py`, `bz2.py`, etc. |
| extension ABI | default | `Modules/Setup.local` |
| capabilities | 5 cap components + WIT versions | `scripts/compose-python-component.sh` |
| toolchain | wasi-sdk-33.0-arm64-macos | `Makefile:WASI_SDK_DIR` |

Almost none of these are content-pinned. Reproducing today's
`python.composed.wasm` requires having the same submodule state and the
same versions of cap repos checked out locally. The forge manifest
would make every one of these axes explicit and content-addressed.

Concrete forge identity for the current state of this repo would look
like (using actual digests of the M-status files):

```
forge_identity = sha256("cpython-3.14.0-wasm32-wasip2-pylon1 + capability digests + shim digests + toolchain digest")
```

That string is what `python --version` would emit alongside `3.14.0`.

## Migration path from today

Phase 0: **emit manifests for current builds.** Write a script that
inspects the current `python.composed.wasm` and produces a
`pyforge-manifest.toml` describing it. No build pipeline change; just
make the implicit identity explicit. This is the cheapest test of
whether the schema works.

Phase 1: **adopt manifest as input.** `pylon forge build` reads a
manifest and reproduces the build. Cap components are still built
externally; the forge fetches by sha256.

Phase 2: **recursive cap forges.** `pylon cap build
tegmentum:compression-multiplexer@0.1.0` becomes the canonical way to
build cap components. The python-forge references them by manifest.

Phase 3: **wheel resolver.** Wheels are tagged with required
capabilities. The resolver matches wheels to forges by the full
identity vector. `pip install` (or a pylon-native equivalent) becomes
forge-aware.

Phase 4: **multi-forge coexistence.** `pylon use <id>` selects between
installed forges; project-local `.pylonrc` pins.

## Open questions

* **Where do forge patches live?** Some forge patches are
  python-wasm-specific (e.g. `_v86_posix` extension wiring); others
  might be portable (e.g. a hypothetical "remove all `signal.SIGALRM`
  references" patch for wasi). Probably: the patches live with the
  forge definition, not centrally.

* **How do forges handle CPython security backports?** A 3.14.1 release
  has the same Lib/ layout as 3.14.0 in most cases but ships ABI-
  affecting changes occasionally. Probably: each upstream patch is its
  own forge identity; the manifest schema tracks the relationship via
  `parent_forge_identity`.

* **What about pure-Python forges with no native build?** A
  "cpython-3.14.0-source" forge that's just the source tree, no
  toolchain, no platform tag — useful for code analysis tooling? Or is
  that a separate concept (a "Python distribution") that the forge
  layer doesn't model?

* **How granular is the capability identity?** Today
  `tegmentum:compression-multiplexer/zstd-extras@0.1.0` could mean
  either of two cap commits with different bug fixes but the same WIT
  shape. The forge manifest pins by `sha256(component.wasm)`, so two
  forges that "use the same WIT version" can still be different
  forges. That's probably right (different artifacts ARE different
  substrates) but it inflates the forge count fast.

* **Web demo bundling.** The python-wasm web demo runs
  `python.composed.wasm` through jco transpilation. Is the transpiled
  output a separate forge identity, or a derived artifact of the
  source forge? Probably the latter — jco transpilation is
  deterministic-given-input, so the manifest can declare it as a
  build step, not a separate identity.

* **Free-threaded wheels.** `cp314t` (free-threading) wheels and
  `cp314` (gil-enabled) wheels share the same source-level Python
  identity but are ABI-incompatible. The forge already handles this
  via `features.free_threading` — but the wheel index needs separate
  caches keyed on that flag.

## Status

Design only. Nothing in `pylon-forge/` exists yet. The python-wasm
build today produces a forge-shaped artifact without any forge
machinery — this doc is the description of what would have to be
written to make that artifact's identity explicit and reproducible.

The cheapest next move is Phase 0: write a manifest-emitter for the
current build. That validates the schema against a real artifact and
surfaces whatever the schema is missing.

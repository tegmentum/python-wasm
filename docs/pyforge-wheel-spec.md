# `pyforge-wheel` — extension wheel format spec

Status: **draft 0.1.0** of the python-wasm extension wheel format,
landed as Phase A of [`pip-wheel-wrapper-plan.md`](pip-wheel-wrapper-plan.md).

This spec defines how a third-party maintainer publishes a python-wasm
extension (cpython-ext bridge + capability component) as a pip-installable
`.whl`, and how `python-wasm rebuild` consumes those wheels to produce
a fresh `python.composed.wasm`.

This spec does **not** make extensions importable at install time.
WASI Preview 2 has no dlopen; installing an extension wheel stages
sources that get *statically linked* into a rebuilt `python.composed.wasm`.
See [`extension-recipe.md`](extension-recipe.md) and
[`c-ext-wheels.md`](c-ext-wheels.md) for the architectural background.

## 1. Filename and ABI tag

```
<name>-<version>-cp<py>-cp<py>-wasm32_wasip2.whl
```

- **`<name>`** — the wheel's distribution name, normalized per PEP 503.
  Matches `[package].name` in the bridge's `pyforge-pkg.toml`, with `-`
  replaced by `_` in the filename per PEP 427.
- **`<version>`** — the wheel's version, matches `[package].version` in
  `pyforge-pkg.toml`. PEP 440 conformant.
- **`cp<py>-cp<py>`** — Python version + ABI tag. One wheel per supported
  CPython minor version. python-wasm ships one minor at a time (currently
  3.14.3 by default; 3.13.9 and 3.12.13 also supported per Phase 7), so a
  maintainer who wants their extension on every supported version ships
  three wheels: `cp312-cp312`, `cp313-cp313`, `cp314-cp314`.
- **`wasm32_wasip2`** — platform tag. Distinguishes a python-wasm wheel
  from a normal CPython wheel. Pip on a non-wasm CPython will refuse to
  install (no matching tag), which is the desired behavior.

Examples:

```
xxhash3_extra-0.1.0-cp314-cp314-wasm32_wasip2.whl
my_codec-1.2.0-cp313-cp313-wasm32_wasip2.whl
```

## 2. Layout inside the wheel

A wheel is a zip file. When pip unpacks it into the conventional
location `~/.python-wasm/site-packages/`, the layout is:

```
site-packages/
├── _<bridge_srcdir>/                      ← the cpython-ext bridge
│   ├── pyforge-pkg.toml
│   ├── _<bridge_srcdir>_module.c          (or _<bridge_srcdir>module.c)
│   ├── wit/
│   │   ├── world.wit
│   │   └── deps/
│   │       ├── <interface1>/<file>.wit
│   │       └── <interface2>/<file>.wit
│   └── gen/
│       ├── <bridge_srcdir>_import.c
│       ├── <bridge_srcdir>_import.h
│       └── <bridge_srcdir>_import_component_type.o
├── _<bridge_srcdir>_component/            ← the capability component
│   └── <component_filename>.wasm
├── <pyshim_name>/                         ← optional Lib/ shim package
│   └── __init__.py                          (and other .py files)
└── <name>-<version>.dist-info/            ← standard PEP-621 metadata
    ├── METADATA
    ├── WHEEL
    ├── RECORD
    └── entry_points.txt                    (optional, for CLI tools)
```

Notes on each piece:

### 2.1 The bridge dir — `_<bridge_srcdir>/`

Exactly mirrors what lives in this repo's `cpython-ext/_<srcdir>/`.
By convention `<bridge_srcdir>` starts with an underscore (matches the
existing cpython-ext naming convention for capability bridges).

`pyforge-pkg.toml` is the single source of truth for the extension —
already specified in [`pyforge-pkg-spec.md`](pyforge-pkg-spec.md).
`python-wasm rebuild` discovers extensions by globbing
`~/.python-wasm/site-packages/**/pyforge-pkg.toml`, so this is the
mandatory marker file.

`wit/` ships the contract source — `world.wit` plus a `deps/` tree
mirroring `wit-bindgen`'s convention. Human-readable; enables auditing
("what does this extension import?") and regeneration ("our wit-bindgen-c
version differs; I want to rebuild the bindings").

`gen/` ships the pre-generated bindings — `wit-bindgen c wit/` output
that the C compiler actually consumes during rebuild. Shipping these
keeps rebuild fast (no wit-bindgen invocation needed) and avoids
maintaining wit-bindgen as a dependency of `python-wasm rebuild`.

### 2.2 The capability component dir — `_<bridge_srcdir>_component/`

A sibling directory containing exactly one `.wasm` file: the
component that implements the WIT contract the bridge imports.
The directory name is the bridge's `srcdir` plus `_component` so the
relationship is greppable.

The component filename is what the maintainer chose at component-build
time; whatever it is, `pyforge-pkg.toml` records it under
`[wheel.component].artifact` so the build pipeline finds it. See §4.

### 2.3 Optional pure-Python shim

If the extension provides a stdlib-overlay (`Lib/zlib.py` shim or
similar), the wheel may also ship a regular Python package at the
site-packages root. Each shim file is listed in `pyforge-pkg.toml`
under `[[provides]]`; the wheel build script copies them into the
wheel relative to the bridge dir.

### 2.4 The `dist-info/` directory

Standard PEP-621 layout. Only required files:

- **`METADATA`** — standard wheel metadata. The wheel builder generates
  this from `pyforge-pkg.toml`'s `[package]` block.
- **`WHEEL`** — wheel-format metadata. The crucial line is `Tag:`,
  which must match the wheel's filename tag.
- **`RECORD`** — file list with SHA-256 hashes (standard PEP 376
  format).

This spec does **not** put anything python-wasm-specific in
`dist-info/`. The `pyforge-pkg.toml` in the bridge dir is the only
metadata `python-wasm rebuild` reads.

## 3. The minimum `pyforge-pkg.toml`

The schema is defined in [`pyforge-pkg-spec.md`](pyforge-pkg-spec.md).
Wheels add no new fields — they reuse the in-tree manifest verbatim.

The minimum-viable bridge manifest (Pattern A, cpython-ext static link):

```toml
schema = "tegmentum:pylon-pyforge/pkg@0.1.0"

[package]
name = "xxhash3-extra"
version = "0.1.0"
description = "Extra xxhash3 helpers over xxhash-wasm-extra"
pattern = "A"

[extension]
srcdir = "_xxhash3_extra"
module_name = "_xxhash3_extra"
c_file = "_xxhash3_extra_module.c"
wit_dir = "wit"
gen_dir = "gen"
gen_import_c = "xxhash3_extra_import.c"
gen_import_obj = "xxhash3_extra_import_component_type.o"

[[capabilities.required]]
interface = "xxhash3:extra/helpers"
version = "0.1.0"

[wheel.component]
artifact = "_xxhash3_extra_component/xxhash3_extra.wasm"
wit_world = "xxhash3:extra/extra"
```

The `[wheel.component]` block is new in this spec — it tells
`python-wasm rebuild` where to find the capability component artifact
inside the wheel. Required for any Pattern A extension. For shim-only
packages (`[package].pattern = "shim-only"`), it's omitted.

## 4. `METADATA` generation

Mapped from `pyforge-pkg.toml`:

| METADATA field | Source |
|---|---|
| `Metadata-Version` | `2.1` (PEP 566) |
| `Name` | `[package].name` |
| `Version` | `[package].version` |
| `Summary` | `[package].description` |
| `Author` / `Author-email` | `[package].author` / `[package].author_email` (optional in pyforge-pkg.toml) |
| `License` | `[package].license` (optional) |
| `Requires-Python` | `>=3.<minor>,<3.<minor+1>` derived from the target tag |
| `Platform` | `wasm32-wasip2` |

`Description-Content-Type` defaults to `text/markdown`; the description
body (if present) goes verbatim into the METADATA body section.

## 5. `WHEEL` generation

```
Wheel-Version: 1.0
Generator: python-wasm-build/0.1.0
Root-Is-Purelib: false
Tag: cp314-cp314-wasm32_wasip2
```

`Root-Is-Purelib: false` because the wheel ships compiled artifacts
(the capability `.wasm` and `gen/*_component_type.o`), even if those
artifacts are not the conventional Python `.so` shape.

## 6. Rebuild flow

`python-wasm rebuild` is Phase C of the wheel-wrapper plan. The
rebuild flow against this spec:

1. **Discover** — glob `~/.python-wasm/site-packages/**/pyforge-pkg.toml`.
   Each match is a candidate extension.
2. **Validate** — for each candidate, run the equivalent of
   `pyforge-pkg-verify.sh` against the unpacked layout. Drop malformed
   ones with a warning.
3. **Stage into the build tree** — copy each `_<srcdir>/` directory
   from site-packages to `cpython-ext/<srcdir>/` in the python-wasm
   build tree, marking with a `.staged` sentinel. The base in-tree
   `cpython-ext/<srcdir>/` directories already there are untouched
   unless a staged extension shares a name (conflict policy below).
4. **Build** — invoke `scripts/build-from-pkgs.sh` (Phase 8) with the
   union of base + staged extensions in `--include`.
5. **Cleanup** — remove the `.staged` sentinels and the copied dirs so
   the repo state matches its tracked-files view.
6. **Land** — copy the resulting `build/<profile>/python.composed.wasm`
   to `~/.python-wasm/python.composed.wasm`.

### 6.1 Conflict policy

If a staged extension's `[package].name` matches an in-tree
cpython-ext extension's name, the staged one **wins** by default
(last-wins). Rebuild emits a warning naming both versions. A user can
override with `python-wasm rebuild --no-replace=<name>` or by removing
the wheel.

### 6.2 The capability component

For each Pattern A extension with a `[wheel.component]` block, the
rebuild step also needs to plug the component into the compose step.
The compose script (`scripts/compose-python-component.sh`) already
takes a list of `--plug` arguments via env vars
(`ZLIB_COMPONENT_WASM`, `OPENSSL_COMPONENT_WASM`, etc.). The rebuild
needs to extend that list dynamically:

- Each staged wheel's `[wheel.component].artifact` is a relative path
  inside the wheel. Rebuild resolves it to an absolute path under
  `~/.python-wasm/site-packages/` and exposes it as a new env var
  (or a `--plug-extra <path>` argument the compose script knows
  about).
- The bridge's `pyforge-pkg.toml` already declares
  `[[capabilities.required]]`, so the import side is fine. The
  component side is what this section provides.

## 7. Validation checklist

Before publication, an extension wheel should pass:

1. **Filename** — matches `<name>-<version>-<py>-<py>-wasm32_wasip2.whl`
   with `<name>` underscored, `<version>` PEP-440.
2. **Layout** — `_<srcdir>/`, `_<srcdir>_component/` (if Pattern A),
   `<name>-<version>.dist-info/` all present.
3. **pyforge-pkg-verify** — `scripts/pyforge-pkg-verify.sh` passes
   against the unpacked wheel's `_<srcdir>/pyforge-pkg.toml`. The
   verifier already checks that `c_file`, `gen_import_c`,
   `gen_import_obj`, and every `[[provides]].shim` exist.
4. **Component-ABI sanity** — `wasm-tools component wit
   <wheel>/_<srcdir>_component/*.wasm` parses and the world matches
   `[wheel.component].wit_world`.
5. **Bridge-component WIT match** — every interface in
   `[[capabilities.required]]` is exported by the component's WIT.

`scripts/build-extension-wheel.sh` (Phase B) runs all five before
emitting the final `.whl`.

## 8. What this spec doesn't cover

- **Pure-Python wheels.** Those follow the standard PEP 427 layout and
  install directly into `~/.python-wasm/site-packages/` via
  `pip install --target`. No rebuild needed.
- **Pattern N (offload-to-v86) extensions.** A separate spec —
  [`pyforge-pkg-native-spec.md`](pyforge-pkg-native-spec.md) — covers
  those. No wheels of this shape exist yet.
- **WIT contract version negotiation.** A wheel built against
  `xxhash3:extra@0.1.0` won't compose against an
  `xxhash3:extra@0.2.0` component. Rebuild fails fast; resolution is
  the maintainer republishing.
- **PyPI publication policy.** Where wheels live, registration with
  PyPA, etc. — Phase E of the plan.

## 9. See also

- [`pyforge-pkg-spec.md`](pyforge-pkg-spec.md) — the in-tree manifest
  schema this wheel format consumes verbatim.
- [`extension-recipe.md`](extension-recipe.md) — Phase 8's recipe-
  driven build model. Wheels are the publication surface on top.
- [`pip-wheel-wrapper-plan.md`](pip-wheel-wrapper-plan.md) — the
  end-to-end plan; this doc is Phase A.
- [`c-ext-wheels.md`](c-ext-wheels.md) — why pip-style .so wheels
  don't work on wasi-p2.

# pip-installable extension wheels — implementation plan

The Phase 8 pivot ([`extension-recipe.md`](extension-recipe.md)) made
`cpython-ext/<srcdir>/pyforge-pkg.toml` the single declarative source of
truth and produced `scripts/build-from-pkgs.sh` to consume it. The
*publication* surface — how an external maintainer ships an extension
to users of python-wasm — is still missing.

This plan covers that gap. The deliverable is a workflow where:

- **An extension maintainer** packages their `cpython-ext`-shaped
  bridge + capability component as a `.whl` and publishes it.
- **An end user** runs `pip install <pkg>` then `python-wasm rebuild` and
  gets a fresh `python.composed.wasm` with the extension included.

The work decomposes into five phases ordered by dependency. Each phase
has a clear exit criterion.

## Why this is non-trivial

A python-wasm extension is **two artifacts** that must ship together
(see the [extensions FAQ in extension-recipe.md](extension-recipe.md)
for full background):

1. **The bridge** — C/Rust source in `cpython-ext/_<name>/`, plus
   wit-bindgen-emitted bindings. This is what gets statically linked
   into a rebuilt `python.wasm`.
2. **The capability component** — a `.wasm` component that implements
   the WIT contract the bridge imports. This is what `wac plug` adds
   to the composed artifact.

A wheel that ships only one of these is unusable. The wheel format
plan below packages both, side-by-side, behind one pip install.

We also can't take the standard pip route ("install a `.so` to
site-packages, import works"). wasi-p2 has no dlopen. So the wheel
**doesn't make the extension importable** — that's the rebuild's job.
The pip step is a *staging* step.

## Phase A — Wheel format spec (1 week)

Define exactly what a python-wasm extension wheel looks like.

### Deliverables

- **`docs/pyforge-wheel-spec.md`** — the wheel layout, the ABI tag, what
  ships, what doesn't. Schema'd so `pyforge-pkg-verify.sh` can check
  unpacked wheels too.

### Layout

```
xxhash3_extra-0.1.0-cp314-cp314-wasm32_wasip2.whl   ← regular zip file
├── _xxhash3_extra/                        the bridge sources (Phase 8 shape)
│   ├── pyforge-pkg.toml
│   ├── _xxhash3_extra_module.c
│   ├── wit/
│   │   ├── world.wit
│   │   └── deps/
│   └── gen/
│       ├── xxhash3_extra_import.c
│       ├── xxhash3_extra_import.h
│       └── xxhash3_extra_import_component_type.o
├── _xxhash3_extra-component/
│   └── xxhash3_extra.wasm                 the capability component
├── xxhash3_extra/                         optional Lib/ shim
│   └── __init__.py
└── xxhash3_extra-0.1.0.dist-info/
    ├── METADATA                            standard PEP 621
    ├── WHEEL                               Tag: cp314-cp314-wasm32_wasip2
    ├── RECORD
    └── pyforge.toml                        wheel-level manifest (next)
```

`xxhash3_extra-0.1.0.dist-info/pyforge.toml` is a small wheel-level
descriptor pip can't see but `python-wasm stage` reads:

```toml
schema = "tegmentum:pylon-pyforge/wheel@0.1.0"

[wheel]
pkg_name = "xxhash3-extra"
pkg_version = "0.1.0"
abi_tag = "cp314-cp314-wasm32_wasip2"

[wheel.bridge]
srcdir = "_xxhash3_extra"
manifest = "_xxhash3_extra/pyforge-pkg.toml"

[wheel.component]
artifact = "_xxhash3_extra-component/xxhash3_extra.wasm"
wit_world = "xxhash3:extra/extra"
```

### Open questions to resolve

- **Single ABI tag or matrix?** `cp314-cp314-wasm32_wasip2` is one
  python-version × one wasi target. A package that supports 3.13 +
  3.14 needs two wheels. Mirror the regular pip flow (multiple
  wheels, pip picks one); document the matrix.
- **What gets included in `wit/`?** The full WIT tree (deps/*.wit
  too) lets the user rebuild from source if they ever need to. Or
  ship only `gen/` and treat the WIT as build-time. **Recommendation:
  ship both — gen/ is what the build uses, wit/ is for transparency
  and re-generation if a wit-bindgen version changes.**
- **Should `pyforge-pkg.toml` move out of `_xxhash3_extra/` into
  `dist-info/`?** Current `cpython-ext/<dir>/pyforge-pkg.toml`
  semantics matches in-tree development; wheels can have both
  copies (one for `pyforge-pkg-verify.sh`, one in dist-info for the
  staging step). **Recommendation: keep it in the bridge dir; add a
  short pointer in pyforge.toml.**

### Exit criterion

Spec doc lands; a hand-built wheel of `_zlib_cap` passes
`scripts/pyforge-pkg-verify.sh` when unpacked.

## Phase B — Maintainer-side packager (1 week)

A tool that turns a `cpython-ext/<srcdir>/` source tree + a built
capability component into the Phase-A wheel.

### Deliverables

- **`scripts/build-extension-wheel.sh`** — bash-side wrapper for the
  same reason every other script in `scripts/` is bash: it's the
  packaging tier and doesn't fight Python ecosystem assumptions.
- **`scripts/lib/wheel_builder.py`** — the actual zipping + manifest
  generation. Stdlib-only (`zipfile`, `tomllib`, `hashlib`).

### What it does

```bash
$ scripts/build-extension-wheel.sh \
      --srcdir cpython-ext/_zlib_cap \
      --component ~/git/zlib-wasm/build/bin/zlib.component.wasm \
      --version 0.1.0 \
      --python 3.14 \
      --output dist/
==> validated cpython-ext/_zlib_cap/pyforge-pkg.toml
==> built dist/zlib_cap-0.1.0-cp314-cp314-wasm32_wasip2.whl
```

Order of operations:

1. Run `pyforge-pkg-verify.sh` on the source tree. Fail fast if the
   manifest is inconsistent.
2. Copy `cpython-ext/<srcdir>/` into a temp staging dir.
3. Copy the named `.wasm` component into `<srcdir>-component/` in the
   staging dir.
4. Emit `dist-info/METADATA` from `pyforge-pkg.toml`'s `[package]`
   block (name, version, description, requires-python).
5. Emit `dist-info/WHEEL` with the correct ABI tag.
6. Emit `dist-info/pyforge.toml` from the bridge manifest + component
   paths.
7. Emit `dist-info/RECORD` with hashes.
8. Zip the staging dir as `<name>-<version>-<tag>.whl`.

### Exit criterion

Running the script against the 11 cap extensions in `cpython-ext/`
produces 11 wheels under `dist/`. Each unpacks cleanly and passes
`pyforge-pkg-verify.sh`. The wheels round-trip — Phase C can stage them.

## Phase C — Consumer-side stager + rebuild (1–2 weeks)

The other half of the pipeline: pip-installed wheels get noticed,
staged, and rebuilt into a usable python.composed.wasm.

### Deliverables

- **`scripts/python-wasm-stage.sh`** — walks `site-packages` (in any
  Python env the user pip-installs into), finds wheels with a
  `pyforge.toml` in their `dist-info/`, copies the bridge + component
  to `~/.python-wasm/extensions/<srcdir>/`. Idempotent.
- **`scripts/python-wasm-rebuild.sh`** — the user-facing rebuild
  command. Reads `~/.python-wasm/extensions/*/pyforge-pkg.toml`,
  passes the package names to `scripts/build-from-pkgs.sh
  --include …`, copies the result to
  `~/.python-wasm/python.composed.wasm`.

### Why pip itself isn't enough

Pip's install step copies wheel contents into the *target Python*'s
`site-packages`. That target Python is whichever interpreter ran
`pip install` — typically the host's CPython 3.14, not python-wasm.
Two consequences:

- The bridge + component land somewhere the host Python knows about
  but python-wasm has never heard of.
- The host Python can't actually use them either (wasm32 binaries
  don't import on darwin/linux/win).

So `pip install` of a python-wasm extension on the host is **just a
content-distribution mechanism**. The stager moves the content into
python-wasm's reach.

### User flow

```bash
# Maintainer side (one-time, by the package author):
$ scripts/build-extension-wheel.sh --srcdir cpython-ext/_xxhash3_extra ...
$ twine upload dist/xxhash3_extra-0.1.0-cp314-cp314-wasm32_wasip2.whl

# Consumer side:
$ pip install xxhash3-extra                     ← drops files in site-packages
Successfully installed xxhash3-extra-0.1.0
$ python-wasm stage                              ← copies to ~/.python-wasm/extensions/
==> staged: _xxhash3_extra (v0.1.0)
$ python-wasm rebuild                            ← rebuilds python.composed.wasm
==> rebuilding with: _zlib_cap, _ssl_capability, ..., _xxhash3_extra
==>  16M ~/.python-wasm/python.composed.wasm
$ python-wasm -c "import _xxhash3_extra; print(_xxhash3_extra.hash(b'x'))"
0x9f47c0b3e7e2...
```

`python-wasm rebuild` could be made implicit (run by `python-wasm
stage` automatically). Decide based on per-rebuild cost: today
`build-from-pkgs.sh` is ~2 minutes from cold; that's a lot if the
user installs 4 wheels and runs rebuild after each.

### Open questions

- **Where does the rebuild get its build inputs from?** `cpython-ext/`
  in this repo is the source-of-truth for the *base* extensions
  shipped with python-wasm. A staged extension under
  `~/.python-wasm/extensions/` adds to that set. The build needs both.
  Approach: copy the staged extensions *into* `cpython-ext/` for the
  build, marked with a `.staged` sentinel so they don't get committed.
- **What if the user doesn't have the python-wasm repo at all?** They
  installed `pip install python-wasm` and got a default
  `python.composed.wasm`. To rebuild, they need the cpython-ext
  sources too — meaning the python-wasm meta-distribution has to ship
  the base `cpython-ext/` source tree. Tractable but not free —
  ships ~1 MB of source.
- **What about caching the rebuild output?** If two users install
  the same set of extensions, can they share a prebuilt
  `python.composed.wasm`? `pylon-forge` (commit `6648782` discussion
  in [`memory/composectl-is-the-substrate`](#)) intersects here —
  content-addressed caching of composed artifacts is exactly its
  scope. Phase 5 of this plan covers integration.

### Exit criterion

A user with a clean `~/.python-wasm/` runs `pip install` of a Phase-B
wheel, runs `python-wasm stage && python-wasm rebuild`, and ends up
with a `~/.python-wasm/python.composed.wasm` that includes the
extension. `python-wasm -c 'import _xxhash3_extra; print(_xxhash3_extra)'`
prints a valid module repr.

## Phase D — `python-wasm` CLI (1 week)

Phase C's scripts work but require the python-wasm repo checked out.
A `pip install python-wasm` should give the user a `python-wasm`
binary on their PATH that does the same thing without a repo.

### Deliverables

- **`python-wasm`** — a Python package (pip-installable from this
  repo or PyPI) that ships:
  - `python-wasm` console script entry point
  - `python_wasm/cli.py` — argparse-driven CLI
  - `python_wasm/data/python.composed.wasm` — the default build,
    bundled at packaging time (~16 MiB; pip wheels can carry that)
  - `python_wasm/data/cpython-ext-base/` — the cpython-ext sources
    for the extensions shipped in the default build, so rebuild has
    the inputs
  - `python_wasm/data/wasi-sdk-pointer` — instructions to fetch the
    wasi-sdk on first rebuild (don't bundle 100+ MiB)

### Subcommands

| Command | What |
|---|---|
| `python-wasm -c "code"` | exec code through the runner — proxies to wasmtime with the standard mounts |
| `python-wasm script.py` | exec a script |
| `python-wasm -m mod` | exec a module |
| `python-wasm stage` | Phase C's stager |
| `python-wasm rebuild` | Phase C's rebuilder |
| `python-wasm extensions list` | what's currently linked into the active composed wasm |
| `python-wasm extensions remove <name>` | drop from `~/.python-wasm/extensions/` + invalidate the cached composed wasm |
| `python-wasm info` | versions, paths, active build digest |
| `python-wasm doctor` | sanity check the install + report any drift |

### Open question

The CLI overlaps with `scripts/run-python.sh` and friends. Approach:
the CLI in pkg form uses the same underlying shell logic via
`subprocess.run` of the existing scripts (when run from a python-wasm
checkout), but ships a Python re-impl for the no-checkout case. Keeps
duplication low.

### Exit criterion

`pip install python-wasm` on a clean machine, no python-wasm
checkout. `python-wasm -c "import sys; print(sys.version)"` prints
3.14.3. `python-wasm stage && python-wasm rebuild` reproduces a
composed wasm equivalent to today's default build.

## Phase E — Distribution + versioning (open-ended)

Once the technical wiring works, the policy questions become
load-bearing.

### Topics

- **PyPI vs private index.** Where do
  `xxhash3-extra-0.1.0-cp314-cp314-wasm32_wasip2.whl` files live?
  PyPI accepts arbitrary ABI tags in principle, but our tag isn't
  standardized. Three options:
  - Self-host an index (Gemfury, Cloudsmith, or an S3-static
    index). Simplest. Documented as the official channel.
  - Push for PyPA recognition of a `wasm32_wasip2` tag. Long term but
    correct.
  - Bundle a small index inside `python-wasm` itself ("blessed
    extensions") + allow `pip install --index-url …` for others.
- **WIT contract version skew.** A wheel ships
  `_xxhash3_extra-component/xxhash3_extra.wasm` against
  `xxhash3:extra@0.1.0`. The bridge sources expect the same version.
  If `xxhash3:extra` later goes to 0.2.0, a wheel built against 0.1.0
  won't compose against an 0.2.0 component (and vice versa). The
  `pyforge-pkg.toml` `[[capabilities.required]]` block captures the
  expected version; rebuild fails fast on mismatch instead of
  composing a non-functional artifact.
- **Default-build coupling.** If the python-wasm meta-package ships
  `python.composed.wasm` with `_zlib_cap` already statically linked,
  and a user installs a wheel that also ships `_zlib_cap` (a
  different version), conflict resolution lives somewhere. Three
  options:
  - First-wins (in-tree base extensions can't be overridden).
  - Last-wins (staged extensions shadow the base).
  - Explicit (`python-wasm rebuild --replace _zlib_cap`).
  - Recommendation: **last-wins by default + warn**; documented in
    `extension-recipe.md`.
- **Build caching.** A user with M staged extensions, picking
  N at a time for a given build, can hit
  `pylon-forge` / `composectl`'s content-addressed cache if the (M
  choose N) set was previously built. The compose step is already
  digest-aware. Wiring rebuild through composectl's cache is a
  separate plan item ([`pylon-pyforge.md`](pylon-pyforge.md)
  covers it).

### Exit criterion

A `docs/extension-distribution.md` covers all of the above with a
documented policy + a worked example. PyPA outreach is a separate
track and not blocking.

## Suggested execution order

```
A — wheel spec               ─┐
B — maintainer packager        ├─ first; everything else needs these
C — stager + rebuild           ┘
D — python-wasm CLI         ─ after C is solid
E — distribution + versioning ─ continuous; doc + policy work
```

**MVP slice** = A + B + C. That's enough to demonstrate the round
trip end-to-end with the python-wasm repo checked out. The CLI (D)
is what makes it usable for someone without a checkout, and policy
(E) is what makes it sustainable beyond one team.

**Roughly 2–4 weeks of work** for the MVP slice, depending on which
open questions Phase A defers and how much PEP 517 / PEP 621
plumbing Phase B picks up versus hand-rolling.

## What this isn't

- Not the bigger Python ecosystem story — this is for *cpython-ext
  bridge + capability component* pairs, not regular pure-Python or
  conventional C-ext wheels. Pure-Python wheels already work via
  `./scripts/run-python.sh -m pip install …` per
  [`wheel-install.md`](wheel-install.md).
- Not a way to load extensions *at runtime* into a running
  python.composed.wasm. No dlopen, no late-binding; the rebuild
  step is structural, not aesthetic.
- Not blocked on PyPA — a self-hosted index works today; PyPA
  recognition of the tag is a nicer end state but not gating.

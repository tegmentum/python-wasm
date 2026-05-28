# `pyforge-pkg-native.toml` — native-tier (Pattern N) Python package spec

A per-package declarative manifest for **native-tier** Python packages
— packages whose code runs as native Linux binaries inside a v86 guest,
crossing the wasm boundary via the `tegmentum:py-offload@0.1.0` contract.

This is the counterpart to [`pyforge-pkg-spec.md`](pyforge-pkg-spec.md)
(Pattern A, cpython-ext static linkage in-wasm). The two specs share
the registry shape (`registry/packages.json` / `wit/py-package.wit`) but
describe different package substrates:

| Pattern | Substrate | Where code runs | `env` in registry |
|---|---|---|---|
| **A** | cpython-ext static linkage + Lib/ shim | In-process (the wasm interpreter IS the worker) | sha256(python.composed.wasm) |
| **N** | v86 guest + native Linux binary | Native code inside a v86 guest, offload contract bridges to/from wasm | sha256(composed v86 worker artifact) |

Pattern N is for packages whose native code is **only** published as
x86/aarch64 — numpy, scipy, pandas, pillow, lxml, cryptography, and the
long tail of manylinux wheels. The v86 guest hosts a 32-bit Linux
running a real CPython that imports the native package; the wasm side
calls into it via `offload.run(env, task)`.

Per the WIT contract (`wit/py-package.wit`), the registry's `env` is the
content-addressed sha256 of the *composed worker artifact*. For Pattern
N this is the composectl-emitted wasm that bundles:

  * v86 base component (`v86-component.wasm`)
  * a guest disk image with Python + the package(s) installed
  * an offload-bridge component that translates `offload.run` calls into
    guest-side Python invocations (see §3.3)

This document is `pyforge-pkg-native/spec@0.1.0`. The shape will change
as v86's typed-WIT story matures — currently v86 is driven through
`wasi:cli/command`-shaped invocations; once `boot-manager` / device
contracts are real WIT, the bridge component becomes a typed
component-to-component call. The spec reserves room for both.

## 1. Why this exists

A native-tier Python package's identity has three orthogonal axes that
Pattern A doesn't:

1. **Wheel identity** — which compiled package binary? `numpy-1.26.4-cp311-cp311-manylinux2014_i686.whl` and `numpy-1.26.4-cp312-cp312-...` are different artifacts.
2. **Base image identity** — which v86 guest image hosts it? Debian Bookworm + CPython 3.11 is different from Alpine + CPython 3.12.
3. **Composed worker identity** — what composectl plan composes the v86 component + image + bridge into a runnable wasm worker?

A single declarative file holds all three, plus the offload entries the
worker exposes. `pylon native materialize` produces a registry entry whose
`env` digests the full composed worker, and `pylon native plan` emits a
composectl plan that, when fed to `composectl emit build`, produces that
worker artifact.

## 2. File location and discovery

```
packages-native/<name>/
  pyforge-pkg-native.toml    # this spec
  README.md                  # author + maintenance notes
  (optional) overlays/       # additional venv content, scripts, etc.
```

Pylon tooling discovers native-tier packages by globbing
`packages-native/*/pyforge-pkg-native.toml`. One file per package
(or per-package-group, like `numpy-scipy-pandas`).

## 3. Schema

### 3.1 Top-level

```toml
schema = "tegmentum:pylon-pyforge/pkg-native@0.1.0"
```

### 3.2 `[package]` — identity

```toml
[package]
name = "numpy"
version = "1.26.4"
description = "Native numpy 1.26.4 (cp311 manylinux2014_i686) over v86"
pattern = "N"
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `name` | string (kebab-case) | yes | Becomes `dist.name` in the materialized registry entry. Use the actual Python package name (`numpy`, `pandas`). |
| `version` | string (PEP 440) | yes | Becomes `dist.version`. Match the wheel's version exactly. |
| `description` | string | yes | One-line summary. |
| `pattern` | `"N"` | yes | Only `"N"` is defined for native-tier packages in 0.1.0. |

### 3.3 `[guest]` — base image + Python runtime

```toml
[guest]
image = "v86://debian-bookworm-py311-i686"
image_sha256 = "0123abc…"
cpython_version = "3.11.7"
arch = "i686"
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `image` | string | yes | Logical name for the v86 base image (e.g., `v86://debian-bookworm-py311-i686`). Pylon doesn't yet have a registry of these — for now this is a label for human accounting. |
| `image_sha256` | string | yes | Content digest of the base image. Anchors the worker to a specific guest snapshot. |
| `cpython_version` | string | yes | The CPython version the guest ships. Determines wheel compatibility tags. |
| `arch` | string | yes | Guest architecture (`i686`, `x86_64`, `aarch64`). v86 today is `i686`-only. |

### 3.4 `[[wheels]]` — what to install in the guest venv

Each entry is one wheel to add to the guest's Python environment. The
guest image starts from `[guest].image`; wheels are installed on top
(via pip in the boot script — see §4).

```toml
[[wheels]]
name = "numpy"
version = "1.26.4"
filename = "numpy-1.26.4-cp311-cp311-manylinux2014_i686.whl"
sha256 = "abcd1234…"
source = "pypi:https://files.pythonhosted.org/packages/.../numpy-1.26.4-cp311-cp311-manylinux2014_i686.whl"

[[wheels]]
name = "numpy"
version = "1.26.4"
filename = "numpy-1.26.4-py3-none-any.whl"
sha256 = "..."
source = "pypi:..."
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `name` | string | yes | Python package name (`numpy`). |
| `version` | string | yes | Version. |
| `filename` | string | yes | Wheel filename — encodes ABI + platform tag per PEP 425. |
| `sha256` | string | yes | sha256 of the wheel file. Pins it to a specific build. |
| `source` | string | yes | URL or `file://` path. `pypi:<url>`, `file:<path>`, `git:<repo>` are conventions. |

Multiple wheels are allowed — typical: the package's own wheel + its
runtime dependencies (numpy in numpy's case is dependency-free; pandas
brings 5+ transitive wheels). `pylon native verify` cross-checks that
the dependency closure is complete by running `pip install --dry-run`
inside a host venv with the same Python version.

### 3.5 `[[provides]]` — offload entries

```toml
[[provides]]
module = "numpy"
purpose = "Native numpy array ops over v86"
version = "1.26.4"
offload = [
  { callable = "array",      doc = "Build an ndarray from a Python sequence.", codecs = ["msgpack", "arrow"] },
  { callable = "matmul",     doc = "Matrix product (a @ b).",                  codecs = ["arrow"] },
  { callable = "linalg.svd", doc = "Singular value decomposition.",            codecs = ["arrow"] },
]
```

**TOML reminder.** Each `{ ... }` inline table MUST stay on one line —
splitting one across multiple lines is invalid TOML (`pylon native verify`
will fail to parse). Use long lines and column-align if readability
suffers.

Same shape as Pattern A's `[[provides]]`, with one notable difference:
**all entries are offload entries**. There's no `shim`/`dest` (no
in-process API to overlay — the package isn't in the wasm runtime at
all). The `module` field still appears so callers' `import numpy`
proxying resolves to this entry via the registry.

| Field | Type | Required | Notes |
|---|---|---|---|
| `module` | string | yes | Python module name as the caller imports it. Becomes `dist.name`-derived. |
| `purpose` | string | yes | One-line note. |
| `version` | string (PEP 440) | yes | Reported in the registry. |
| `offload` | array of inline tables | yes | At least one entry — see Pattern A spec §3.5 for the callable / doc / codecs shape. |

For native-tier packages, the `codecs` field matters more than for
Pattern A: arrow is often required for ndarray / dataframe-style
payloads (msgpack/json don't carry numeric arrays efficiently).

### 3.6 `[bridge]` — offload-bridge component

The bridge is the wasm component that exports
`tegmentum:py-offload/offload` and translates `run(env, task)` calls
into guest-side invocations (today via `wasi:cli/command` + virtiofs;
later via typed v86 WIT). Pylon doesn't build the bridge — it's a
separate component, identified by its sha256.

```toml
[bridge]
component_sha256 = "fedc4321…"
local_path = "${V86_OFFLOAD_BRIDGE_WASM:-$HOME/git/v86-offload-bridge/build/bridge.wasm}"
source = "git@github.com:tegmentum/v86-offload-bridge.git#0.1.0"
provides = ["tegmentum:py-offload/offload@0.1.0"]
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `component_sha256` | string | yes | sha256 of the bridge `.wasm`. Pinned. |
| `local_path` | string | yes | Build-time path. Supports `${VAR:-default}` expansion (same shape as `.pylon.toml`'s `[[capabilities]].path`). |
| `source` | string | recommended | Provenance — git URL + ref, or local path. |
| `provides` | array of strings | yes | WIT interface(s) the bridge exports. Always includes `tegmentum:py-offload/offload@<ver>`. |

**Status note (2026-05).** A `v86-offload-bridge` component doesn't
exist yet as a standalone repo — the today/later split described in
`docs/native-execution-and-parallelism.md` §4.2 means today's "bridge"
is a host shim, not a wasm component. This spec describes the future
state. For 0.1.0, `[bridge]` may be omitted with a TODO comment, and
`pylon native plan` emits a plan with a placeholder for the bridge.

### 3.7 `[gating]` — backend selection hints

Native-tier packages aren't gated by forge variant (there's no
in-wasm forge to gate against), but they can be gated by host
capability:

```toml
[gating]
requires_host = ["v86"]
prefer_over = ["in-wasm"]
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `requires_host` | array of strings | optional, default `["v86"]` | What host caps the worker needs. `v86` means a host that can run v86 (browser via the v86 wrapper, or wasmtime + v86's component). |
| `prefer_over` | array of strings | optional, default `[]` | Tiers this backend takes precedence over when both are available. Honored by `registry.select(name, prefer)` in callers. Today empty — `select` defaults to in-wasm-first regardless. |

## 4. Composed worker artifact

For Pattern N, the registry's `env` is `sha256(composed-worker.wasm)`
where `composed-worker.wasm` is what `composectl emit build` produces
from a plan composing:

```
root  : v86-offload-bridge.wasm           (exports tegmentum:py-offload/offload)
plug  : v86-component.wasm                (imports satisfied: wasi:cli + posix bits)
asset : guest-image.tar.zst (or .img)     (guest filesystem with venv installed)
```

The guest image is content-addressed and lives in composectl's blob
store like every other artifact; its sha256 becomes part of the plan
inputs and therefore part of the composed worker's identity.

When the wheel set changes, the guest image changes, the worker
artifact changes, and `pylon native materialize` re-registers the new
`env` in `registry/packages.json`. Same content-address discipline as
Pattern A — the worker artifact IS its identity.

## 5. Derivation rules

`pylon native materialize` (planned; see §6) produces one registry
entry per Pattern N package:

* `dist.name` = `[[provides]].module`
* `dist.version` = `[package].version`
* `dist.backends[]` = `[{tier: "native-v86", env: <composed-worker-sha>}]`
* `entries[]` = the `[[provides]].offload[]` array translated to
  `{entry: "<module>:<callable>", doc, codecs}`

Existing Pattern A entries are unaffected — the same package can carry
both an in-wasm backend (Pattern A) and a native-v86 backend (Pattern N)
in its `backends` list. `registry.select` picks one per the caller's
`prefer` list.

## 6. Tooling (planned)

| Verb | What it does | Status |
|---|---|---|
| `pylon native new <name>` | Scaffold a `packages-native/<name>/` skeleton (toml + README) | Pending — same shape as `pylon pkg new` |
| `pylon native verify` | Validate toml + wheel reachability + offload-entry shape | Pending |
| `pylon native plan <name>` | Emit a composectl plan JSON for the composed worker | Stub — see `pylon native plan-stub` |
| `pylon native build <name>` | Build the guest image (install wheels) + run `composectl emit build` + return the artifact sha | Pending — blocked on v86's WIT story |
| `pylon native materialize` | Merge Pattern N entries into `registry/packages.json` | Pending — extension of Pattern A's `pylon pkg materialize` |

The first two and the plan-emission stub are achievable today; build
emission is blocked on the v86 component growing its typed WIT (currently
only `posix.wit` exists, with `boot-manager` / device contracts pending).

## 7. Worked example

[`packages-native/numpy/pyforge-pkg-native.toml`](../packages-native/numpy/pyforge-pkg-native.toml)
is the reference example — a numpy 1.26.4 native-tier package. The
declaration covers everything described above; the `[bridge]` block
includes the TODO marker reflecting the 2026-05 state.

## 8. What this spec deliberately doesn't model

* **Live object proxying** — returning a numpy array to wasm Python that
  acts like a normal ndarray (with attribute access, slicing, …) is
  Issue #5 from `docs/native-execution-and-parallelism.md`. The current
  contract is call-and-serialize: every call's args + return cross the
  boundary via the declared codec, no opaque handles.
* **Multi-package workers** — one package per `pyforge-pkg-native.toml`
  today. If a worker needs numpy + pandas, that's a future v0.2.0
  feature (`[[provides]]` listing multiple modules within one worker
  artifact). For now: one declaration per worker.
* **Wheel resolution from package name + version** — the toml carries
  explicit wheel `filename` + `sha256`; there's no `pip resolve`
  step in pylon. Use uv-wasm or similar separately to produce the
  resolved wheel list, then transcribe.
* **Worker pooling** — same `env` reused across many `offload.run` calls.
  That's composectl + the bridge component's concern; pylon emits one
  composed worker artifact per package and trusts composectl to manage
  instances.

## 9. Versioning

Document version: `0.1.0`.

* Breaking schema changes → `@1.0.0`.
* Additive optional fields → `@0.2.0`.
* Doc clarifications → patch.

The expected `@0.2.0` adds: multi-package workers (`[[provides]]` with
multiple modules), typed-WIT bridge (when v86 grows `boot-manager`),
arrow-everywhere codec defaulting.

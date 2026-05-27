# Pylon / PyForge — Implementation Plan

Companion to `pylon-pyforge.md`. The design doc says what gets built;
this doc says **in what order, with what gates, and against what
risks**. Calibrated to one engineer working steadily — calendar time
will stretch with reviews, side-tracks, and ecosystem coordination.

## Headline numbers

| Phase | Engineer-weeks | Calendar (one person, ~50% focused) |
|---|---|---|
| 0. Manifest emitter | 1 | 1–2 weeks |
| 1. Reproducible builds | 2–3 | 1 month |
| 2. Recursive cap forges | 3–4 | 1.5–2 months |
| 3. Wheel resolver | 3–4 | 1.5–2 months (overlaps Phase 2) |
| 4. Multi-forge coexistence | 1–2 | 2–3 weeks |
| **Total to first usable system** | **10–14** | **5–8 months** |

The bands widen with phase number: Phase 0/1 are well-understood;
Phase 2/3 have ecosystem-coordination risk that can blow up scope. If
the calendar projection feels wrong, the variable is almost certainly
Phase 2 cap-repo migration (touching multiple sibling repos with
different build systems) or Phase 3 wheel-metadata coordination
(potentially needing a real PEP).

## Sequencing rationale

```
Phase 0  ─── Phase 1 ─┬─ Phase 2 ─┐
                       │           ├── Phase 4
                       └─ Phase 3 ─┘
```

* **Phase 0** is the cheapest possible test of the manifest schema.
  Before writing build code, write a manifest *emitter* that describes
  the artifact this repo already produces. Reveals schema gaps fast.
* **Phase 1** is the gating dependency. Nothing downstream can start
  until `pylon forge build manifest.toml` produces a bit-identical
  artifact reproducibly.
* **Phase 2** and **Phase 3** can run in parallel — recursive cap
  forges (Phase 2) and wheel resolver (Phase 3) don't strictly depend
  on each other, only on Phase 1.
* **Phase 4** is the UX layer; defer it until at least one consumer
  workflow actually wants multi-forge selection.

## Repo layout

Recommend a single new repo `pylon-forge/` at top-level, sibling to
python-wasm and the cap repos. Inside:

```
pylon-forge/
├── README.md
├── DESIGN.md          # → ../python-wasm/docs/pylon-pyforge.md (symlink or copy)
├── pyproject.toml     # Python tool, packaged as `pylon`
├── src/pylon/
│   ├── __init__.py
│   ├── cli.py         # argparse / click / typer
│   ├── manifest.py    # schema, serialization
│   ├── forge.py       # forge resolution + build orchestration
│   ├── cas.py         # content-addressed storage
│   ├── cap.py         # cap-side forge (Phase 2)
│   ├── resolver.py    # wheel matching (Phase 3)
│   ├── builders/
│   │   ├── cpython.py
│   │   └── cap_rust.py
│   └── tests/
│       └── pyforge-tests.toml
├── schemas/
│   └── pyforge-manifest.schema.json
└── docs/
    ├── manifest-spec.md
    ├── wheel-metadata-extension.md
    └── migrating-a-cap-repo.md
```

Single language (Python) for the CLI even though the caps are
Rust — Python iterates faster on schema work, has native TOML support
(`tomllib` stdlib), and the CLI does mostly file I/O + subprocess.
Performance isn't the bottleneck for any of this.

## Phase 0 — Manifest Emitter

**Goal.** Write a tool that inspects the current `python.composed.wasm`
in this repo and produces a `pyforge-manifest.toml` describing it. No
build changes; no new infrastructure.

**Why first.** The manifest schema in `pylon-pyforge.md` is plausible
but unvalidated. Round-tripping a real artifact through it will surface
fields that don't fit, fields I forgot, and identity axes that turn
out to be redundant or load-bearing.

**Deliverables.**

1. `pylon emit-manifest <python.composed.wasm>` command.
   * Reads the artifact + the source tree it was built from.
   * Extracts:
     - CPython source identity (read `deps/cpython/.git/HEAD` or fall
       back to `python --version` introspection).
     - Toolchain identity (parse `Makefile:WASI_SDK_DIR`).
     - Stdlib overlay (walk `cpython-ext/_*/`; record sha256 of each
       Python file installed by `make install-python-shims`).
     - Capability set: `wasm-tools component wit <wasm> | grep import`.
     - Bound capabilities: parse `scripts/compose-python-component.sh`
       for the `--plug` arguments and sha256 each one.
   * Writes a TOML matching `schemas/pyforge-manifest.schema.json`.
2. JSON Schema for the manifest, used by the emitter + by every
   downstream tool for validation.
3. One canonical manifest checked into the repo:
   `pylon-forge/manifests/cpython-3.14.0-wasm32-wasip2-pylon0.toml`.
   This becomes the contract Phase 1 reproduces.

**Definition of done.**
* Emitter runs against this repo's current `python.composed.wasm`
  without error.
* `forge_identity` field is deterministic across two invocations on
  the same inputs.
* Schema doc updated with whatever gaps Phase 0 surfaced (and there
  will be gaps — that's the point).

**Risks.**
* CPython source identity is fragile if `fetch-cpython.sh` doesn't
  pin to a commit. May need to upgrade that script first.
* Capability sha256 over `~/git/<repo>/build/*.wasm` is unstable
  across rebuilds. Want to address by sha256 of the *checked-in
  source + build steps*, not the artifact — but that's already
  Phase 2 work. Phase 0 just records what's there.

**Effort.** 3–5 days. Most of the work is iterating the schema
against reality.

---

## Phase 1 — Reproducible Builds

**Goal.** `pylon forge build manifest.toml` produces a
`python.composed.wasm` whose sha256 matches a recorded reference, on
any machine, given the same inputs.

**Why this is the keystone.** Every other phase assumes reproducible
builds. Without it, "cap-forge identity" is meaningless (you can't
say "this is the same forge as before" if rebuilding produces
different bytes).

**Deliverables.**

1. **Content-addressed storage (CAS).** Local filesystem implementation:
   ```
   ~/.pylon/cas/sha256/ab/cd/abcdef…/
   ```
   `pylon cas add <file>` returns the sha256; `pylon cas get <sha256>`
   resolves to a path. Two-byte prefix to avoid 100k-entries-in-one-dir
   issues. Atomic adds via temp-file + rename.

2. **Source-fetcher.** `pylon source fetch <manifest>`:
   * Downloads CPython tarball at pinned URL+sha256.
   * Applies pinned patches.
   * Returns a populated source dir in `~/.pylon/sources/<sha>/`.

3. **Toolchain-fetcher.** Same for wasi-sdk.

4. **Build orchestrator.** `pylon forge build manifest.toml`:
   * Resolves toolchain.
   * Resolves CPython source.
   * Resolves capability components by sha256 from CAS (must be
     pre-populated; fails clean if missing — Phase 2 fixes this).
   * Runs configure with `manifest.python.config.configure_args`.
   * Wires `cpython-ext/` extensions (absorbs `wire-cpython-ext.sh`).
   * Runs `Tools/wasm/wasi build`.
   * Installs stdlib overlay from manifest.
   * `wac plug`s the capability components.
   * Verifies output sha256 matches manifest, OR records it if
     manifest says `expect_sha = "<update-me>"`.

5. **Reproducibility CI test.** Build the canonical manifest twice in
   clean environments. Compare sha256. Must match.

**Definition of done.**
* Building this repo's current artifact from its Phase-0 manifest
  produces a bit-identical `python.composed.wasm`.
* CI runs the reproducibility test on every push.
* `pylon forge build` runs on macOS and Linux (the two hosts the
  current Makefile supports).

**Gates.**
* **Reproducibility check.** If two builds produce different sha256:
  do not move to Phase 2. The likely culprits are
  `SOURCE_DATE_EPOCH` gaps, wit-bindgen-c version drift, or
  non-deterministic configure outputs (timestamp injection,
  randomized symbol naming). Fix at the build level, not by hashing
  around it.

**Risks.**
* **CPython build determinism.** Existing
  `SOURCE_DATE_EPOCH=1770132740` in the Makefile gets us most of the
  way, but `python -c "import marshal; …"` can embed timestamps,
  `Programs/python.c` embeds build identity strings, and frozen
  modules' bytecode embeds source file paths. Each of these is
  fixable but not trivial.
* **wit-bindgen-c version pinning.** The bindings under
  `cpython-ext/_*/gen/` are regenerated from WIT against a specific
  wit-bindgen version. The manifest needs to pin that. Currently
  uses `~/.cargo/bin/wit-bindgen` from cargo-installed; replace with
  pinned binary in CAS.
* **Cross-build host toolchain.** The build runs a host CPython
  (`aarch64-apple-darwin23.6.0/python.exe`) as a build dependency.
  That host CPython is *also* a forge in principle. Phase 1
  intentionally treats it as a black-box dep on system Python ≥3.12;
  Phase 1.5 (if needed) makes it explicit.

**Effort.** 2–3 weeks. The build orchestrator is the big chunk;
absorbing `wire-cpython-ext.sh` + `compose-python-component.sh` is
maybe 4–5 days. Reproducibility debugging is the wild card — could
be days, could be 2 weeks.

---

## Phase 2 — Recursive Cap Forges

**Goal.** Each capability component (`compression-multiplexer`,
`sqlite-wasm`, `openssl-wasm`, `crypto-hash-multiplexer`,
`hashing-multiplexer`, `v86-posix-stub`) becomes its own forge with
its own manifest. `pylon cap build <cap-manifest>` produces the
component and adds it to the CAS. The Python-side forge resolves
cap-side forges as build dependencies.

**Why this scope.** Phase 1 leaves caps as "fetch by sha256 from CAS,
pre-populated by hand". Phase 2 makes cap building itself reproducible
and managed. Without it, "the python forge is reproducible" is true
in a way that depends on whoever populated the CAS.

**Deliverables.**

1. **Cap manifest schema** (sibling of pyforge-manifest):
   ```toml
   [cap]
   package         = "tegmentum:compression-multiplexer"
   version         = "0.1.0"
   source_url      = "ssh://git@github.com/tegmentum/compression-multiplexer.git"
   source_commit   = "12bb76e..."
   build_system    = "cargo-component"

   [cap.toolchain]
   rust_toolchain  = "1.83.0"
   wit_bindgen     = "0.44.0"
   cargo_component = "0.21.0"
   wasi_sdk        = "33.0"

   [cap.build]
   target          = "wasm32-wasip2"
   profile         = "release"
   features        = ["zdict_builder"]

   [cap.wit]
   exports = [
     { interface = "compression-dispatcher", version = "0.1.0" },
     { interface = "zstd-extras",            version = "0.1.0" },
   ]

   [cap.artifact]
   path            = "target/wasm32-wasip2/release/compression_multiplexer.wasm"
   sha256          = "..."   # post-build
   ```

2. **`pylon cap build <manifest>`** verb:
   * Clones source at pinned commit.
   * Resolves toolchain (rustup + cargo-component + wit-bindgen).
   * Runs the build per `build_system`.
   * Validates exported WIT matches `cap.wit.exports`.
   * Adds artifact to CAS.

3. **Cap manifest emitters** for each existing cap repo:
   * `pylon cap emit-manifest ~/git/compression-multiplexer/` — for
     existing builds.
   * Migrate the 6 in-use caps. Each is a separate PR-shaped chunk:
     1. compression-multiplexer
     2. sqlite-wasm (CMake build system — different code path)
     3. openssl-wasm (different build system again — likely Make)
     4. crypto-hash-multiplexer (cargo-component)
     5. hashing-multiplexer (cargo-component)
     6. v86-posix-stub (cargo-component)

4. **Recursive resolver.** `pylon forge build` now reads
   `[capabilities.bound]` entries; if the sha256 isn't in CAS, looks
   up the cap manifest and builds it first. Builds a dep graph,
   runs cargo-component / cmake in correct order.

5. **Build-system adapters.** Each `build_system` value in cap
   manifests gets an adapter in `src/pylon/builders/`:
   - `cap_cargo_component.py` — for Rust caps
   - `cap_cmake.py` — for sqlite-wasm
   - `cap_make.py` — for openssl-wasm

**Definition of done.**
* All 6 production caps have manifests checked in.
* `pylon forge build python.toml` builds from scratch with empty
  CAS — populates everything, including all caps, including
  toolchain installs.
* Cap rebuilds produce bit-identical wasm given the same manifest.

**Gates.**
* After cap #2 migrates: review the build-system-adapter pattern. If
  CMake and cargo-component need wildly different orchestration,
  consider a "do it via shell script per cap" escape hatch before
  abstracting too hard.
* If any cap rebuild is non-reproducible: same drill as Phase 1.

**Risks.**
* **Build-system fragmentation.** sqlite-wasm uses CMake.
  openssl-wasm uses Make + custom scripts. cap_* adapters might
  proliferate.
* **WIT version drift across caps.** compression-multiplexer's WIT
  vendored into python-wasm is at commit X; if compression-mux
  itself moves to commit Y with a new WIT, the python-side bindings
  need re-gen. Manifest needs a `wit_source` pointing back at the
  cap manifest so this stays in sync.
* **Cap source-fetch over private repos.** Most `tegmentum/*` caps
  are private. Need `gh auth` or SSH-key-based clone. Don't bake
  credentials into manifests; the CAS-side caches the resolved
  artifact so manifests stay portable.
* **Multiple-Rust-toolchain coordination.** Different caps might
  need different `rust_toolchain` values. The pylon tool can use
  rustup to install on demand, but the disk-cost grows fast.

**Effort.** 3–4 weeks. The first cap migration takes a week (build
the adapter pattern from scratch). The rest take 2–3 days each.

---

## Phase 3 — Wheel Resolver

**Goal.** Wheels declare which capabilities they require; the resolver
matches wheels to forges accordingly. `pip install some-wheel` (or a
pylon-native equivalent) refuses to install a wheel whose required
caps aren't satisfied by the active forge.

**Why this matters.** Without it, the artifact identity work is just
internal hygiene. Users hit ImportErrors at runtime when they install
wheels that assume capabilities the forge doesn't provide.

**Deliverables.**

1. **Metadata spec.** `docs/wheel-metadata-extension.md` describing
   the proposed `Required-Capability:` header. Either:
   * Submit as a PEP (long, ecosystem-coordinating).
   * OR ship as a pylon-private extension (faster, less politically
     entangled, but doesn't compose with `pip` directly).

   Recommend pylon-private to start; promote to PEP if usage grows.

2. **Wheel-tag derivation.**
   ```
   pylon resolver wheel-tag <forge-identity>
   → ['cp314-cp314-wasm32_wasi_component_v1_pylon1']
   ```
   This becomes the value of `--platform` for any wheel-build
   targeting this forge.

3. **Wheel-build helpers.**
   ```
   pylon wheel build <project> --forge <forge-identity>
   ```
   Wrapper around `pip wheel` that:
   * Sets the right env vars (PIP_PLATFORM, _PYTHON_HOST_PLATFORM).
   * Injects `Required-Capability:` headers based on the project's
     `pyproject.toml [tool.pylon.capabilities]` section.
   * Validates the resulting wheel before publishing.

4. **Compatibility check.**
   ```
   pylon resolver compatible <wheel.whl> <forge-identity>
   ```
   Returns 0/1 + diagnostic message:
   - Requires-Python out of range
   - Platform tag mismatch
   - Missing capability `tegmentum:foo/bar@0.1.0`
   - Capability version mismatch

5. **Install integration.**
   ```
   pylon install <requirement> --forge <forge-identity>
   ```
   Resolves wheels against the forge identity; rejects incompatible
   wheels with clear error message.

6. **At least one demo wheel** with `Required-Capability:` metadata,
   proving the end-to-end flow.

**Definition of done.**
* Compatibility check rejects a wheel built for a forge missing a
  required cap, accepts one for a matching forge.
* The python-wasm project can declare its own capability requirements
  in `pyproject.toml` and produce a wheel that resolver picks up.

**Gates.**
* If the metadata extension proves controversial in upstream Python
  packaging discussion (likely), don't block on it — ship as a
  pylon-private extension and revisit the standardization question
  later.

**Risks.**
* **PEP politics.** Proposing wheel metadata extensions is an
  ecosystem coordination problem. Reasonable people will disagree
  about the namespace, syntax, and the role of capabilities in
  wheel resolution. Stay private until the design has 2-3 real
  consumers to point at.
* **pip compatibility.** A pylon-private extension means `pip install`
  doesn't honor `Required-Capability:`. Either users use
  `pylon install` exclusively (UX cost) or there's a pylon-pip-plugin
  layer (engineering cost).
* **Cap version negotiation.** "Wheel requires
  `tegmentum:compression-multiplexer/zstd-extras@^0.1.0`; forge
  provides 0.1.5" — is that compatible? Need a version-range model
  for capabilities. SemVer-ish but at the WIT level (additive
  changes = compatible; renames = incompatible).

**Effort.** 3–4 weeks. Most of the work is in the resolver logic
and the wheel-build helper; the metadata spec is paper.

---

## Phase 4 — Multi-forge Coexistence

**Goal.** Multiple forges live side-by-side on one machine. Users
select between them per-shell or per-project. Wrapping `python`
binary dispatches to the right forge.

**Deliverables.**

1. **Forge installer.** `pylon use cpython-3.14.0-wasm32-wasip2-pylon1`:
   * Writes `~/.pylon/active` (machine default) or
     `~/.pylon/shells/<shell-pid>/active` (per-shell, set via env
     hook).
   * The wrapping `~/.pylon/bin/python` reads this and execs the
     right artifact.

2. **Per-project pinning.** `.pylonrc` (TOML) at project root:
   ```toml
   [forge]
   identity = "cpython-3.14.0-wasm32-wasip2-pylon1"
   ```
   `~/.pylon/bin/python` walks up from cwd looking for `.pylonrc`
   before falling back to the per-shell or machine default.

3. **Shell integration.** Optional hooks for bash/zsh/fish that
   put a forge-aware `python` on PATH first. Print the active forge
   identity in the shell prompt if requested.

4. **`pylon doctor`.** Diagnostic verb that prints the active forge,
   its location, its manifest, and any obvious problems
   (missing cap components, missing toolchain, expired toolchain
   cache, etc.).

**Definition of done.**
* Install two forges (`pylon1` and `pylon2`); `pylon use` switches;
  `python --version` reports the right identity for each.
* A test project with `.pylonrc` runs against its pinned forge
  even when a different one is active globally.

**Gates.** None — this is UX layer, doesn't gate other phases.

**Risks.** Low. UX iteration is the main cost.

**Effort.** 1–2 weeks. Mostly polish.

---

## Cross-cutting concerns

### Storage budget

The CAS holds: every forged Python artifact, every cap component
version, every toolchain version. A rough estimate:

| Item | Size each | Count after 6 months |
|---|---|---|
| python.composed.wasm | ~40 MB | ~20 forges × 1 = 20 (mostly minor-version variants) |
| Cap components | 0.5–5 MB | ~10 caps × ~5 versions = 50 |
| CPython source tarballs | ~30 MB | ~5 active versions |
| wasi-sdk tarballs | ~150 MB | ~3 active versions |
| **Total** | | **~2 GB** |

Manageable on developer machines. A `pylon gc --keep-latest=3`
verb keeps the active set bounded.

### Networking and source-of-truth

Where do CAS-addressed artifacts come from for fresh installs?

Options:
* **OCI registry** — re-use a Docker/GitHub container registry. Free
  for public; private requires auth setup.
* **gh release artifacts** — simple, free, GitHub-native; size limits
  per release (~2GB).
* **S3 / R2** — flexible, costs money.
* **Mirror of `~/git/<cap>/build/`** — for the cap repos we own.

Recommend GitHub release artifacts to start; migrate if usage grows.

### Auth

The cap repos (`tegmentum/*`) are private. Cap source-fetch needs
either:
* `gh auth token` (works with GitHub CLI session)
* SSH key
* GitHub App-issued token (for CI)

The pylon tool should look for an `~/.config/pylon/auth.toml` and
fall back to env (`GH_TOKEN`, `GITHUB_TOKEN`).

### Test infrastructure

Each phase has a definition-of-done that includes tests. Aggregate:

* Phase 0: emitter round-trip test (emit → parse → re-emit; must
  match).
* Phase 1: bit-identical reproducibility test (build twice in clean
  env; sha256 must match).
* Phase 2: cap rebuild reproducibility (per cap).
* Phase 3: compatibility-check truth table (matrix of forge × wheel
  scenarios).
* Phase 4: forge-switch + `.pylonrc` discovery (shell-level test).

Plus the substrate-bound test suite from `pylon-pyforge.md §
Forge-aware testing` — those tests live under
`pylon-forge/tests/substrate/` and run as part of `pylon forge build`.

### Migration path for python-wasm itself

The python-wasm repo today has its own build system (`Makefile` +
shell scripts). Migrating to pylon-driven builds doesn't have to be
big-bang:

* **Phase 0–1**: pylon coexists. The Makefile still works; pylon
  emits manifests describing its outputs.
* **Phase 2**: `make python-composed` calls `pylon forge build` under
  the hood for the actual work, but keeps the make-level UX.
* **Phase 3+**: pylon is the canonical interface. Makefile becomes a
  thin wrapper or goes away.

Don't break the existing `make build && make python-composed` flow
at any point. Anyone using this repo should be able to ignore pylon
through all of Phase 1.

---

## Risks (cross-phase)

* **Reproducibility is harder than it looks.** SOURCE_DATE_EPOCH
  handles most timestamps but not all. CPython embeds build identity
  strings in several places (`Py_GetVersion`, `__build__` attribute,
  frozen module paths). Some of these are fixable upstream; others
  need post-build patching.

* **WIT version drift across the ecosystem.** wasi-sdk 33 emits
  @0.2.4 imports by default; the multiplexers were rebuilt to match
  python-wasm at @0.2.6. The cap-forge manifest needs to pin the
  effective wasi version per cap, and the python-forge manifest
  needs to validate cross-cap consistency.

* **Closure of capability set is fragile.** A new capability becomes
  available, gets composed in, but a wheel built against the
  capabilities-before-it now needs re-resolution. The forge identity
  changes when caps change, which is correct — but consumers need
  guidance on when to re-lock.

* **Free-threading / no-GIL CPython splits the universe.** `cp314`
  and `cp314t` are different ABIs. The forge model handles this via
  `features.free_threading`, but the wheel ecosystem needs separate
  caches per flag. Could double the wheel-build volume.

* **CMake + cargo-component + make build systems don't unify
  cleanly.** Cap-side build adapters might multiply faster than
  hoped.

---

## Decision points / gates summary

| When | Decide |
|---|---|
| End of Phase 0 | Schema revisions before locking into Phase 1 |
| End of Phase 1 | Reproducibility holds? If no, debug before Phase 2 |
| After cap #2 migrates | Build-adapter pattern works or needs escape hatch? |
| Start of Phase 3 | Submit metadata extension as PEP, or keep pylon-private? |
| Mid Phase 3 | If PEP discussion blocks, do we have 2–3 consumers to keep momentum? |

---

## Out of scope (explicit non-goals)

* **Replacing pyenv / asdf / mise.** Those manage *system* Python
  installs; pylon-forge manages *forge* identity for the
  wasm-targeted Python builds this org cares about. If overlap
  becomes useful, that's a future bridge.

* **Replacing pip's resolver.** Phase 3 adds capability-awareness
  on top of pip's existing resolution. Don't reimplement
  dependency resolution.

* **Cross-target Python builds in one forge.** A forge is for one
  target (`wasm32-wasip2`). Building for `x86_64-linux` is a
  different forge identity; pylon-forge can manage many but doesn't
  conflate.

* **GUI / TUI.** CLI only. If a TUI becomes wanted post-Phase 4,
  build it as a separate tool consuming pylon's library API.

* **Replacing conda / mamba.** Conda manages
  packages-and-environments; pylon-forge manages
  interpreter-and-substrate. Adjacent concerns.

---

## What "first usable system" means

Definition of "Phase 1 done, Phase 2 partially done, the org can use
it for one workflow":

* The python-wasm web demo can be built via
  `pylon forge materialize manifests/python-wasm-web-demo.lock.toml`
  on any team member's machine.
* The build is bit-identical to the reference.
* The cap-side components are still built by hand (one cap migrated
  proves the pattern, others stay manual until needed).
* No wheel resolver yet; pip installs are out-of-band.
* No multi-forge UX; the forge identity is whatever
  `materialize` produced.

That's 5–7 engineer-weeks. After that, each subsequent capability
gained (cap-side forges, wheel resolver, multi-forge) has a clear
value proposition and a clear cost.

---

## Status

**Phase 0 landed** (Jan 2026). `pylon-forge/` repo created at
`~/git/pylon-forge/` (local-only). Emits a 243-line manifest for the
current `python.composed.wasm`; round-trip is byte-identical, forge
identity is deterministic across runs.

  ```bash
  $ pylon emit-manifest -o manifests/python-wasm-current.toml
  forge_identity: 121c14fea151b4435e24e430e4fc4172c2f971e81e3fabf3df63b4be8ab57b3e

  $ pylon validate manifests/python-wasm-current.toml
  OK: parses, identity matches, re-emit is byte-identical
  ```

Phase 1 (reproducible builds) is the next target.

### Phase 0 retrospective — schema gaps surfaced

Six observations from inspecting a real artifact through the schema.
These should be addressed before Phase 1 hardens the build pipeline
against the manifest.

1. **Multi-interface caps inflate `[[capabilities.bound]]`.** The
   openssl-component exports 10 interfaces (error/pkey/x509/tls/
   random/digest/cipher/mac/kdf/bignum); the emitter records one
   `[[capabilities.bound]]` entry per (interface × component), so a
   single .wasm appears 10 times with identical sha256 + source. Two
   options for the next revision:

   * **Group by component** — one entry per .wasm, with `provides =
     ["iface1", "iface2", ...]`. Cleaner manifest; loses the 1:1
     map from required → bound.
   * **Keep one per interface** — current behavior. Inflates the
     manifest but each entry is self-contained and matches the
     resolver's query shape (`compatible(wheel, forge)` asks per
     interface).

   Recommendation: keep one-per-interface for the resolver, add a
   separate `[[components]]` section that lists unique .wasm files
   by sha256 so the CAS layer has a clean list of artifacts to fetch.

2. **`source_tarball_url` is a misnomer when carrying git refs.**
   The emitter records `git+local:/path/to/cpython@<commit>` for the
   common case (CPython is checked in as a submodule, not fetched as
   a tarball). Either rename to `source_url` (carrying any URI:
   `https://`, `git+ssh://`, `git+local:`) or split into two fields
   for the two cases. Renaming is cheaper.

3. **`wasi_sdk_sha256` is the clang-binary hash, not the SDK tarball
   hash.** The full SDK is ~150 MB; hashing only the clang binary
   (a few MB) is a cheap proxy for "the toolchain that produced this
   build" but is technically a different identity axis. Either
   document the choice explicitly in the schema, or add a
   `wasi_sdk_clang_sha256` field for the proxy and reserve
   `wasi_sdk_sha256` for the tarball.

4. **No way to discover frozen_modules from a built artifact.**
   `frozen_modules` and `frozen_modules_sha256` are empty in the
   Phase 0 output. Would need to either (a) parse the build-time
   flag (Phase 1: we own the build), (b) introspect the bare wasm
   for the frozen-module section, or (c) run `python -X
   frozen_modules=on` at materialize time. Pushing to Phase 1.

5. **`packaging` section is boilerplate.** The emitter hardcodes
   `wheel_policy = "pylon-component-wheel-v1"` and
   `native_extension_policy = "external-wasm-component"`. In Phase 3
   these come from real configuration (e.g. `.pylonrc` or a sibling
   file). Phase 0 just stamps in the defaults.

6. **`stdlib_overlay.purpose` is from a hardcoded table.** The
   emitter walks `make install-python-shims` for the `cp` commands
   but doesn't know what each shim's *intent* is — that comes from
   a lookup table in `emitter.py`. As new shims get added the table
   needs manual updates. A cleaner approach: a `.pylon-shim.toml`
   sidecar next to each shim declaring its purpose + the Lib/ path
   it shadows. The emitter then needs nothing hardcoded. Defer until
   the count of shims justifies the indirection (maybe ~10+).

These six items go into the manifest spec revision before Phase 1
build orchestration starts depending on the schema being stable.

### Phase 1 progress

**Phase 1.0 (schema revisions) landed** in pylon-forge `6648782`.

* Gap #1 (multi-interface caps inflate bound entries) closed by
  splitting `[[capabilities.bound]]` (one entry per interface, FK
  via `component_sha256`) from a new `[[components]]` section
  (one entry per unique .wasm). openssl-component now appears
  once in `[[components]]` with all 10 interfaces in `provides`,
  while `[[capabilities.bound]]` keeps the per-interface query
  shape the resolver needs.

* Gap #2 (source_tarball_url misnomer) closed by renaming to
  `source_url`. Reader keeps a legacy-key fallback for one
  revision window so existing manifests parse cleanly.

The canonical manifest re-emitted under the new schema is now
241 lines (was 243; slightly tighter despite added structure).

**Phase 1.1 (CAS primitive) landed.** `~/.pylon/cas/sha256/ab/cd/<full-hex>/`
layout with atomic adds (os.link → tempfile rename), integrity-checked
reads (re-hash on get; IntegrityError on mismatch), idempotent re-add.
15/15 unit tests pass. CLI verbs: `pylon cas list/add/get` with
`--cas-root` for sandboxing.

**Phase 1.2 (`pylon forge populate-cas`) landed.** Walks a manifest's
named files (components, stdlib_overlay shims, artifacts) and adds each
to the CAS. Mismatch detection: if the CAS-computed sha256 disagrees
with the manifest's claim, surfaces as `mismatched` in the report.

Validated against the canonical `python-wasm-current.toml`:
**14/14 entries added, 0 missing, 0 mismatched, 83.5 MiB**.
Every sha256 the manifest claims is self-consistent end-to-end —
the schema's data layer is sound, the CAS can ingest a full forge
artifact bundle, and `pylon forge build` (Phase 1.3) can build on
a known-good substrate.

### Pivot: composectl is the substrate

Mid-Phase-1.3, discovered that
`~/git/webassembly-component-orchestration` is a substantial Rust
implementation of `sys:compose@1.0.0` providing `composectl` with
`blob put/get/has/list`, `plan validate/info`, `emit build`, `exec`,
`reflect`, `trust`, `secrets`, `metrics`. python-wasm already
integrates via `scripts/build-composectl-plan.sh` → `plans/python-
browser.json` + `plans/python-v86.json`. The Makefile's
`composectl-plan` target was waiting for "composectl's emit
dep-wiring fixed upstream" before becoming the production path.

So pylon-forge's CAS (Phase 1.1) + populate-cas (Phase 1.2) +
ComposeStage (planned Phase 1.3 work) **substantially duplicate**
composectl's existing primitives. Rather than maintain two parallel
implementations, pylon **pivots to delegation**:

  * **pylon owns the Python-specific identity layer.**
    pyforge-manifest captures ABI tags, build flags, stdlib_overlay,
    capability-set requirements — things composectl's plan format
    doesn't model and shouldn't (they're cross-cutting CPython
    concerns, not WIT-component-graph concerns).
  * **composectl owns the WIT/component layer.** Blobs, plans,
    deterministic composition, attestation.
  * **pylon translates pyforge-manifest → composectl plan,** then
    delegates the staging + composition to composectl.

New layering:

```
   pyforge-manifest.toml               <- pylon's authored truth
        │ pylon plan
        ▼
   composectl plan (JSON)              <- delegated to composectl
        │ composectl blob put × N
        │ composectl emit build (when upstream gap closed)
        ▼
   python.composed.wasm
```

Code reshape in pylon-forge (commit `dec88c5`):

* Removed `src/pylon/cas.py` (composectl blob store is the truth).
* Removed `src/pylon/populate.py` (composectl blob put does this).
* Removed `src/pylon/build.py`'s `ComposeStage` (composectl emit
  build is the truth, even with the upstream gap).
* Added `src/pylon/composectl.py` — thin subprocess wrapper around
  the composectl binary (find_composectl, blob_put, plan_validate,
  emit_build).
* Added `src/pylon/plan.py` — translates pyforge-manifest →
  composectl plan dict + collects source paths for each component.
* `pylon forge plan` + `pylon forge build` CLI verbs. The latter
  stages blobs + emits + validates; `--emit` opts into composectl
  emit build (off by default per the upstream gap).

Cost: ~370 lines of CAS + populate-cas built in commit `6648782`
turned out to be parallel-implementation work. Lesson saved in
memory note `composectl-is-the-substrate.md` so future sessions
don't repeat it.

### Phase 1.3 (pivoted) — composectl delegation landed

`pylon forge build manifests/python-wasm-current.toml` against the
canonical manifest:

```
composectl: /Users/zacharywhitley/git/webassembly-component-orchestration/target/release/composectl
manifest:   manifests/python-wasm-current.toml
repo:       /Users/zacharywhitley/git/python-wasm
plan_out:   /Users/zacharywhitley/git/python-wasm/plans/python-pylon.json

  + emitted plan: /Users/zacharywhitley/git/python-wasm/plans/python-pylon.json
  + staging blobs (in composectl's per-repo store at .../.compose/blobs)
      root        d2ff4c728f68975b…  python.wasm
      openssl     5bed74a40b863bd1…  openssl-component.wasm
      sqlite      95912ce2609df102…  sqlite-core.wasm
      crypto-hash c63eb22349a6166c…  crypto_hash_multiplexer.wasm
      hashing     c665cbd9c9c2fe9f…  hashing_multiplexer.wasm
      compression ccd2928985b65fe4…  compression_multiplexer.wasm
      v86         f974d021eba63c93…  v86_posix_stub.wasm
  + plan validates

OK
```

The emitted plan is shape-compatible with the existing
`scripts/build-composectl-plan.sh` output (same `version`/`root`/
`components`/`bindings`/`policy` JSON schema). The actual compose
step still uses `wac plug` via the existing
`scripts/compose-python-component.sh` (dev fast path) until
composectl's emit dep-wiring catches up.

### Phase 1.4 — reproducibility verification landed

`pylon forge verify` runs the testable subset of the Phase 1.4
contract:

```
=== manifest emission ===
  two emissions: byte-identical TOML                      OK  6735 bytes
  two emissions: same forge_identity                      OK  cbd6821cff7ab525…

=== forge_identity ===
  parse(emit(repo)).identity == emit(repo).identity       OK
  re-emit after parse is byte-identical to original       OK

=== plan emission ===
  two plan emissions: byte-identical JSON                 OK  6735 bytes
  two plan emissions: same component count                OK  7 components
  two plan emissions: same binding count                  OK  17 bindings

=== plan emission purity ===
  plan(in-mem) == plan(parsed-from-toml)                  OK

=== write_plan determinism ===
  two write_plan() calls: byte-identical files            OK  6736 bytes
```

8/8 pass. Suitable as a CI gate today: `pylon forge verify`
returns 0 iff every reproducibility invariant holds for the current
repo state.

**Important Phase 1.4 finding: `wac plug` is non-deterministic.**
Probed during this phase. Two back-to-back runs of
`scripts/compose-python-component.sh` against identical inputs
produce identical-size python.composed.wasm files
(43,739,345 bytes each) with **different sha256**:

```
$ shasum -a 256 build/python.composed.wasm   # run 1
b60a003b21ebf92e5a160289e6c650c4da1d33d73c3cb868d45e106ddf4ca21a

$ shasum -a 256 build/python.composed.wasm   # run 2
5b6478cf5f5c8826532e7370a24ad3746759f4670471f34639fad90446468440
```

Byte-level diff starts at offset ~0x3486 inside a sub-component's
LEB128 length prefix — wac is reordering embedded components run-
to-run. The `producers` custom sections total the same bytes but
appear at different offsets in each output. This is wac plug's
issue, not anything our manifest/plan layer can fix.

So the gate "`pylon forge build` produces bit-identical
python.composed.wasm" needs **upstream wac fix** or pivot to
composectl emit build (which has its own dep-wiring gap). Phase 1.4
treats this as **documented, not blocking** — the bytes are
functionally equivalent (same total size, same custom section
contents, presumably same runtime behavior), just laid out
differently.

Tracked as a deferred deeper gate; Phase 2 (recursive cap forges)
can start without it.

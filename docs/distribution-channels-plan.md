# `python-wasm` distribution channels — implementation plan

Status: **draft**. Phase D of
[`pip-wheel-wrapper-plan.md`](pip-wheel-wrapper-plan.md), split
into D₁ (how python-wasm itself reaches users) and D₂/D₃ (how the
launcher orchestrates rebuild + extension install).

## The four channels

Everything below builds on **one core artifact** — a self-contained
directory:

```
python-wasm-<version>-<platform>/
├── bin/
│   └── python-wasm                ← the launcher
├── lib/
│   ├── python.composed.wasm       ← bundled WASM interpreter (~16 MiB)
│   └── cpython-ext-base/          ← sources for `python-wasm rebuild`
├── share/
│   └── python-wasm/
│       ├── LICENSE
│       └── README.md
└── VERSION
```

Each channel is a different way to get that directory onto a user's
machine + a `python-wasm` command on their PATH.

| Channel | Audience | Trade-off |
|---|---|---|
| **Standalone tarball** | manual installs, CI, air-gapped envs | most portable, user manages PATH |
| **`install.sh`** | most CLI users (uv/rustup pattern) | one-liner; needs a stable URL |
| **Homebrew** | macOS + Linux Homebrew users | brew handles upgrades + PATH; needs a tap |
| **pip wheel** | Python users who already have CPython | familiar; doubles as the entry to extension wheels |

Plus optional but worth shipping at the same time:

- **Docker image** — `ghcr.io/tegmentum/python-wasm:latest`. Ships
  wasmtime too; one-command run.
- **GitHub Release artifacts** — the tarballs, install.sh, and a
  `checksums.txt` for verification.

## D₁a — Standalone tarball (the foundation)

### Deliverable

`scripts/package-release.sh` produces one tarball per platform:

```
dist/release/python-wasm-0.1.0-darwin-arm64.tar.gz
dist/release/python-wasm-0.1.0-darwin-x86_64.tar.gz
dist/release/python-wasm-0.1.0-linux-x86_64.tar.gz
dist/release/python-wasm-0.1.0-linux-arm64.tar.gz
```

The wasm artifact is the same across platforms (it's wasm). What
differs is the launcher: on macOS and Linux it's a small shell
script; on Windows it would be a `.cmd` file.

### Launcher

`bin/python-wasm` is a shell script (~30 lines) that:

1. Resolves `lib/python.composed.wasm` relative to its own location.
2. Looks for an opt-in override at `~/.python-wasm/python.composed.wasm`
   (the rebuild-output location).
3. Dispatches subcommands (`stage`, `rebuild`, `extensions list`,
   `info`, `doctor`) to sibling scripts under `lib/python-wasm/cli/`.
4. For everything else (the default case), exec's `wasmtime run …
   python.composed.wasm "$@"`.

No external Python dependency. Wasmtime is the only requirement.

### Exit criterion

`tar xzf python-wasm-0.1.0-$(uname).tar.gz && ./python-wasm-0.1.0/bin/python-wasm
-c "print(1+1)"` prints `2` on a fresh machine with only wasmtime
preinstalled.

## D₁b — `install.sh`

### Deliverable

`scripts/install.sh` published to a stable URL (e.g.
`https://python-wasm.dev/install.sh` or
`https://raw.githubusercontent.com/tegmentum/python-wasm/main/install.sh`).
Detects platform, fetches the right tarball, untars to
`~/.python-wasm/install/`, symlinks `bin/python-wasm` into
`~/.local/bin/` (or `/usr/local/bin/` with sudo).

Pattern mirrors `rustup`, `uv`, `bun`'s installers. Stays small (~100
lines of POSIX shell).

### Behaviors

- `curl -fsSL https://python-wasm.dev/install.sh | sh` — interactive,
  installs latest.
- `curl … | sh -s -- --version 0.1.0` — pin to a version.
- `curl … | sh -s -- --prefix ~/python-wasm-test` — install elsewhere.
- `curl … | sh -s -- --no-modify-path` — for CI where the script
  shouldn't touch shell rc files.
- Detects wasmtime; if missing, prints exactly the command to install
  it (per platform: `brew install wasmtime`, `curl … wasmtime install`,
  etc.).

### Exit criterion

On a clean macOS + clean Linux VM with wasmtime preinstalled, the
one-liner ends with `python-wasm --version` printing the version.

## D₁c — Homebrew

### Deliverable

`packaging/homebrew/python-wasm.rb` — the Formula. Lives in
`tegmentum/homebrew-tap` (a separate repo). The formula:

```ruby
class PythonWasm < Formula
  desc "CPython interpreter running on WebAssembly (WASI Preview 2)"
  homepage "https://python-wasm.dev"
  url "https://github.com/tegmentum/python-wasm/releases/download/v#{version}/python-wasm-#{version}-darwin-arm64.tar.gz"
  sha256 "..."
  version "0.1.0"
  depends_on "wasmtime"

  def install
    bin.install "bin/python-wasm"
    libexec.install Dir["lib/*"]
    share.install Dir["share/*"]
  end

  test do
    assert_match "Python 3.14.3", shell_output("#{bin}/python-wasm --version")
  end
end
```

### Tap

A separate `tegmentum/homebrew-tap` repo. Initial layout:

```
homebrew-tap/
├── README.md
└── Formula/
    └── python-wasm.rb
```

User installs with:

```bash
$ brew tap tegmentum/tap
$ brew install python-wasm
```

(Could later push for `homebrew-core` inclusion. That's a separate
PyPA-style policy track.)

### Exit criterion

`brew tap tegmentum/tap && brew install python-wasm && python-wasm
-c "print(1+1)"` works on a clean macOS.

## D₁d — pip-installable launcher

### Deliverable

`packaging/pypi/` — a regular Python package that publishes as
`python-wasm` on PyPI. The wheel ships the same core artifact (wasm +
sources + launcher) bundled as package data.

```
python_wasm-0.1.0-py3-none-any.whl
└── python_wasm/
    ├── __init__.py
    ├── cli.py                     # the entry point
    ├── data/
    │   ├── python.composed.wasm   # bundled wasm
    │   └── cpython-ext-base/      # rebuild sources
    └── entry_points.txt:
          [console_scripts]
          python-wasm = python_wasm.cli:main
```

`cli.py` is a Python wrapper that:

- Locates `python.composed.wasm` via `importlib.resources`.
- Dispatches subcommands to sibling Python modules.
- For the default case, `os.execvp("wasmtime", [...])`.

### Why `py3-none-any` and not a platform wheel

The wasm artifact is platform-independent. The launcher only shells
out to wasmtime. So a single wheel works on every platform; no
per-OS wheels needed.

### Trade-off vs the other channels

- ✅ User has pip → one install.
- ✅ Trivially upgrades via `pip install --upgrade python-wasm`.
- ❌ User must already have CPython.
- ❌ Still needs wasmtime installed separately.

So this is the right shape for the audience that already lives in
the Python ecosystem; doesn't replace the standalone channels for
the rest.

### Exit criterion

`pip install python-wasm && python-wasm -c "print(1+1)"` works in a
fresh venv with wasmtime preinstalled.

## D₁e — Docker

### Deliverable

`packaging/docker/Dockerfile`:

```dockerfile
FROM debian:bookworm-slim AS base
ARG WASMTIME_VERSION=27.0.0
RUN apt-get update && apt-get install -y curl xz-utils ca-certificates && \
    curl -fsSL "https://github.com/bytecodealliance/wasmtime/releases/download/v${WASMTIME_VERSION}/wasmtime-v${WASMTIME_VERSION}-x86_64-linux.tar.xz" \
      | tar xJ -C /usr/local/bin --strip-components=1 wasmtime-v${WASMTIME_VERSION}-x86_64-linux/wasmtime

FROM base
COPY dist/release/python-wasm-*-linux-x86_64.tar.gz /tmp/
RUN tar xzf /tmp/python-wasm-*-linux-x86_64.tar.gz -C /opt && \
    ln -s /opt/python-wasm-*/bin/python-wasm /usr/local/bin/python-wasm

ENTRYPOINT ["python-wasm"]
```

Published to GHCR: `ghcr.io/tegmentum/python-wasm:0.1.0`,
`ghcr.io/tegmentum/python-wasm:latest`.

Usage:

```bash
$ docker run -it ghcr.io/tegmentum/python-wasm -c "print(1+1)"
2
```

Ships wasmtime → fully self-contained.

### Exit criterion

`docker run ghcr.io/tegmentum/python-wasm:latest --version` works on
any Docker host.

## D₂ — `python-wasm` subcommands (the CLI surface)

Once the launcher exists, expose the orchestration commands as
subcommands rather than the user invoking shell scripts directly.

| Subcommand | Today | Tomorrow |
|---|---|---|
| `python-wasm -c "code"` | `./scripts/run-python.sh -c "code"` | direct |
| `python-wasm script.py` | `./scripts/run-python.sh script.py` | direct |
| `python-wasm rebuild` | `./scripts/python-wasm-rebuild.sh` | direct |
| `python-wasm extensions list` | (none) | walks site-packages, prints installed |
| `python-wasm extensions remove <name>` | (none) | drops from site-packages + invalidates compose |
| `python-wasm info` | (none) | versions, paths, active build digest |
| `python-wasm doctor` | (none) | sanity check the install |
| `python-wasm pip install <pkg>` | `pip install --target …` (manual) | wraps pip with the right --target |

The subcommand machinery lives in the same code regardless of which
channel the user came in through (tarball / brew / pip / docker).

## D₃ — Extension wheel install convention

Already wired by the MVP (Phase A+B+C). Documented under D for
discoverability:

- Pure-Python wheel: `python-wasm pip install requests` →
  `pip install --target ~/.python-wasm/site-packages requests` →
  `python-wasm -c "import requests"` works immediately.
- Extension wheel: `python-wasm pip install xxhash3-extra` →
  same `pip install --target` → `python-wasm rebuild` → import works.
- Wheel discovery: `python-wasm rebuild` globs
  `~/.python-wasm/site-packages/**/pyforge-pkg.toml`.

`python-wasm pip` is a thin wrapper around the host's `pip` (since
pip itself doesn't need to run inside python-wasm — the wheel is just
content that lands in our `site-packages`).

## Roadmap

```
D₁a tarball builder       ─ foundation; everything else depends on this
D₁b install.sh             ─ wraps tarball download
D₁c Homebrew formula       ─ points at tarball release
D₁d pip launcher           ─ embeds tarball contents
D₁e Docker image           ─ bundles tarball + wasmtime
                          ↓
D₂  python-wasm CLI         ─ subcommands in the launcher
D₃  extension wheel UX      ─ already MVP'd; just documented + the
                              `python-wasm pip install` wrapper
```

Plus external-platform admin (one-time):

- Register `python-wasm` on PyPI
- Create `tegmentum/homebrew-tap` repo
- Configure `python-wasm.dev` DNS (or use github.io)
- Set up GitHub Actions to build all release artifacts on tag push
- Set up GHCR for Docker

## Out of scope

- A Rust/Go launcher binary that vendors wasmtime as a library. Would
  remove the wasmtime-on-PATH dependency for the standalone channels.
  Real value (one-binary install) but substantial work; deferred until
  there's a concrete ask for it.
- Windows MSI. The tarball + install.sh stay POSIX; Windows users
  use pip or the eventual standalone binary.
- A `.deb` / `.rpm`. Linux users use the tarball, pip, or Homebrew on
  Linux. Distro packaging is a community-contribution track.

## See also

- [`pip-wheel-wrapper-plan.md`](pip-wheel-wrapper-plan.md) — D₂/D₃
  pre-conditions (MVP is shipped).
- [`pyforge-wheel-spec.md`](pyforge-wheel-spec.md) — extension wheel
  format the launcher's pip wrapper helps install.
- [`extension-recipe.md`](extension-recipe.md) — what
  `python-wasm rebuild` actually does.

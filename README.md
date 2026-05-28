# python-wasm

CPython compiled to WebAssembly (`wasm32-wasip2`) and run two ways:

- **Natively** as a CLI under [wasmtime](https://wasmtime.dev/).
- **In the browser**, by [jco](https://github.com/bytecodealliance/jco)-transpiling
  the component to JavaScript and supplying the WASI Preview 2 host with the
  [`@tegmentum/wasi-polyfill`](https://github.com/tegmentum/wasi-polyfill).

The repository builds CPython from source against the WASI SDK, then provides a
small Vite web demo that runs arbitrary Python in the browser with captured
stdout/stderr.

## Toolchain

| Component  | Version            |
| ---------- | ------------------ |
| CPython    | 3.14.3             |
| WASI SDK   | 33                 |
| wasmtime   | 45.0.0             |
| Host triple | `wasm32-wasip2`   |

The WASI SDK is fetched automatically. wasmtime must be on your `PATH`
(`brew install wasmtime` on macOS).

## Prerequisites

- **macOS on Apple Silicon (arm64).** `scripts/fetch-sdk.sh` downloads the
  `arm64-macos` WASI SDK build; adjust it for other platforms.
- A host **Python 3** interpreter (drives CPython's WASI cross-build).
- **wasmtime** ≥ 45 on `PATH` (for `make run` / `make test`).
- Standard build tools: `git`, `make`, a C compiler (Xcode CLT), `curl`, `tar`.
- For the web demo: **Node.js** + npm. The demo also depends on a local checkout
  of `@tegmentum/wasi-polyfill` at `../wasi-polyfill` (see `web/package.json`).

## Quick start (CLI)

```sh
make all                            # fetch SDK + CPython source, cross-build python.wasm
make python-composed                # compose with cap multiplexers -> python.composed.wasm
make run ARGS='-c "print(1+1)"'
make test                           # WASI CLI smoke tests
```

`make all` does two things:

1. `fetch-deps` — downloads the WASI SDK (`scripts/fetch-sdk.sh`) and clones
   CPython at the version named by the active profile, applying any
   per-version patches under `patches/<py-minor>/` (`scripts/fetch-cpython.sh`).
2. `build` — runs CPython's own `Tools/wasm/wasi build` to produce
   `deps/<profile-source-dir>/cross-build/wasm32-wasip2/python.wasm`.

### Build profiles

Multi-version, multi-variant builds are driven by `profiles/*.toml`. Each
profile names: a CPython version + source tree, the wasi-sdk version, the
static-vs-cap toggles (OpenSSL, zlib, v86), the cap artifact paths plugged
in at compose time, and the per-profile output dir. Default profile
(`profiles/default.toml`, symlinked to `3.14-current`) is CPython 3.14.3
with v86 enabled — the historical build behavior.

```sh
make python-composed                            # default profile -> build/3.14-current/
make python-composed PROFILE=3.13-current       # CPython 3.13.9 -> build/3.13-current/
make show-profile PROFILE=3.13-current          # dump resolved variables
```

See `docs/build-profiles.md` for the schema and adding new profiles.

Run Python directly:

```sh
make run ARGS='--version'
make run ARGS='/path/to/script.py'      # paths resolve inside the mounted CPython tree
make run PROFILE=3.13-current ARGS='--version'
```

`scripts/run-python.sh` invokes wasmtime with the CPython checkout (per profile)
mounted at `/` and `PYTHONPATH` pointed at the cross-build's stdlib.

## Web demo

```sh
make web-dev      # build stdlib + component, then start the Vite dev server
make web-build    # produce a production bundle in web/dist
make web-clean    # remove generated web artifacts
```

`make web-dev` runs three steps before launching Vite:

- `web-stdlib` (`scripts/bundle-stdlib.sh`) — tars the CPython stdlib into
  `web/public/stdlib.tar.gz` (test suites, idlelib, tkinter, etc. excluded).
- `web-transpile` (`scripts/transpile-component.sh`) — jco-transpiles
  `python.wasm` into `web/public/python-component/` (core wasm modules +
  `python.js` glue). The output directory is cleaned on each run so a changed
  module count across SDK versions never leaves orphaned modules behind.
- `web-deps` — `npm install` in `web/`.

## How it works

```
                    deps/cpython  ──(Tools/wasm/wasi build, WASI SDK 33)──▶  python.wasm
                                                                              (wasm32-wasip2 component)
        ┌───────────────────────────────────────────┬──────────────────────────────────┐
        │ CLI path                                   │ Browser path                      │
        ▼                                            ▼                                    
   wasmtime run                                  jco transpile  ──▶  python-component/    
   --dir cpython::/                                                  (core*.wasm + python.js)
   PYTHONPATH=<cross-build stdlib>                     │                                   
        │                                              ▼                                   
   native WASI P2 host                          @tegmentum/wasi-polyfill (WASI P2 in JS)  
                                                  • in-memory filesystem populated from   
                                                    stdlib.tar.gz                          
                                                  • custom stdio → captured stdout/stderr 
                                                  • sockets interfaces stubbed (unused)   
```

In the browser (`web/src/python-runner.ts`):

1. `initialize()` registers the polyfill's core plugins, fetches and untars
   `stdlib.tar.gz` into an in-memory map, and imports the transpiled `python.js`.
2. `runPython(code)` builds a WASI policy (args `python -c <code>`, an in-memory
   filesystem preopened at `/`, captured stdio), populates the filesystem with the
   stdlib on first run, instantiates the component, and calls `run.run()`.
3. The component declares the `wasi:sockets/*` interfaces but never uses them in
   the browser, so they are supplied as throwing stubs.

## Layout

```
Makefile                       build / run / test / web targets
patches/                       CPython build-tool patch (WASI SDK version + wasip2 target)
scripts/
  fetch-sdk.sh                 download + extract the WASI SDK
  fetch-cpython.sh             clone CPython at the pinned tag and apply patches
  run-python.sh                run python.wasm under wasmtime
  bundle-stdlib.sh             tar the stdlib for the browser
  transpile-component.sh       jco-transpile python.wasm for the browser
tests/
  test_wasi_cli.sh             CLI smoke tests
  smoke_test.py                in-guest assertions
web/
  src/                         Vite app: runner, stdlib loader, output capture, examples
  public/                      generated artifacts (gitignored): stdlib.tar.gz, python-component/
deps/                          downloaded SDK + CPython source + build output (gitignored)
```

## Notes

- The build reports "Could not build the ssl module" and a handful of "missing"
  modules — this is expected for WASI, which has no OpenSSL and no support for
  some POSIX facilities.
- `deps/` and the generated `web/public/` artifacts are gitignored; they are
  recreated by the `make` targets above.

# Wheel install

Phase 1 of [`coverage-implementation-plan.md`](coverage-implementation-plan.md).
Three things make `pip install` work in python-wasm today:

1. **pip is in the build** — CPython's stdlib ships `Lib/ensurepip/_bundled/pip-25.3-py3-none-any.whl`, and `scripts/run-python.sh` puts that wheel on `PYTHONPATH` so `python -m pip` imports without an `ensurepip` bootstrap subprocess.
2. **A writable mount** — `$PYTHON_WASM_HOME/site-packages` (default `~/.python-wasm/site-packages`) is `--dir`-mounted as `/site-packages` and added to `PYTHONPATH`. Wheels installed there persist across runs.
3. **A `sitecustomize.py`** that fills the WASI-shaped gaps pip and its vendored deps assume: `os.getuid`/`umask`, an `_ssl` alias for the cap-routed TLS module, and matching constants.

## Quickstart

```bash
# Default profile (CPython 3.14, cap-routed TLS + DNS + compression):
./scripts/run-python.sh -m pip install --use-deprecated=legacy-certs \
    --no-deps --target /site-packages --no-cache-dir requests

./scripts/run-python.sh -c "import requests; print(requests.get('https://example.com').status_code)"
# -> 200
```

A few flags every invocation needs today:

- `--use-deprecated=legacy-certs` — opts out of pip's `truststore` SSLContext (it expects native `ssl.SSLContext` subclassing semantics our cap-routed wrapper doesn't provide).
- `--target /site-packages` — pip's default install prefix doesn't match the writable mount; force the destination.
- `--no-deps` — multi-package install steps currently hang in `Installing collected packages` when there are 2+ wheels (under investigation). Install dependencies one at a time as a workaround.
- `--no-cache-dir` — pip's HTTP cache hits `os.stat`/`os.utime` calls we don't fully model; cheaper to disable.

## Writable site-packages — host layout

```
~/.python-wasm/                ← $PYTHON_WASM_HOME
└── site-packages/             ← mounted to /site-packages in the guest
    ├── requests/
    ├── requests-2.34.2.dist-info/
    ├── urllib3/
    ├── certifi/
    └── …
```

Override the location with `PYTHON_WASM_HOME=/path env ./scripts/run-python.sh ...`. The script creates the directory if missing.

## Two integration paths

### Path A — pip (recommended today)

`python -m pip install --target /site-packages <pkg>` against PyPI works for pure-Python wheels. This is the path the quickstart above uses. Confirmed working with:

- `certifi` (134 KB)
- `idna` (65 KB)
- `urllib3` (131 KB)
- `charset_normalizer` (61 KB)
- `requests` (73 KB)

End-to-end: `requests.get('https://example.com')` returns 200 with the full body.

### Path B — uv-wasm (via WasmMachine)

`uv-wasm` (at `~/git/uv-wasm/`) is a wasip2 component that compiles uv to WASM. It runs under WasmMachine, which provides the `wasmmachine:command/exec` import so uv can shell out to `python` and `git`. Three deployable variants:

- `dist/uv.wasm` — WIT-exec backend. Needs WasmMachine for command/exec.
- `dist/uv-ipc.wasm` — IPC backend. Runs on WasmMachine via the `/run/wasm/ipc` mailbox.
- `dist/uv-dev.wasm` — wac-plugged with a dev mock; runs under plain wasmtime via `scripts/uv-dev.sh`.

For wheel resolution + dependency graph (the things uv is fast at), Path B is the better choice once integrated into a WasmMachine-managed environment. python-wasm doesn't embed uv-wasm — uv runs as its own component and writes its install layout to a path python-wasm's `sys.path` picks up via the same `$PYTHON_WASM_HOME/site-packages` mount. Phase 6 of the coverage plan covers full integration; today the recommended path is pip.

### Path NOT taken — embed uv-core via WIT

Possible but discouraged. uv-core is a large reactor component and embedding it duplicates what the WasmMachine command boundary already gives us. Path A (pip) or Path B (uv as separate component) both leave the python-wasm artifact smaller and the dependency graph cleaner.

## Known issues

| Issue | Workaround | Tracking |
|---|---|---|
| Multi-package install step hangs in `Installing collected packages` | install one wheel at a time | Phase 1 polish |
| `OSError(8, 'Bad file descriptor')` on every download (retried successfully) | tolerated by pip's retry logic; openssl-component closes the connection prematurely | Phase 2 (asyncio + TLS) |
| `--use-deprecated=legacy-certs` required | pip ships truststore which expects native SSLContext subclassing; our wrapper doesn't satisfy `super().verify_mode.__set__` | Phase 8 — openssl-component v0.2.x |
| `getpeercert(binary_form=False)` returns synthetic dict | openssl-component validates hostname during handshake, so the synthetic SAN matches | Phase 8 — openssl-component v0.2.x |
| `asyncio.run()` fails at `socket.socketpair()` | use sync HTTP for now | Phase 2 |

## What the sitecustomize.py does

`cpython-ext/_posix_user_shim/sitecustomize.py` is copied to `Lib/sitecustomize.py` by `make install-python-shims`. CPython runs it at interpreter startup via `site.py`. It:

- Adds `os.getuid`/`os.geteuid`/`os.getgid`/`os.getegid`/`os.getppid`/`os.getlogin` returning container-style identity (uid/gid 0, ppid 1, login "wasi"). Needed because `Lib/os.py` is frozen into `python.wasm` and our `posix` C ext doesn't export these. pip's bundled `platformdirs` imports `getuid` unconditionally.
- Adds `os.umask()` as a stub. pip uses it to mask wheel-install file modes.
- Aliases `_ssl` to `_ssl_capability` in `sys.modules`. pip's `has_tls()` does `import _ssl` to detect TLS support; we ship the cap-routed name.

## What was needed for `pip install requests`

Worth recording so future polish work isn't surprised:

| Fix | Where | Why |
|---|---|---|
| Z_BUF_ERROR retry loop in zlib-wasm streaming decompress | `~/git/zlib-wasm/src/exports.c` | zipimport of pip wheel modules >64 KiB failed; one-shot decompress treated Z_BUF_ERROR as fatal instead of "output buffer full, retry" |
| `os.getuid`, `os.umask`, `_ssl` alias | `cpython-ext/_posix_user_shim/sitecustomize.py` | pip's vendored `platformdirs`/`urllib3` imports |
| `load_verify_locations(cafile=path)` reads path itself | `cpython-ext/_ssl/ssl_capability.py` | pip passes the certifi bundle as a file path; the wasi mount lets us open it |
| `getpeercert(binary_form=False)` returns synthetic SAN dict | same | urllib3's hostname check needs `subjectAltName`; openssl-component already validated against `server_hostname` |
| `OPENSSL_VERSION_INFO` is a real tuple | same | urllib3 compares as `info < (1, 1, 1)`; the C ext returned a string |
| Module-level `OP_NO_*` / `VERIFY_X509_*` constants | `cpython-ext/_ssl/ssl.py` | urllib3 imports them at module level, not via `Options`/`VerifyFlags` enums |
| `SSLSocket.settimeout`/`gettimeout`/`fileno`/`setblocking` no-ops | ssl_capability.py | urllib3 calls these on the socket-shaped wrapper |
| `SSLContext.verify_flags` attribute | ssl_capability.py | urllib3 sets `verify_flags |= ...` on the context |
| `SSLSocket.read` drain only on `pending()>0` | ssl_capability.py | the old eager drain blocked urllib3 keepalive reads |

Each of these is a small targeted fix. The cumulative result: the standard pure-Python web stack (requests/urllib3/certifi/idna/charset_normalizer) installs and runs.

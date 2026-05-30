# Wheel install

Phase 1 of [`coverage-implementation-plan.md`](coverage-implementation-plan.md).
Three things make `pip install` work in python-wasm today:

1. **pip is in the build** — CPython's stdlib ships `Lib/ensurepip/_bundled/pip-25.3-py3-none-any.whl`, and `scripts/run-python.sh` puts that wheel on `PYTHONPATH` so `python -m pip` imports without an `ensurepip` bootstrap subprocess.
2. **A writable mount** — `$PYTHON_WASM_HOME/site-packages` (default `~/.python-wasm/site-packages`) is `--dir`-mounted as `/site-packages` and added to `PYTHONPATH`. Wheels installed there persist across runs.
3. **A `sitecustomize.py`** that fills the WASI-shaped gaps pip and its vendored deps assume: `os.getuid`/`umask`, an `_ssl` alias for the cap-routed TLS module, and matching constants.

## Quickstart

```bash
# Default profile (CPython 3.14, cap-routed TLS + DNS + compression):
./scripts/run-python.sh -m pip install \
    --no-deps --target /site-packages --no-cache-dir requests

./scripts/run-python.sh -c "import requests; print(requests.get('https://example.com').status_code)"
# -> 200
```

A few flags every invocation needs today:

- ~~`--use-deprecated=legacy-certs`~~ — no longer required (2026-05-29): truststore subclassing now works.
- `--target /site-packages` — pip's default install prefix doesn't match the writable mount; force the destination.
- `--no-deps` — multi-package install steps currently hang in `Installing collected packages` when there are 2+ wheels (under investigation). Install dependencies one at a time as a workaround.
- `--no-cache-dir` — pip's HTTP cache hits `os.stat`/`os.utime` calls we don't fully model; cheaper to disable.

For wheel paths that shell out (sdist builds, post-install scripts), use
`./scripts/run-python-with-subprocess.sh -m pip ...` instead of
`./scripts/run-python.sh -m pip ...` — the former wires v86-posix-host
so `subprocess.run` actually fork-execs. See Phase 5 in
[`coverage-implementation-plan.md`](coverage-implementation-plan.md).

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
| ~~Multi-package install step hangs in `Installing collected packages`~~ | ✅ **fixed 2026-05-29** — pip's rich progress thread looped in our inline threading shim; Thread.start() now defers subclass-override `run()` until `join()` | done |
| `OSError(8, 'Bad file descriptor')` on every download (retried successfully) | tolerated by pip's retry logic; openssl-component closes the connection prematurely after the response, before urllib3 expects a clean keepalive | Phase 8 — needs openssl-component v0.2.x non-blocking peek to safely drain TLS records before close-notify |
| ~~`--use-deprecated=legacy-certs` required~~ | ✅ **fixed 2026-05-29** — moved verify_mode/options/verify_flags/max+min_version into an `_SSLContextDescriptors` base class so truststore's `super().<name>.__set__()` lookup walks the MRO into the base and finds them. Also added `SSLSocket.get_unverified_chain()`, `.context` back-ref, and real `set_default_verify_paths()`. | done |
| `getpeercert(binary_form=False)` returns synthetic dict | openssl-component validates hostname during handshake, so the synthetic SAN matches | Phase 8 — openssl-component v0.2.x |
| `asyncio.run()` works ✅ | self-pipe stubbed via sitecustomize (Phase 2) | done |
| ~~`httpx`/`httpcore` async HTTP fails on `select.poll(socket)`~~ | ✅ **httpx sync fixed 2026-05-29** via openssl-component@0.2.x socket-fd + sitecustomize patch of `httpcore.is_socket_readable`. ✅ **asyncio.to_thread trap fixed 2026-05-29** (`run_in_executor` patched). ✅ **httpx async fixed 2026-05-29** — added `mem-bio-client` to openssl-component and wired `SSLContext.wrap_bio` + `SSLObject` Python shim. End-to-end: `httpx.AsyncClient().get('https://example.com')` returns 200. | done |
| `aiohttp` won't install (sdist-only, C exts) | use `requests` for HTTP today | Phase 4 (C-ext wheel pipeline) |
| ~~`urllib` chunked + Connection:close raises IncompleteRead~~ | ✅ **fixed 2026-05-29** — root cause was `SSLSocket.close()` synchronously tearing down the openssl-component client when http.client called `self.sock.close()` after parsing headers (stdlib's close is just a ref decrement). Made our `close()` a no-op and rely on inner dealloc. Also patched `urllib3.util.wait.wait_for_read` to consult `cap.socket_readable()` instead of `select.poll(SSL fd)` which EBADFs across the component boundary. urllib + urllib3 + requests now download files of any size end-to-end. | done |
| ~~`tarfile.open(fileobj=BytesIO(sdist), mode='r:gz')` raises CRC check failed~~ | ✅ **fixed 2026-05-29** — `_Decompress.eof` returned True as soon as the one-shot capability finished the deflate stream, even when 332 KB of decompressed bytes were still buffered in `_unconsumed` because `max_length` truncated the first read. gzip then jumped straight to `_read_eof()` and compared the file footer's CRC against a running CRC covering only the first 16 KB. Gate eof on `_unconsumed` being drained too. sdist tar extraction now works. | done |

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

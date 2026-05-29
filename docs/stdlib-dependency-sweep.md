# python-wasm stdlib dependency sweep

Audit of CPython's stdlib in the default python-wasm build: what works,
what's cap-routed, what's gapped, and which gaps are candidates for closure.

**Snapshot.** 2026-05-28, refreshed for Phase 0 of [`coverage-implementation-plan.md`](coverage-implementation-plan.md).

**Build under test.** `build/3.14-current/python.composed.wasm` — CPython 3.14.3,
WASI Preview 2 component, post-multiplexer-retirement per-codec caps (commits
through `c2fc788`).

**How to reproduce.**

- Import sweep: see `scripts/test-imports.sh` (probes the 205-module list below).
- Functional probes: `scripts/test-functional-sweep.sh`.
- Network paths: `NETWORK=1 scripts/test-dns-resolution.sh`, `NETWORK=1 scripts/test-ssl-network.sh`.

## Summary

- **169 of 205** top-level stdlib modules import (82%). The remaining 36 split into:
  - **22 removed upstream in 3.13/3.14** (PEP 594): `aifc`, `asynchat`, `asyncore`, `cgi`, `cgitb`, `chunk`, `crypt`, `imghdr`, `mailcap`, `msilib`, `nis`, `nntplib`, `ossaudiodev`, `parser`, `smtpd`, `sndhdr`, `spwd`, `sunau`, `symbol`, `telnetlib`, `uu`, `xdrlib`. Not a python-wasm gap; gone from CPython itself.
  - **9 platform-foreign or terminal/GUI** (intrinsic): `curses`, `tkinter`, `turtle`, `pty`, `tty`, `termios`, `readline`, `winreg`, `winsound`. No wasm path.
  - **5 POSIX system-level** (intrinsic in pure wasip2): `fcntl`, `grp`, `pwd`, `resource`, `syslog`. Some of these may grow caps later (`fcntl.flock` is plausible; `pwd`/`grp` have no analog).
- **Functional coverage** of common paths: ~85% — cap-routed cryptography, compression, TLS, sqlite, tzdata, and pbkdf2 all work end-to-end. Known gaps: `asyncio.run()` self-pipe (new finding, see C.2), `subprocess.Popen` (waits on v86), `scrypt` (cap unwired in stdlib).

## Coverage by category

### A. Cap-routed via wasm capability components ✅

Pattern A static-linked cpython-ext extensions + their `Lib/` shims:

| Stdlib module | cpython-ext | Capability | Status |
|---|---|---|---|
| `zlib`              | `_zlib_cap`            | `zlib:compression@0.1.0`             | ✅ per-codec (Phase A) |
| `bz2`               | `_bzip2_cap`           | `bzip2:compression@0.1.0`            | ✅ per-codec (Phase B) |
| `lzma`              | `_lzma_cap`            | `lzma:compression@0.1.0`             | ✅ per-codec (Phase C) |
| `compression.zstd`  | `_zstd_cap`            | `zstd:compression@0.1.0`             | ✅ full advanced API (Phase D + fidelity) |
| `gzip`              | (uses `_zlib_cap`)     | (same)                                | ✅ |
| `hashlib` (14 algos + KDFs) | `_crypto_hash` | `tegmentum:crypto-hash-multiplexer/hash-dispatcher` | ✅ + blake2 params, SHA-3 224/384, SHAKE, digest_size, block_size |
| `ssl`               | `_ssl_capability`      | `openssl:component/{tls,x509,pkey,error,random}` | ✅ Tier 1+2 + WebPKI |
| `sqlite3`           | `_sqlite_capability`   | `sqlite:wasm/high-level`             | ✅ |
| `subprocess` (v86 only) | `_v86_posix`       | `v86:posix/process`                  | ⚠ stub returns `GuestNotReady` |
| (non-stdlib) `_xxhash` | `_xxhash`           | `tegmentum:hashing-multiplexer/hashing-dispatcher` | ✅ blake3 keyed/derive_key/xof, xxh3 with-secret |
| (non-stdlib) `_password_hash` | (via `_crypto_hash`) | `tegmentum:password-hash-multiplexer` | ✅ pbkdf2 / scrypt / argon2id explicit-cost variants |

Notes:

- The compression-multiplexer is **retired from this build** as of the Phase A–G work (commit `c2fc788`). Each codec routes directly through its own WIT contract. The multiplexer remains shipped from `~/git/compression-multiplexer/` for use by other consumers.
- `_xxhash` is not exposed as a top-level `import xxhash`; the cpython-ext name is the actual entry. See `cpython-ext/_xxhash/`.

### B. Static-linked + working ✅

~85 C extensions baked into `python.wasm` directly:

```
_abc _ast _asyncio _bisect _blake2 _codecs (+ _codecs_cn/_codecs_hk/_codecs_iso2022/_codecs_jp/_codecs_kr/_codecs_tw)
_collections _contextvars _csv _datetime _decimal _elementtree
_functools _heapq _hmac _imp _io _json _locale _lsprof _md5
_multibytecodec _opcode _operator _pickle _queue _random _sha1 _sha2
_sha3 _signal _socket _sre _stat _statistics _string _struct
_symtable _thread _tokenize _tracemalloc _types _typing _warnings
_weakref _xxtestfuzz _zoneinfo array atexit binascii builtins cmath
errno faulthandler gc itertools marshal math posix pyexpat select
sys time unicodedata xxsubtype
```

Pure-Python stdlib modules that depend on these (json, asyncio module
imports, datetime, decimal, struct, …) work for their core paths.

### C. Importable but partially broken ⚠

Modules that load but where specific operations fail:

| Module | What works | What's broken | Reason |
|---|---|---|---|
| `threading` | imports, locks, condition vars, RLock, **`Thread.start()` runs inline** | actual parallelism | `Lib/threading.py` shim runs targets inline (no preemption); is_alive() correctly returns False after target completes |
| `asyncio` | imports, `asyncio.run`, `gather`, `sleep`, `create_task`, `cancel`, `Queue`, full single-task event loop (fixed in Phase 2 2026-05-29) | `to_thread` (needs real threading), HTTP-async via `httpx`/`aiohttp` (needs Phase 8 TLS surface + Phase 4 C-ext build) | Self-pipe stubbed via sitecustomize (no signals/threads to wake the loop in wasi-p2 anyway) |
| `os` | most stat/file/env operations | `os.fork`, `os.execvp`, `os.popen` | no process model in pure wasm |
| `socket` | TCP via `wasi:sockets/tcp`, **DNS via `wasi:sockets/ip-name-lookup`** | `socketpair()` (see asyncio above), raw sockets | wasi-p2 has no socketpair primitive; fallback path doesn't work in wasmtime today |
| `subprocess` | full `run`/`Popen`/`check_call`/`check_output`/signals/stdin/stdout/stderr capture/parallel spawns when running under `scripts/run-python-with-subprocess.sh` (Phase 5 done 2026-05-29) — default stub still returns `GuestNotReady` | (nothing — full surface) | composes with `v86-posix-host` instead of stub; helper polls a shared mailbox dir |
| `hashlib` | all 14 algorithms + pbkdf2_hmac + blake2 params + SHA-3 + SHAKE | `scrypt` (cap impl shipped but not wired in stdlib `hashlib.scrypt`) | gap is purely in `Lib/_hashlib.py` shim wiring; cap supplies it |
| `ssl` | TLS handshake, cert validation, urllib.urlopen, MemoryBIO | SSLObject, SSLSession, `get_server_certificate`, DER_cert_to_PEM_cert, RAND_add, RAND_status | deferred to openssl-component v0.2.x |
| `multiprocessing` | imports, **`Pool.map`/`apply`/`imap` via OffloadPool** when `OFFLOAD_POOL_DIR` is set (Phase 6 done 2026-05-29; real parallelism across N host workers), Process object | `Process.start()` (needs fork), Pool without offload backend wired | sitecustomize hijacks `multiprocessing.Pool` when env is set |

### D. Fail to import entirely ❌

```
ctypes (no _ctypes)        curses (no _curses)       tkinter (no _tkinter)
turtle (depends on tkinter) pty (no termios)         tty (no termios)
fcntl, grp, pwd, resource, readline, syslog, termios, winreg, winsound
```

And the 22 PEP 594 removals listed in Summary.

| Missing C ext | Used by | Cap candidate? |
|---|---|---|
| `_ctypes` (libffi) | ctypes | Yes — would be a libffi-wasm build; large effort, big ecosystem unlock |
| `mmap` | `mmap` module | Pure-Python `Lib/mmap.py` shim possible (memory: in-RAM `bytearray` backing); see [`wasm-cap-vs-shim-decision`](../../../../.claude/projects/-Users-zacharywhitley-git-python-wasm/memory/wasm-cap-vs-shim-decision.md) |
| `fcntl` | locking, file flags | Partial via `wasi:filesystem/types`; most flags no-op |
| `grp`, `pwd` | user lookup | No analog in wasm |
| `_posixsubprocess` | subprocess fast-path | Routed via `_v86_posix` |

## What's worth advertising

- Full `json`, `xml.etree`, `csv`, `pickle` (with `_pickle` C accelerator)
- Full `sqlite3` via `_sqlite_cap` + sqlite:wasm capability
- Full `decimal` with `_decimal` C accelerator
- `urllib` + `http.client` + `smtplib` + `ftplib` + `imaplib` + `poplib` over `_socket` + `_ssl_capability`
- `email`, `html` parsing
- `importlib`, `zipimport`, `zipfile` — wheel loading works
- `compileall`, `py_compile` — bytecode tooling
- `inspect`, `dis`, `ast`, `opcode` — full introspection
- `zoneinfo` with 598 IANA timezones (bundled tzdata 2026.2)
- `hashlib` with all 14 algorithms, pbkdf2_hmac, full blake2 (key/salt/person/digest_size)

## Test snapshot

Against `build/3.14-current/python.composed.wasm` post-`c2fc788`:

| Probe | Result |
|---|---|
| `hashlib.sha256(b'x').hexdigest()` starts `2d711642` | ✅ |
| `hashlib.sha3_224(b'x').hexdigest()` = `63e6ceb2…` | ✅ |
| `hashlib.sha3_384(b'x').hexdigest()` = `5abfc7bc…` | ✅ |
| `hashlib.shake_128(b'x').hexdigest(32)` len = 64 | ✅ |
| `hashlib.shake_256(b'x').hexdigest(64)` len = 128 | ✅ |
| `hashlib.blake2b(b'x').hexdigest()` = `0909377a…` | ✅ |
| `hashlib.blake2b(b'x', salt=b'salt', person=b'person')` | ✅ |
| `hashlib.pbkdf2_hmac('sha256', b'pw', b'salt', 1000, 32)` | ✅ |
| `hashlib.scrypt(...)` | ❌ `AttributeError` (cap shipped; stdlib shim unwired) |
| `zlib / bz2 / lzma / gzip / compression.zstd` roundtrip | ✅ all five |
| `compression.zstd.train_dict(samples, 1024)` | ✅ |
| `sqlite3.connect(':memory:').execute('SELECT 1').fetchone()` | ✅ `(1,)` |
| `ssl.OPENSSL_VERSION.startswith('OpenSSL')` | ✅ |
| `urllib.request.urlopen('https://example.com')` (NETWORK=1) | ✅ via `scripts/test-ssl-network.sh` |
| `socket.getaddrinfo('pypi.org', 443)` (NETWORK=1) | ✅ 8 entries via `scripts/test-dns-resolution.sh` |
| `zoneinfo.ZoneInfo('America/New_York')` | ✅ |
| `threading.Lock` / `acquire` / `release` | ✅ |
| `threading.Thread(target=...).start()` | ✅ runs inline (no parallelism) |
| `asyncio.run(coro)` | ✅ via Phase 2 sitecustomize stub of `_make_self_pipe` |
| `asyncio.gather(*tasks)` | ✅ concurrent task scheduling works |
| `os.fork()` | ❌ no fork |
| `subprocess.run(...)` (default build, stub component) | ❌ `GuestNotReady` (fail-fast — opt in via run-python-with-subprocess.sh) |
| `subprocess.run(...)` via `run-python-with-subprocess.sh` | ✅ full Phase-3c surface — see v86's test-v86-posix-roundtrip.sh, 35+ assertions pass |

## Known gaps, by track

### Resolved this session (no further work)

- DNS resolution end-to-end via `wasi:sockets/ip-name-lookup` (CLI: requires `-S allow-ip-name-lookup`; browser: DoH default per `python-runner.ts`).
- All compression codecs reachable via per-codec caps (multiplexer retired).
- blake2 full parametric API (`key`, `salt`, `person`, `digest_size`).
- SHA-3 224/384, SHAKE 128/256, `digest_size`/`block_size` on hash objects.
- zstd advanced: `train_dict`, `finalize_dict`, `compress_advanced`, `compress_advanced_with_dict`.
- pbkdf2_hmac.

### Active gaps (have an owner)

| Gap | Track |
|---|---|
| `asyncio.run()` socketpair fallback fails | Phase 2 (asyncio + TLS battle-test) — investigate whether to (a) implement a wasi socketpair shim, (b) replace asyncio's self-pipe with a `wasi:io/poll`-native pollable, or (c) ship a `Lib/_socket_socketpair.py` shim using two ends of a memfd-style polyfill |
| `hashlib.scrypt` unwired | Phase 1 polish — wire `Lib/_hashlib.py` to `_crypto_hash.scrypt` (cap impl already shipped) |
| `subprocess.Popen.spawn()` | Phase 5 (v86 subprocess) — upstream-driven |
| `ssl.SSLObject`, `ssl.get_server_certificate`, RAND_add/status | Phase 8-ish — needs openssl-component v0.2.x with `SSL_get_peer_certificate`/`i2d_X509` |
| `mmap` | Tier 3 — `Lib/mmap.py` pure-Python shim viable |

### Intrinsic — won't be filled

- `tkinter`, `turtle`, `_tkinter` (GUI)
- `curses`, `readline`, `termios`, `pty`, `tty` (terminal)
- `os.fork`, `multiprocessing.Process.start` (process model — `tegmentum:py-offload` is the replacement path)
- `grp`, `pwd`, `nis` (user/group lookup)
- `winreg`, `winsound`, `msilib` (Windows-only)
- 22 PEP 594 removals — gone upstream

## Test scripts (Phase 0 + Phase 1 deliverables)

| Script | What it probes |
|---|---|
| `scripts/test-dns-resolution.sh` | `socket.getaddrinfo` / `gethostbyname` for `pypi.org`, `files.pythonhosted.org`, `example.com` |
| `scripts/test-ssl-network.sh` | TLS handshake + HTTPS roundtrip to `example.com:443` (pre-existing) |
| `scripts/test-wheel-smoke.sh` | install + import probe over the top-20 pure-Python wheels (Phase 1) |
| `scripts/uv-dev.sh` | wraps `~/git/uv-wasm/dist/uv-dev.wasm` with the wasmtime flags it needs |
| `scripts/run-python.sh` | standard runner — composed wasm + mounted writable `/site-packages` + DNS + TLS flags |

## Phase 1 wheel-install smoke table

Against `build/3.14-current/python.composed.wasm` via `scripts/run-python.sh`,
following `docs/wheel-install.md` Path A (pip with `--use-deprecated=legacy-certs`,
`--no-deps`, `--target /site-packages`). **17 of 20 pass.**

| Package | Install | Import | Notes |
|---|---|---|---|
| certifi             | ✅ | ✅ | |
| idna                | ✅ | ✅ | |
| urllib3             | ✅ | ✅ | needs SSL OPENSSL_VERSION_INFO tuple + module-level OP_NO_* constants |
| charset_normalizer  | ✅ | ✅ | |
| requests            | ✅ | ✅ | `requests.get('https://example.com')` returns 200 |
| six                 | ✅ | ✅ | |
| python-dateutil     | ✅ | ✅ | (imports as `dateutil`) |
| click               | ✅ | ✅ | |
| jinja2              | ✅ | ❌ | missing `markupsafe` dep — sdist-only on PyPI for 3.14 |
| markupsafe          | ❌ | —  | sdist install fails on gzip CRC (early-close — Phase 2) |
| pyyaml              | ❌ | —  | sdist install fails (same root cause) |
| attrs               | ✅ | ✅ | (imports as `attr`) |
| packaging           | ✅ | ✅ | |
| typing_extensions   | ✅ | ✅ | |
| wheel               | ✅ | ✅ | |
| toml                | ✅ | ✅ | |
| tomli               | ✅ | ✅ | |
| rich                | ✅ | ✅ | |
| pygments            | ✅ | ✅ | |
| colorama            | ✅ | ✅ | |

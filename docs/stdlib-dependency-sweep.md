# python-wasm stdlib dependency sweep

Audit of CPython's stdlib in the default python-wasm build (Phase 5.2.1):
what works, what's cap-routed, what's gapped, and which gaps are good
candidates for closure via new wasm capability components.

Snapshot: 2026-05-28. Verified against `build/python.composed.wasm`
(browser variant, no static OpenSSL, hashlib + ssl now cap-default).

## Summary

- **163 of 169** top-level stdlib modules import successfully (~96%).
- **6 fail import entirely**: ctypes, curses, tkinter, pty, tty, turtle.
- **Functional coverage** of common operations: ~70% — what's importable
  is mostly usable, with specific gaps (no threading, no fork, partial
  hashlib/ssl APIs, no DNS, no tzdata).

## Coverage by category

### A. Cap-routed (shipped via wasm capability) ✅

Pattern A static-linkage extensions + their `Lib/` shims:

| Stdlib module | cpython-ext | Capability | Status |
|---|---|---|---|
| `zlib` | `_compression` | `tegmentum:compression-multiplexer/compression-dispatcher` | ✅ |
| `bz2` | `_compression` | same | ✅ |
| `lzma` | `_compression` | same | ✅ |
| `compression.zstd` | `_compression` | + `zstd-extras` | ✅ |
| `hashlib` (9 algos) | `_crypto_hash` | `tegmentum:crypto-hash-multiplexer/hash-dispatcher` | ✅ Phase 5.1 |
| `ssl` (Tier-1+2 surface) | `_ssl` | `openssl:component/{tls,x509,pkey,error,random}` | ✅ Phase 5.2 |
| `sqlite3` | `_sqlite_capability` | `sqlite:wasm/high-level` | ✅ |
| `subprocess` (v86 only) | `_v86_posix` | `v86:posix/process` | ✅ |
| `xxhash` (new, non-stdlib) | `_xxhash` | `tegmentum:hashing-multiplexer/hashing-dispatcher` | ✅ |

### B. Static-linked + working ✅

85 C extensions baked into `python.wasm` directly. The big ones:

  _abc _ast _asyncio _bisect _blake2 _codecs (+ _codecs_cn/_codecs_hk/…)
  _collections _contextvars _csv _datetime _decimal _elementtree
  _functools _heapq _hmac _imp _io _json _locale _lsprof _md5
  _multibytecodec _opcode _operator _pickle _queue _random _sha1 _sha2
  _sha3 _signal _socket _sre _stat _statistics _string _struct
  _symtable _thread _tokenize _tracemalloc _types _typing _warnings
  _weakref _xxtestfuzz _zoneinfo array atexit binascii builtins cmath
  errno faulthandler gc itertools marshal math posix pyexpat select
  sys time unicodedata xxsubtype

These work as expected — pure-Python stdlib modules that depend on
these (json, asyncio, datetime, decimal, struct, …) work.

### C. Importable but partially broken ⚠

Modules that load but where specific operations fail:

| Module | What works | What's broken | Reason |
|---|---|---|---|
| `threading` | imports, locks, condition vars, RLock | `Thread.start()` → `RuntimeError: can't start new thread` | wasi-p2 has no preemptive threads |
| `os` | most stat/file/env operations | `os.fork`, `os.execvp`, `os.popen` | no process model in pure wasm |
| `socket` | TCP via `wasi:sockets/tcp` + **DNS via `wasi:sockets/ip-name-lookup`** (resolved 2026-05-28) | nothing in default scope | needs runtime grant: `-S inherit-network -S allow-ip-name-lookup` |
| `subprocess` | imports + Popen object | `Popen.spawn()` requires v86 backend ready | `v86-posix-stub` returns `GuestNotReady` until v86 grows real spawn |
| `hashlib` | **all 14 algorithms + pbkdf2_hmac** (post Phase 5.1 redesign 2026-05-28) | scrypt | scrypt impossible in pure Python at production parameters; needs cap |
| `ssl` | TLS handshake, cert validation, urllib.urlopen, MemoryBIO | SSLObject, SSLSession, get_server_certificate, DER_cert_to_PEM_cert, RAND_add, RAND_status | deferred to openssl-component v1.1 |
| `gzip` | **full roundtrip + zipfile + tarfile.tar.gz** (fixed 2026-05-28) | nothing | zlib shim _ZlibDecompressor + unused_data tracking shipped |
| `zoneinfo` | **598 IANA timezones via PyPI tzdata fallback** (fixed 2026-05-28) | nothing | scripts/fetch-tzdata.sh wires into make fetch-deps |
| `multiprocessing` | imports + Process object | `Process.start()` fails | same as `os.fork` — no process model |

### D. Fail to import entirely ❌

| Module | Underlying C ext missing | Fixability |
|---|---|---|
| `ctypes` | `_ctypes` (libffi) | Possible via libffi-wasm; large effort |
| `curses` | `_curses` | Intrinsic — no terminal in wasm sandbox |
| `tkinter` | `_tkinter` | Intrinsic — no GUI |
| `turtle` | depends on tkinter | Same |
| `pty`, `tty` | `termios` | Intrinsic — no tty |

And the per-platform C extensions absent from the build:

| Missing C ext | Used by | Cap candidate? |
|---|---|---|
| `mmap` | `mmap` module | Yes — `wasi:io/memory` (no spec yet) or a custom cap |
| `fcntl` | locking, file flags | Some via wasi-p2 `wasi:filesystem/types`, most no-ops in wasm |
| `grp`, `pwd` | user lookup | No — no user/group concept in pure wasm |
| `readline` | interactive REPL | No — no tty |
| `resource` | rlimit, getrusage | Maybe — wasi-p2 doesn't model resource limits |
| `syslog` | logging to syslog | Maybe — would need a logging cap |
| `nis` | NIS/YP | No — obsolete |
| `_posixsubprocess` | subprocess fast-path | Routed via `_v86_posix` instead |

## Gap candidates worth a wasm cap

In rough priority order — most leverage / least effort first.

### High-priority — all RESOLVED 2026-05-28

1. **DNS resolution** — ✅ **Was never broken.** Original sweep ran without
   `-S inherit-network -S allow-ip-name-lookup` flags. With them,
   `socket.getaddrinfo()`, `socket.create_connection()`, and
   `socket.gethostbyname()` all work against example.com, www.python.org,
   github.com. Wasmtime auto-wires `wasi:sockets/ip-name-lookup` to host
   DNS when the flag is granted.

2. **tzdata bundle** — ✅ **Shipped** (commit `73d0a03`).
   `scripts/fetch-tzdata.sh` pulls tzdata 2026.2 from PyPI, stages it
   at `deps/cpython/Lib/tzdata/`. 598 IANA timezones available. Wired
   into `make fetch-deps`.

3. **gzip.decompress bug** — ✅ **Shipped** (commit `5dd6788`).
   `Lib/zlib.py` shim now has `_ZlibDecompressor` + proper `unused_data`
   tracking. `gzip.decompress`, multi-member gzip, zipfile ZIP_DEFLATED,
   and tarfile.tar.gz all roundtrip correctly.

4. **pbkdf2_hmac, scrypt** — ✅ **pbkdf2_hmac shipped** (commit `738ae9b`).
   Pure-Python PBKDF2 (RFC 8018) in `Lib/_hashlib.py`. RFC 6070 vectors
   match; 4096-iteration HMAC-SHA1 takes 13ms. `scrypt` deliberately
   omitted — pure-Python scrypt at production parameters (N=16384) is
   too slow to be useful; tracked for a future `tegmentum:kdf` cap.

### Medium-priority

5. **sha224, sha3_224, sha3_384** — ✅ **Resolved by Phase 5.1 redesign**.
   These are already available via CPython's BUILTIN `_sha2`/`_sha3`
   C extensions (statically linked into python.wasm). Stdlib hashlib
   uses them natively when `_hashlib` doesn't provide them. The
   `crypto-hash-multiplexer` extension stays for component-to-component
   cap-routed hashing but Python users go through the faster builtins.

6. **shake_128, shake_256 (XOFs)** — ✅ **Same as above** — covered by
   builtin `_sha3.shake_128`/`shake_256`.

7. **ssl.get_server_certificate** — ⏸ **Blocked on cap revision.** The
   `_ssl_capability` C ext exposes no peer-cert getter; openssl-component
   WIT 0.1.0 doesn't expose `SSL_get_peer_certificate` / `i2d_X509`.
   Pure-Python implementation isn't possible (no way to extract cert
   bytes without bypassing the TLS stack). Tracked as openssl-component
   v0.2.x. Stub raises NotImplementedError with this context.

8. **mmap** — ⏸ **Deferred.** `wasi:io/memory` doesn't exist; would need
   a new `tegmentum:memory/anonymous-pages` cap or use wasi-p2 filesystem
   APIs as backing. Most Python code paths around mmap have `io.BytesIO`
   fallbacks. Medium-utility; not blocking common use.

### Low-priority / experimental

9. **ctypes via libffi-wasm** — would unlock a huge swath of binding-
   based packages (cffi, pycparser-using libs). Substantial effort
   to build libffi for wasm32-wasip2 + wire the trampolines.
   **Effort:** 2–4 weeks.

10. **threading.Thread** — wasi-p2 has no preemptive threads. Would
    require either wasi-threads-proposal landing OR a girder-style
    actor model (separate WASM instances per "thread"; shared-nothing).
    Substantial architectural decision; tracked in
    `docs/native-execution-and-parallelism.md` §5. **Effort:** months
    if pursued.

### Intrinsic — won't be filled

- `tkinter`, `turtle`, `_tkinter` (GUI)
- `curses`, `readline`, `termios`, `pty`, `tty` (terminal)
- `os.fork`, `multiprocessing.Process.start` (process model)
- `grp`, `pwd`, `nis` (user/group lookup)

These either contradict the wasm sandbox model or have no plausible
analog. Document as "not supported in this runtime" and move on.

## Notable existing surface

What works that's worth advertising:

- Full asyncio (with one event-loop backend that uses `select` / `wasi:io/poll`)
- Full json, xml.etree, csv, pickle (with `_pickle` C accelerator)
- Full sqlite3 (via `_sqlite_cap` + sqlite:wasm capability)
- Full decimal with `_decimal` C accelerator
- urllib + http.client + smtplib + ftplib + imaplib + poplib (all
  network-stdlib over `_socket` + ssl_capability)
- email + html parsing
- importlib + zipimport + zipfile (so loading wheels works)
- compileall + py_compile (bytecode tooling)
- inspect, dis, ast, opcode (full introspection)

## Test snapshot

The functional probe results from this sweep (against
`build/python.composed.wasm` at commit `1000c12`):

| Probe | Result |
|---|---|
| `hashlib.sha256(b'x').hexdigest()` | ✅ `2d711642…` |
| `hashlib.pbkdf2_hmac('sha256', b'x', b'y', 1)` | ❌ NotImplementedError (documented) |
| `hashlib.sha224(b'x')` | ❌ NotImplementedError (documented) |
| `ssl.create_default_context()` | ✅ `<SSLContext>` |
| `ssl.SSLObject()` | ❌ NotImplementedError (documented) |
| `urllib.request.urlopen('https://example.com')` | ✅ 528-byte HTML body, chunked decoded |
| `urllib.request.urlopen('https://www.python.org')` | ✅ 12015 bytes (matches native python) |
| `urllib.request.urlopen('https://httpbin.org/get')` | ✅ JSON body, parses |
| `sqlite3.connect(':memory:').execute('SELECT 1').fetchone()` | ✅ `(1,)` |
| `xml.etree.ElementTree.fromstring('<a>x</a>').text` | ✅ `'x'` |
| `bz2.compress / bz2.decompress` roundtrip | ✅ |
| `gzip.decompress` | ❌ EOFError (Lib/zlib.py shim streaming bug — file as separate) |
| `json.loads / json.dumps` roundtrip | ✅ |
| `uuid.uuid4()` | ✅ |
| `decimal.Decimal('1.5')` | ✅ |
| `zoneinfo.ZoneInfo('UTC')` | ❌ no tzdata bundled |
| `threading.Thread().start()` | ❌ no preemptive threads |
| `socket.create_connection((host, port))` (with DNS) | ❌ gaierror -4 |
| `os.fork()` | ❌ no fork |
| `subprocess.run` (default build) | ❌ v86 stub returns GuestNotReady |

## Recommended sequence

**All HIGH-priority gaps closed 2026-05-28.** The MEDIUM gaps split into:

- ✅ MED-5, MED-6 (extra hash algos) — turned out to be already-shipped
  via CPython builtin _sha2/_sha3
- ⏸ MED-7 (ssl.get_server_certificate) — blocked on openssl-component
  cap revision; can't be done at the Python layer
- ⏸ MED-8 (mmap) — needs new cap; medium-utility

What's left for "feels like a competent Python runtime out of the box":

| Gap | Track |
|---|---|
| ssl.get_server_certificate | openssl-component v0.2.x — adds SSL_get_peer_certificate / i2d_X509 |
| scrypt | tegmentum:kdf cap (or vendor in pure-Python from the `cryptography` lib if low-tier perf is acceptable) |
| mmap | tegmentum:memory cap, or document `io.BytesIO` as the workaround for most use cases |

These are all **cap-side** work. The Python-side surface (Pattern A
extensions + Lib/ shims) is now feature-complete for the 14
stdlib-hashlib algorithms, gzip/tar/zip compression, ssl with bundled
roots, tzdata, and pbkdf2.

The Phase 5 retirement is **functionally complete** for the default
build: `import hashlib`, `import ssl`, `import gzip`, `import zoneinfo`,
and all common operations work end-to-end without `STATIC_OPENSSL=1`.

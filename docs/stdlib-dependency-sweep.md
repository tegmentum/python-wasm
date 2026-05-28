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
| `socket` | TCP via `wasi:sockets/tcp` | DNS (`gaierror: [Errno -4]`) | `socket.getaddrinfo` not wired to `wasi:sockets/ip-name-lookup` |
| `subprocess` | imports + Popen object | `Popen.spawn()` requires v86 backend ready | `v86-posix-stub` returns `GuestNotReady` until v86 grows real spawn |
| `hashlib` | 9 algorithms (md5..blake2s) | sha224, sha3_224, sha3_384, shake_128, shake_256, pbkdf2_hmac, scrypt | not in `crypto-hash-multiplexer` 0.1.0 + no KDF cap |
| `ssl` | TLS handshake, cert validation, urllib.urlopen, MemoryBIO | SSLObject, SSLSession, get_server_certificate, DER_cert_to_PEM_cert, RAND_add, RAND_status | deferred to openssl-component v1.1 |
| `gzip` | `gzip.compress` works | `gzip.decompress` raises `EOFError: Compressed file ended before the end-of-stream marker` | bug in Lib/zlib.py shim's streaming decompressor — file separately |
| `zoneinfo` | imports + algorithm | `ZoneInfo('UTC')` fails | no tzdata bundled in deps/cpython/Lib/zoneinfo/ |
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

### High-priority

1. **DNS resolution** — `socket.getaddrinfo()` failing is the single most
   impactful gap. Block on: wiring `wasi:sockets/ip-name-lookup` (already
   in wasi-p2 0.2.x) into `_socket`. Likely a CPython patch + maybe a
   small wrapper. **Effort:** 1–2 days.

2. **tzdata bundle** — `zoneinfo` works algorithmically but ships no
   timezone database. Ship a curated tzdata blob (~200 KB compressed),
   either as part of python.wasm or as a sibling asset. No new cap
   needed — just data. **Effort:** 0.5 day.

3. **gzip.decompress bug** — `Lib/zlib.py` shim's streaming
   decompressor doesn't signal end-of-stream correctly to `gzip`.
   `zlib.decompressobj(31).decompress(data)` works directly; the gzip
   module's incremental usage doesn't. **Effort:** 0.5–1 day, no
   new cap.

4. **pbkdf2_hmac, scrypt** — common KDFs needed by hashlib users
   (passlib, cryptography). Two paths:
   - Pure-Python pbkdf2 implementation built on top of capability-
     routed HMAC (slow but correct; ~50 LOC).
   - New WIT contract `tegmentum:kdf-multiplexer/kdf-dispatcher` →
     PBKDF2, scrypt, argon2.
   **Effort:** pure-Python 1 day; full cap 5–7 days.

### Medium-priority

5. **sha224, sha3_224, sha3_384** — `crypto-hash-multiplexer` v0.2.x
   addition. Mechanical extension to the dispatcher enum + wasm. May
   land alongside other multiplexer revisions. **Effort:** 1 day
   cap-side + auto-pickup on this side.

6. **shake_128, shake_256 (XOFs)** — variable-length output, different
   shape than the fixed-digest contract. New `xof-dispatcher` interface
   in crypto-hash-multiplexer. **Effort:** 2 days.

7. **ssl.get_server_certificate** — out-of-band cert fetching helper.
   Could be pure-Python on top of existing SSLSocket (open conn,
   getpeercert, close). **Effort:** 1 day.

8. **mmap** — `wasi:io/memory` doesn't exist; would need a new
   `tegmentum:memory/anonymous-pages` cap or use wasi-p2 filesystem
   APIs as a backing for file-backed mmap. **Effort:** 5–7 days,
   medium-utility (most Python code path-of-least-resistance to
   `io.BytesIO` anyway).

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

A tight three-week plan to close the high-value gaps:

| Week | Gap | Action | Cap repo |
|---|---|---|---|
| 1 (M-W) | DNS + tzdata bundle | Wire `wasi:sockets/ip-name-lookup` into `_socket`; ship tzdata-2026a as a stdlib asset | (no new cap) |
| 1 (Th-F) | gzip streaming bug | Patch `Lib/zlib.py` shim's decompressor end-of-stream signaling | (no new cap) |
| 2 (M-W) | pbkdf2 + scrypt in pure-Python | Build PBKDF2 + scrypt on top of capability-routed HMAC | (no new cap; pure-Python) |
| 2 (Th-F) | sha224/sha3_224/sha3_384 | Land in `crypto-hash-multiplexer` v0.2.0; rebuild | crypto-hash-multiplexer |
| 3 (M-T) | ssl.get_server_certificate (pure-Python) | Build on existing SSLSocket primitives | (no new cap) |
| 3 (W-F) | Documentation + ship | Update this sweep + componentize-python.md; tag a release | — |

After these 3 weeks, the python-wasm runtime would cover the
~98%-of-real-world stdlib usage threshold that matters for `uv python
install wasm32-wasip2` to feel like a competent Python runtime out of
the box.

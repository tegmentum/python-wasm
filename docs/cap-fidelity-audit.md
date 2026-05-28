# Cap fidelity audit (2026-05-28)

Survey of every cap python-wasm consumes, focused on whether each cap's WIT surface preserves the relevant CPython stdlib semantics or loses them to a least-common-denominator interface.

Methodology: read the cap's WIT, compare to the stdlib module(s) it backs, list the lossy gaps, classify by impact.

## Summary table

| Cap | Type | Stdlib consumer | Stdlib hurt? | Direct-cap-user hurt? |
|---|---|---|---|---|
| compression-multiplexer | dispatcher-mux | `zlib`, `bz2`, `lzma`, `compression.zstd` | **yes** | yes |
| crypto-hash-multiplexer | dispatcher-mux | (none — stdlib uses CPython builtins) | no | yes |
| hashing-multiplexer | dispatcher-mux | (none — xxhash/blake3/etc. are 3rd-party) | no | yes |
| password-hash-multiplexer | dispatcher-mux | (none — pbkdf2 is pure-Python in `_hashlib.py`) | no | **yes** |
| openssl-component | cap-specific | `ssl` | no | no |
| sqlite-component | cap-specific | `sqlite3` | no | no |
| v86-posix-stub | cap-specific | `subprocess` (`_v86_posix` ext) | no | no |

**Where multiplexers actively hurt stdlib fidelity: only compression.** The other three muxes (`crypto-hash`, `hashing`, `password-hash`) are bypassed by stdlib — `hashlib` uses CPython builtin `_sha2`/`_sha3`/`_blake2`/`_md5`/`_sha1`, `hashlib.pbkdf2_hmac` was reworked in Phase 5.1 to be pure-Python over those builtins, and xxhash/blake3/etc. aren't in stdlib at all.

The two cap-specific (non-mux) components, `openssl-component` and `sqlite-component`, are already shaped around the operations their stdlib modules need. No fidelity issue there.

## Per-cap detail

### compression-multiplexer — **stdlib parity gap**

WIT exposes a generic `compressor`/`decompressor` resource pair with `compress`/`decompress` returning opaque bytes. Stdlib gaps:

- `zlib`: missing `wbits` (zlib vs raw DEFLATE vs gzip framing — `gzip` module depends on raw), `zdict`, `crc32`/`adler32`, `max_length`, `unused_data`/`unconsumed_tail`/`eof`
- `bz2`: missing `unused_data`, `needs_input`, `max_length`
- `lzma`: missing `format` (XZ/ALONE/RAW), `check`, `preset`-extreme, custom `filters`, `memlimit`, `unused_data`, `needs_input`, `max_length`
- `compression.zstd`: missing `ZstdDict`, dictionary training, advanced parameters, `unused_data`, `needs_input`, `max_length`

Workaround we shipped in Blocked-4 (`decompress-counted`): the multiplexer learned one codec-specific feature (DEFLATE-end consumed-bytes) at the cost of an awkward WIT addition. That's the symptom — every codec-specific feature requires a new mux method.

**Recommendation: B (migrate to per-codec caps with richer WITs).** This is the work currently in Tasks #98-103 (zlib first; bz2/lzma/zstd as follow-ups). User already chose this direction.

### crypto-hash-multiplexer — no stdlib hurt

WIT: generic `hasher` resource with `constructor(algo)`, `update`, `finish`, `reset` + one-shot `digest`. Algo enum: `md5`, `sha1`, `sha256`, `sha384`, `sha512`, `sha3256`, `sha3512`, `blake2b`, `blake2s`.

Gaps vs. CPython `hashlib`:
- No `digest_size` query (sha512 = 64, blake2b output is variable, SHAKE has variable output…)
- No `block_size` query
- No `.copy()` (rolling-digest patterns)
- blake2 family: no `digest_size`/`key`/`salt`/`person`/`fanout`/`depth`/etc. params — just generic blake2b/blake2s with default config
- SHAKE (variable-length output XOF) missing entirely — `hashlib.shake_128`/`shake_256` are stdlib
- SHA-3 224 and 384 missing (mux only has 256/512)

**But stdlib isn't routed through the cap.** `Lib/_hashlib.py` is the cap-routed-overlay-shim, but after Phase 5.1's redesign it only provides `pbkdf2_hmac` (pure-Python) and CPython's builtin `_sha2`/`_sha3`/`_blake2`/`_md5`/`_sha1` cover the full hashlib surface natively. The cap is only consumed by `import _crypto_hash` directly.

**Recommendation: A (keep as-is).** Direct cap users are advanced enough to accept the limitations; stdlib isn't impacted.

### hashing-multiplexer — no stdlib hurt

WIT: same shape as crypto-hash-mux but `xxh32`, `xxh64`, `xxh3`, `xxh128`, `crc32`, `crc32c`, `murmur3`, `murmur128`, `blake3`, all with a `seed: u64` parameter.

Gaps:
- xxh3 takes a "secret" (arbitrary bytes), not just a u64 seed
- blake3 has its own keyed-hash and key-derive contexts, not seed-shaped
- Output is canonical big-endian bytes — users often want raw integers

**But no stdlib uses these algorithms.** `binascii.crc32(data, value=0)` is the one stdlib match for CRC-32, and the mux satisfies that. The other algos are 3rd-party packages on PyPI (`xxhash`, `blake3`, `mmh3`).

**Recommendation: A (keep as-is).** Cap is only used by `import _xxhash` directly. No stdlib impact.

### password-hash-multiplexer — direct-cap-user gap (significant)

WIT exposes:
- `hash(algo, password) -> phc_string` (recommended params + random salt baked in)
- `verify(password, phc_string) -> bool`
- `derive(algo, password, salt, length) -> bytes` (recommended cost)

Gaps vs. CPython:
- `hashlib.pbkdf2_hmac(hash_name, password, salt, iterations, dklen=None)` — can't specify iterations (mux uses one fixed value), can't specify hash function (mux is sha256-only)
- `hashlib.scrypt(password, *, salt, n, r, p, maxmem=0, dklen=64)` — can't specify cost params
- argon2 isn't in stdlib, but the mux's "recommended" defaults will diverge from what other Python argon2 implementations chose

**Stdlib isn't routed through the cap.** Same as the hash multiplexers — `Lib/_hashlib.py`'s `pbkdf2_hmac` is pure-Python over CPython builtins (Phase 5.1), so `hashlib.pbkdf2_hmac(..., iterations=600000)` works at the expected iteration count.

But `_kdf_cap.derive()` (the Blocked-1 cap surface) inherits the multiplexer's lossy interface. Direct users of `_kdf_cap.derive('pbkdf2', pw, salt, 32)` get whatever iteration count the cap hardcoded. They can't choose 100k vs 600k vs custom.

**Recommendation: C (extend WIT).** Small additions to expose iterations on pbkdf2 and n/r/p on scrypt. Argon2 can keep "recommended" if we name a specific RFC-9106 profile. Doesn't require splitting into per-algorithm caps.

### openssl-component — good fidelity

Cap-specific WIT for TLS: `tls.context`, `tls.connect`, `x509.cert.parse_chain`, `x509.cert.encode`, `peer_info` (chain + ALPN + protocol version + cipher), `random.bytes` + `random.priv_bytes`. The python `_ssl_capability` extension layers stdlib-equivalent `SSLContext`/`SSLSocket`/`MemoryBIO`/`get_server_certificate` on top.

The two gaps we found (Phase 5.2.1 EOF-handling, Blocked-2 getpeercert) were both already serviceable through the existing WIT — no cap revisions needed for either.

**Recommendation: keep.** Already in the right shape.

### sqlite-component — good fidelity

Cap-specific WIT mirrors the sqlite3 C API (databases, statements, bind/step/finalize, row/column accessors). The `_sqlite_cap` ext + `Lib/sqlite3/__init__.py` shim exposes stdlib `sqlite3.connect`/`Connection`/`Cursor`.

**Recommendation: keep.**

### v86-posix-stub — good fidelity

Cap-specific WIT for process spawn (`spawn(program, args, env, cwd, stdin, stdout, stderr)`). Subprocess shim wraps it. Already shaped for the consumer.

**Recommendation: keep.**

## What changes scope

Out of the four multiplexers, only **compression** has a stdlib parity problem that justifies migration to per-algorithm caps. The user already chose to do this (Tasks #98–103, started with zlib).

The other three multiplexers are fine as-is for stdlib — they're bypassed entirely (`hashlib`, `binascii`) or consumed only via direct-cap surface that users opted into.

**Password-hash-multiplexer is the second-most-painful** but the right fix isn't migration to per-KDF caps — it's adding three or four parameter fields to the existing WIT (iterations, n, r, p, maybe argon2's m/t/p). That's a single-cap revision, not a fleet refactor.

## Suggested follow-on tasks

After the compression migration (#98–#103) lands:

1. **password-hash-multiplexer WIT extension** — add explicit-cost variants of `derive` (e.g., `derive-pbkdf2(pw, salt, iterations, hash_name, dklen)`, `derive-scrypt(pw, salt, n, r, p, dklen)`). Backward-compatible — keep the existing `derive(algo, pw, salt, length)` as the "recommended cost" entry point.

2. **crypto-hash-multiplexer WIT extension (optional)** — add SHAKE family + missing SHA-3 variants + `digest_size`/`block_size` queries. Only if a real consumer wants them; stdlib is fine without.

3. **hashing-multiplexer WIT extension (optional)** — flexible-init for xxh3/blake3 keyed mode. Only if a real consumer wants it.

The compression migration is the only one driven by stdlib pain. The rest are quality-of-life for direct cap users and can defer until they have demand.

"""_hashlib — pure-Python `_hashlib` for python-wasm's default build.

CPython's stdlib `hashlib` and `hmac` both try `import _hashlib as
_hashopenssl` to bind to the static OpenSSL-backed C extension. The
default python-wasm build doesn't include static OpenSSL, so the real
`_hashlib.so` is missing. Stdlib gracefully handles ImportError on the
module itself, but ASSUMES that if `_hashlib` is importable, it provides:

  * `compare_digest` — constant-time bytes comparison
  * `openssl_md5`, `openssl_sha1`, `openssl_sha256`, … — constructors
  * `openssl_md_meth_names` — set of supported names
  * `new(name, data, **kwargs)` — by-name constructor
  * `UnsupportedDigestmodError` — exception type
  * `hmac_new`, `hmac_digest` — fast-path HMAC (OPTIONAL — hmac.py
    falls back to its pure-Python implementation if absent)
  * `pbkdf2_hmac`, `scrypt` — KDFs (OPTIONAL — hashlib.py silently
    omits them if absent)

This module provides everything except `hmac_new`/`hmac_digest` (hmac
falls back to pure-Python, which is fast enough on CPython builtins)
and `scrypt` (pure-Python scrypt is too slow to be useful).

All real hash work delegates to CPython's BUILTIN C extensions
(`_sha2`, `_sha3`, `_blake2`, `_md5`, `_sha1`) — these are statically
linked into python.wasm and don't need OpenSSL. So the `openssl_*` names
here are just thin aliases.

Phase 5.1 redesign (was: a Lib/hashlib.py overlay routing through the
crypto-hash capability — turned out to be unnecessary since builtin
_sha2 etc. cover all 14 stdlib algorithms natively). This file is
the minimum to keep `hashlib.pbkdf2_hmac` available; the rest of
hashlib + hmac work via stdlib + builtins.
"""

from __future__ import annotations

# --- compare_digest --------------------------------------------------------
# Constant-time comparison. Delegate to _operator's builtin (no Python
# loop, faster than a hand-rolled version, and importing _operator
# doesn't pull in hmac → no circular import on module load).

from _operator import _compare_digest as compare_digest


# --- openssl_<algo> aliases ------------------------------------------------
# These are normally OpenSSL-backed but stdlib code only requires that
# they're CALLABLE and have hashlib-compatible interfaces. The builtin
# _sha2 etc. constructors satisfy both. hmac.py uses `type(openssl_sha256)`
# for isinstance() checks, so consistency in constructor type matters.

from _md5  import md5  as openssl_md5
from _sha1 import sha1 as openssl_sha1
from _sha2 import sha224 as openssl_sha224
from _sha2 import sha256 as openssl_sha256
from _sha2 import sha384 as openssl_sha384
from _sha2 import sha512 as openssl_sha512
from _sha3 import sha3_224 as openssl_sha3_224
from _sha3 import sha3_256 as openssl_sha3_256
from _sha3 import sha3_384 as openssl_sha3_384
from _sha3 import sha3_512 as openssl_sha3_512
from _sha3 import shake_128 as openssl_shake_128
from _sha3 import shake_256 as openssl_shake_256
from _blake2 import blake2b as openssl_blake2b
from _blake2 import blake2s as openssl_blake2s


# --- openssl_md_meth_names -------------------------------------------------
# Set of names hashlib.new uses for the fast-path lookup.

openssl_md_meth_names = frozenset({
    "md5", "sha1",
    "sha224", "sha256", "sha384", "sha512",
    "sha3_224", "sha3_256", "sha3_384", "sha3_512",
    "shake_128", "shake_256",
    "blake2b", "blake2s",
})


# --- UnsupportedDigestmodError ---------------------------------------------

class UnsupportedDigestmodError(ValueError):
    """Raised when a digest name isn't recognized by this _hashlib."""


# --- new(name, data, **kwargs) --------------------------------------------
# Stdlib hashlib's fast path: `_hashlib.new(name, data, usedforsecurity=...)`.
# Returns a hasher object using the named builtin.

_ALGO_TO_FUNC = {
    "md5":       openssl_md5,
    "sha1":      openssl_sha1,
    "sha224":    openssl_sha224,
    "sha256":    openssl_sha256,
    "sha384":    openssl_sha384,
    "sha512":    openssl_sha512,
    "sha3_224":  openssl_sha3_224,
    "sha3_256":  openssl_sha3_256,
    "sha3_384":  openssl_sha3_384,
    "sha3_512":  openssl_sha3_512,
    "shake_128": openssl_shake_128,
    "shake_256": openssl_shake_256,
    "blake2b":   openssl_blake2b,
    "blake2s":   openssl_blake2s,
}


def new(name, data=b"", *, usedforsecurity=True, **kwargs):
    """`hashlib._hashlib.new(name, data)` — construct a hasher by name.

    `usedforsecurity` is accepted for stdlib compatibility but ignored
    (the builtin hashers don't expose a FIPS mode toggle)."""
    try:
        ctor = _ALGO_TO_FUNC[name.lower()]
    except (KeyError, AttributeError):
        raise UnsupportedDigestmodError(
            f"unsupported hash type {name!r}") from None
    # Forward extra kwargs (e.g., blake2b's `key`/`digest_size`/`person`)
    if kwargs:
        return ctor(data, **kwargs)
    return ctor(data)


# --- hmac_new / hmac_digest ----------------------------------------------
# hmac.py's `__init` chain looks like:
#   1. try _hashopenssl.hmac_new (OpenSSL fast-path)
#   2. except UnsupportedDigestmodError: try the builtin `_hmac` module
#   3. fall back to pure-Python HMAC
#
# Tier 2 uses CPython's builtin `_hmac` C extension which is statically
# linked into python.wasm. So the right move here is to declare both
# hmac_new and hmac_digest as ALWAYS unsupported — hmac.py then falls
# through to `_init_builtin_hmac` and everyone's happy. Without these
# stubs hmac.py crashes with AttributeError (it doesn't expect _hashlib
# to be partially-shaped).


def hmac_new(*_args, **_kwargs):
    raise UnsupportedDigestmodError(
        "OpenSSL-backed hmac_new not available in this _hashlib — "
        "stdlib hmac falls back to the builtin _hmac C extension")


def hmac_digest(*_args, **_kwargs):
    raise UnsupportedDigestmodError(
        "OpenSSL-backed hmac_digest not available in this _hashlib — "
        "stdlib hmac falls back to the builtin _hmac C extension")


# --- pbkdf2_hmac -----------------------------------------------------------
# RFC 8018 §5.2. Pure-Python on top of stdlib hmac (which uses CPython's
# builtin hash C extensions, so the per-iteration work IS C-fast).
#
# Performance: ~30× slower than OpenSSL's C PBKDF2 due to the per-byte
# XOR loop being Python. For 100k iterations of HMAC-SHA256 → ~200ms;
# for 600k iterations → ~1.2s. Acceptable for password verification;
# cache the result rather than re-deriving on every request.


def pbkdf2_hmac(hash_name, password, salt, iterations, dklen=None):
    """RFC 8018 PBKDF2-HMAC.

    Stdlib-compatible signature; same semantics as `_hashlib.pbkdf2_hmac`
    when built against OpenSSL."""
    # Lazy import — hmac.py imports _hashlib at top-of-module, which
    # is THIS module, so importing hmac here at module top would be a
    # circular import. Defer until first call.
    import hmac as _hmac

    if not isinstance(hash_name, str):
        raise TypeError("hash_name must be str")
    if not isinstance(iterations, int):
        raise TypeError("iterations must be int")
    if iterations < 1:
        raise ValueError("iterations must be >= 1")
    if dklen is not None:
        if not isinstance(dklen, int):
            raise TypeError("dklen must be int or None")
        if dklen < 1:
            raise ValueError("dklen must be >= 1")

    digest_size = _hmac.new(b"", None, hash_name).digest_size
    if dklen is None:
        dklen = digest_size
    if dklen > (2**32 - 1) * digest_size:
        raise OverflowError("dklen too large")

    password = bytes(password)
    salt = bytes(salt)

    # Pre-keyed HMAC so copy() is cheap per iteration.
    prf = _hmac.new(password, None, hash_name)
    blocks = (dklen + digest_size - 1) // digest_size
    result = bytearray(blocks * digest_size)

    for block_idx in range(1, blocks + 1):
        # U_1 = HMAC(password, salt || INT(block_idx))
        h = prf.copy()
        h.update(salt)
        h.update(block_idx.to_bytes(4, "big"))
        u_i = h.digest()
        t = bytearray(u_i)
        # U_j = HMAC(password, U_{j-1});  T = T xor U_j  for 2..iterations
        for _ in range(iterations - 1):
            h = prf.copy()
            h.update(u_i)
            u_i = h.digest()
            for i in range(digest_size):
                t[i] ^= u_i[i]
        result[(block_idx - 1) * digest_size : block_idx * digest_size] = t

    return bytes(result[:dklen])


# scrypt — routed through `_kdf_cap` (password-hash-multiplexer). Native
# wasm speed (libscrypt under the cap) so production parameters are usable.
def scrypt(password, *, salt, n, r, p, maxmem=0, dklen=64):
    """RFC 7914 scrypt.

    Matches stdlib ``hashlib.scrypt`` signature. ``maxmem`` is accepted for
    API parity but not enforced — the cap's native impl sizes its arena
    against ``n*r*128``."""
    import _kdf_cap
    if not isinstance(n, int) or n < 2 or (n & (n - 1)) != 0:
        raise ValueError("n must be a power of 2 >= 2")
    if not isinstance(r, int) or r < 1:
        raise ValueError("r must be >= 1")
    if not isinstance(p, int) or p < 1:
        raise ValueError("p must be >= 1")
    if not isinstance(dklen, int) or dklen < 1:
        raise ValueError("dklen must be >= 1")
    return _kdf_cap.derive_scrypt(bytes(password), bytes(salt), n, r, p, dklen)


__all__ = (
    "compare_digest",
    "openssl_md_meth_names",
    "UnsupportedDigestmodError",
    "new",
    "pbkdf2_hmac",
    "scrypt",
    # openssl_<algo> are also public via dir() but not in __all__ —
    # they're implementation details that hashlib + hmac reach for.
)

"""hashlib — capability-routed replacement for CPython's stdlib hashlib.

Phase 5.1 of the componentize-python plan. Replaces the stdlib hashlib
module entirely (installed at deps/cpython/Lib/hashlib.py by
`make install-python-shims`). Routes all hash construction through
`_crypto_hash` — the cpython-ext static extension that imports
`tegmentum:crypto-hash-multiplexer/hash-dispatcher`.

The default build no longer ships static `_hashlib` (no OpenSSL link),
so the stdlib hashlib's `import _hashlib` would fail. This shim is a
self-contained replacement that doesn't depend on `_hashlib` at all.

## What's supported

The crypto-hash-multiplexer exposes 9 algorithms via the wasm capability:

    md5, sha1, sha256, sha384, sha512, sha3_256, sha3_512, blake2b, blake2s

Both `hashlib.new(name)` and the per-algorithm constructors
(`hashlib.sha256()`, `hashlib.blake2b()`, etc.) work for those names.

## What's NOT supported

| Stdlib name | Status here | Why |
|---|---|---|
| sha224, sha3_224, sha3_384 | NotImplementedError | not in the multiplexer's 0.1.0 dispatcher |
| shake_128, shake_256 | NotImplementedError | XOF (variable-length output) — different shape than the fixed-digest contract |
| pbkdf2_hmac, scrypt | NotImplementedError | KDFs — separate WIT contract to be added |
| hmac (separate stdlib module) | works via `hmac.new(key, ...)` because `hmac` uses the public hashlib API and the capability hashers satisfy it |
| file_digest (3.11+) | works (reads file, feeds chunks to the hasher) |

`algorithms_guaranteed` and `algorithms_available` reflect what's
actually here — calling code that probes these sets gets accurate
answers rather than promises this hashlib can't keep.

## Backwards compatibility

Code that did `import hashlib_capability; hashlib_capability.install()`
still works — the shim is still installed at Lib/hashlib_capability.py
and its monkey-patches end up touching this module (which they don't
need to, since the routing is already correct, but they're harmless).
"""

from __future__ import annotations

import _crypto_hash


# ---------------------------------------------------------------------------
# Hasher class — the object hashlib.new() and per-algo constructors return
# ---------------------------------------------------------------------------


class _CapHasher:
    """A hashlib-shape wrapper around the `_crypto_hash` capability hasher
    resource. Matches the surface of `_hashlib.HASH` / `hashlib._Hash`."""

    __slots__ = ("_inner", "_block_size")

    # block_size isn't carried by the capability; hardcode per-algo defaults
    # matching what stdlib OpenSSL hashlib reports.
    _BLOCK_SIZES = {
        "md5":      64,
        "sha1":     64,
        "sha256":   64,
        "sha384":  128,
        "sha512":  128,
        "sha3_256":136,  # rate for SHA-3-256
        "sha3_512": 72,  # rate for SHA-3-512
        "blake2b": 128,
        "blake2s":  64,
    }

    def __init__(self, name: str, data: bytes = b"") -> None:
        self._inner = _crypto_hash.new(name)
        self._block_size = self._BLOCK_SIZES.get(name, 64)
        if data:
            self._inner.update(data)

    # public attributes per the hashlib API
    @property
    def name(self) -> str:
        return self._inner.name

    @property
    def digest_size(self) -> int:
        return self._inner.digest_size

    @property
    def block_size(self) -> int:
        return self._block_size

    # public methods
    def update(self, data) -> None:
        # Accept bytes-like (memoryview, bytearray) the same way stdlib does
        self._inner.update(bytes(data) if not isinstance(data, bytes) else data)

    def digest(self) -> bytes:
        return self._inner.digest()

    def hexdigest(self) -> str:
        return self._inner.hexdigest()

    def copy(self) -> "_CapHasher":
        clone = object.__new__(_CapHasher)
        clone._inner = self._inner.copy()
        clone._block_size = self._block_size
        return clone

    def __repr__(self) -> str:
        return f"<{self.name} _CapHasher object @ 0x{id(self):x}>"


# ---------------------------------------------------------------------------
# Algorithm registry
# ---------------------------------------------------------------------------


# The cap reports its supported list at import time; freeze it as the set
# this hashlib promises to support.
algorithms_available: frozenset[str] = frozenset(_crypto_hash.algorithms())
"""Set of hash algorithm names available on this hashlib."""

algorithms_guaranteed: frozenset[str] = algorithms_available
"""Set of hash algorithm names guaranteed to be supported.

Unlike CPython's stdlib hashlib (which guarantees a wider set including
sha224/sha3_224/shake_*), this build only guarantees what the
crypto-hash-multiplexer capability supports — calling sha224(...) here
raises NotImplementedError rather than promising something we can't
deliver. Code that probes algorithms_guaranteed will get the truth."""


# Stdlib-shape "openssl_<name>" aliases — some code (CPython internals,
# third-party hashlib polyfills) does `from _hashlib import openssl_sha256`.
# Provide them as module-level functions for compatibility.
def new(name: str, data=b"", *, usedforsecurity: bool = True) -> _CapHasher:
    """Return a new hash object using the given algorithm.

    `usedforsecurity` is accepted for stdlib compatibility but ignored —
    the capability doesn't expose a FIPS mode toggle (the multiplexer
    decides which underlying impl to use)."""
    if name not in algorithms_available:
        raise NotImplementedError(
            f"hash algorithm '{name}' not available via the crypto-hash "
            f"capability (this hashlib supports: "
            f"{sorted(algorithms_available)})")
    if data and not isinstance(data, (bytes, bytearray, memoryview)):
        raise TypeError(
            f"object supporting the buffer API required, got {type(data).__name__}")
    return _CapHasher(name, bytes(data) if data else b"")


# Per-algorithm constructors. Generated at module import so each is a
# real function (introspection-friendly), not a lambda.
def _make_algo_ctor(algo_name: str):
    def _ctor(data=b"", *, usedforsecurity: bool = True) -> _CapHasher:
        return new(algo_name, data, usedforsecurity=usedforsecurity)
    _ctor.__name__ = algo_name
    _ctor.__qualname__ = algo_name
    _ctor.__doc__ = f"Return a {algo_name} hash object; optionally initialized with `data`."
    return _ctor


for _name in algorithms_available:
    globals()[_name] = _make_algo_ctor(_name)
del _name, _make_algo_ctor


# ---------------------------------------------------------------------------
# Stubs for unsupported stdlib API surface
# ---------------------------------------------------------------------------


def _unsupported(api: str, why: str = ""):
    """Return a callable that raises NotImplementedError with a clear
    message — used for stdlib hashlib API the capability doesn't cover."""
    def _stub(*_args, **_kwargs):
        msg = (f"hashlib.{api} is not available via the crypto-hash "
               f"capability in the default build")
        if why:
            msg += f" ({why})"
        msg += (". Either switch to a supported algorithm, or rebuild with "
                "STATIC_OPENSSL=1 to restore the static _hashlib path.")
        raise NotImplementedError(msg)
    _stub.__name__ = api
    return _stub


# Per-algorithm constructors that the stdlib defines but the cap doesn't
# (would otherwise be silent AttributeError; explicit stub is nicer)
for _missing in ("sha224", "sha3_224", "sha3_384", "shake_128", "shake_256"):
    globals()[_missing] = _unsupported(_missing,
                                       why="not in crypto-hash-multiplexer 0.1.0")
del _missing

pbkdf2_hmac = _unsupported(
    "pbkdf2_hmac",
    why="key derivation isn't part of the hash capability — separate KDF "
        "contract pending")

scrypt = _unsupported(
    "scrypt",
    why="key derivation isn't part of the hash capability — separate KDF "
        "contract pending")


# ---------------------------------------------------------------------------
# file_digest (Python 3.11+)
# ---------------------------------------------------------------------------


def file_digest(fileobj, digest, /, *, _bufsize: int = 2**18) -> _CapHasher:
    """Hash the contents of a file-like object. `digest` is either a string
    algorithm name or a callable returning a hashlib-shape hasher (lets
    callers pass the per-algo constructors)."""
    if isinstance(digest, str):
        h = new(digest)
    elif callable(digest):
        h = digest()
    else:
        raise TypeError(
            "digest must be a string algorithm name or a callable returning "
            f"a hashlib-shape object, got {type(digest).__name__}")

    if hasattr(fileobj, "getbuffer"):  # io.BytesIO short-circuit
        h.update(fileobj.getbuffer())
        return h
    if hasattr(fileobj, "readinto"):
        buf = bytearray(_bufsize)
        view = memoryview(buf)
        while True:
            n = fileobj.readinto(buf)
            if not n:
                break
            h.update(view[:n])
        return h
    # fallback path for files without readinto
    while chunk := fileobj.read(_bufsize):
        h.update(chunk)
    return h


# ---------------------------------------------------------------------------
# Module identity hints
# ---------------------------------------------------------------------------


__all__ = (
    "new",
    "algorithms_guaranteed",
    "algorithms_available",
    "file_digest",
    "pbkdf2_hmac",
    "scrypt",
    *sorted(algorithms_available),
    # Stubs for stdlib-named-but-unsupported algos so `from hashlib import sha224`
    # raises the explicit NotImplementedError when called, not ImportError.
    "sha224", "sha3_224", "sha3_384", "shake_128", "shake_256",
)

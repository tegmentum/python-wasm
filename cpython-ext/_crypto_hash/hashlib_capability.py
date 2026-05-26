"""hashlib_capability — proof-of-concept shim wiring _crypto_hash into hashlib's
public API so `hashlib.sha256(b'abc').hexdigest()` resolves to the capability
component instead of OpenSSL's `_hashlib`.

This module isn't installed automatically yet (Phase 5 retires the static
OpenSSL `_hashlib`); for now you opt in:

    import hashlib_capability  # registers as hashlib.new backend

Run it through python.composed.wasm to see the cross-validation against the
existing static `_hashlib` (OpenSSL) output.
"""

from __future__ import annotations

import hashlib as _hashlib_stdlib
import _crypto_hash


class _CapabilityHasher:
    """A minimal hashlib-shaped wrapper around _crypto_hash.hasher."""

    __slots__ = ("_inner",)

    def __init__(self, name: str, data: bytes = b"") -> None:
        self._inner = _crypto_hash.new(name)
        if data:
            self._inner.update(data)

    @property
    def name(self) -> str:
        return self._inner.name

    @property
    def digest_size(self) -> int:
        return self._inner.digest_size

    @property
    def block_size(self) -> int:
        # Not exposed by the capability — return a sane default for compatibility.
        return 64

    def update(self, data: bytes) -> None:
        self._inner.update(data)

    def digest(self) -> bytes:
        return self._inner.digest()

    def hexdigest(self) -> str:
        return self._inner.hexdigest()

    def copy(self) -> "_CapabilityHasher":
        clone = object.__new__(_CapabilityHasher)
        clone._inner = self._inner.copy()
        return clone

    def __repr__(self) -> str:
        return f"<_CapabilityHasher {self.name}>"


_SUPPORTED = frozenset(_crypto_hash.algorithms())


def new(name: str, data: bytes = b"") -> _CapabilityHasher:
    if name not in _SUPPORTED:
        raise ValueError(f"unsupported hash algorithm via capability: {name}")
    return _CapabilityHasher(name, data)


def install() -> None:
    """Override hashlib.new + the per-algorithm constructors with the capability."""
    _hashlib_stdlib.new = new  # type: ignore[assignment]
    for name in _SUPPORTED:
        setattr(_hashlib_stdlib, name, lambda data=b"", _n=name: new(_n, data))


# Auto-install on import so `import hashlib_capability` does the wiring.
install()

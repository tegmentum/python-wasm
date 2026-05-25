"""pyhash — importable Python API backed by the hashing-multiplexer capability
component (tegmentum:hashing-multiplexer/hashing-dispatcher) over WIT.

python-wasm's consumption lane: worker code `import pyhash` and gets a
hashlib/xxhash/mmh3-style API; every call routes to the Rust multiplexer
composed in via wac. Mirrors how a Python package binds to a language-agnostic
capability component.
"""
from wit_world.imports import hashing_dispatcher as _hd
from wit_world.imports.hashing_dispatcher import Algorithm, Hasher as _WitHasher

_ALGO = {
    "xxh32": Algorithm.XXH32,
    "xxh64": Algorithm.XXH64,
    "xxh3": Algorithm.XXH3,
    "xxh128": Algorithm.XXH128,
    "crc32": Algorithm.CRC32,
    "crc32c": Algorithm.CRC32C,
    "murmur3": Algorithm.MURMUR3,
    "murmur128": Algorithm.MURMUR128,
    "blake3": Algorithm.BLAKE3,
}
_NAME = {v: k for k, v in _ALGO.items()}


def algorithms() -> list[str]:
    return [_NAME[a] for a in _hd.supported_algorithms()]


def digest(algo: str, data: bytes, seed: int = 0) -> bytes:
    return _hd.digest(_ALGO[algo], data, seed)


def hexdigest(algo: str, data: bytes, seed: int = 0) -> str:
    return digest(algo, data, seed).hex()


def intdigest(algo: str, data: bytes, seed: int = 0) -> int:
    return int.from_bytes(digest(algo, data, seed), "big")


def xxh32(data: bytes, seed: int = 0) -> int: return intdigest("xxh32", data, seed)
def xxh64(data: bytes, seed: int = 0) -> int: return intdigest("xxh64", data, seed)
def xxh3(data: bytes, seed: int = 0) -> int: return intdigest("xxh3", data, seed)
def xxh128(data: bytes, seed: int = 0) -> int: return intdigest("xxh128", data, seed)
def crc32(data: bytes) -> int: return intdigest("crc32", data, 0)
def crc32c(data: bytes) -> int: return intdigest("crc32c", data, 0)
def murmur3(data: bytes, seed: int = 0) -> int: return intdigest("murmur3", data, seed)
def murmur128(data: bytes, seed: int = 0) -> int: return intdigest("murmur128", data, seed)
def blake3(data: bytes) -> bytes: return digest("blake3", data, 0)


class Hasher:
    """Streaming hasher (hashlib-style: update / digest / hexdigest / reset)."""

    def __init__(self, algo: str, seed: int = 0):
        self.name = algo
        self._h = _WitHasher(_ALGO[algo], seed)

    def update(self, data: bytes) -> None:
        self._h.update(data)

    def digest(self) -> bytes:
        return self._h.finish()

    def hexdigest(self) -> str:
        return self._h.finish().hex()

    def intdigest(self) -> int:
        return int.from_bytes(self._h.finish(), "big")

    def reset(self) -> None:
        self._h.reset()

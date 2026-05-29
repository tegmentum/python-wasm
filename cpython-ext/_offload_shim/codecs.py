"""Codec layer for the py-offload reference worker.

Phase 1 implements json (stdlib) and msgpack (self-contained, see _msgpack.py).
arrow and pickle are recognized by the WIT enum but not implemented — see
docs/native-execution-and-parallelism.md §10 (arrow → Phase 3; pickle is opt-in,
trusted-only).
"""

from __future__ import annotations

import json

from . import _msgpack
from .types import Codec


def encode(codec: Codec, obj) -> bytes:
    if codec == Codec.JSON:
        return json.dumps(obj).encode("utf-8")
    if codec == Codec.MSGPACK:
        return _msgpack.packb(obj)
    raise NotImplementedError(f"codec {codec.value!r} is not implemented in Phase 1")


def decode(codec: Codec, data: bytes):
    if codec == Codec.JSON:
        return json.loads(bytes(data).decode("utf-8"))
    if codec == Codec.MSGPACK:
        return _msgpack.unpackb(data)
    raise NotImplementedError(f"codec {codec.value!r} is not implemented in Phase 1")

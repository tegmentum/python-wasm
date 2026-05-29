"""Framed byte-stream transport for the py-offload contract.

A resident worker and its host talk over a byte stream — a subprocess pipe here,
a v86 guest's serial/virtiofs channel in Tier 1. Each message is a 4-byte
big-endian length prefix followed by a msgpack control frame. The control plane
(msgpack) is independent of the task's data-plane `codec`; `task.args` and a
result's `ok` value travel as opaque byte strings inside the frame.
"""

from __future__ import annotations

import struct

from . import _msgpack
from .types import Codec, Ok, PyError, Raised, Task


def write_frame(stream, payload: bytes) -> None:
    stream.write(struct.pack(">I", len(payload)))
    stream.write(payload)
    stream.flush()


def read_frame(stream):
    """Read one frame, or None at a clean end-of-stream."""
    header = _read_exact(stream, 4, allow_eof=True)
    if header is None:
        return None
    (length,) = struct.unpack(">I", header)
    return _read_exact(stream, length)


def _read_exact(stream, n: int, *, allow_eof: bool = False):
    buf = bytearray()
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            if allow_eof and not buf:
                return None
            raise EOFError("unexpected EOF while reading frame")
        buf += chunk
    return bytes(buf)


# --- request / response frames --------------------------------------------


def encode_request(env: str, task: Task) -> bytes:
    return _msgpack.packb(
        {
            "env": env,
            "entry": task.entry,
            "codec": task.codec.value,
            "args": task.args,  # already codec-encoded; carried as msgpack bin
        }
    )


def decode_request(payload: bytes):
    d = _msgpack.unpackb(payload)
    task = Task(entry=d["entry"], args=d["args"], codec=Codec(d["codec"]))
    return d["env"], task


def encode_response(outcome) -> bytes:
    if isinstance(outcome, Ok):
        return _msgpack.packb({"tag": "ok", "value": outcome.value})
    err = outcome.error
    return _msgpack.packb(
        {
            "tag": "raised",
            "kind": err.kind,
            "message": err.message,
            "traceback": err.traceback,
        }
    )


def decode_response(payload: bytes):
    d = _msgpack.unpackb(payload)
    if d["tag"] == "ok":
        return Ok(d["value"])
    return Raised(
        PyError(kind=d["kind"], message=d["message"], traceback=d["traceback"])
    )

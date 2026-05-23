"""Python mirror of the tegmentum:py-offload WIT types (see wit/py-offload.wit)."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Union


class Codec(str, Enum):
    MSGPACK = "msgpack"
    ARROW = "arrow"
    PICKLE = "pickle"
    JSON = "json"


@dataclass
class Task:
    entry: str  # "package.module:callable"
    args: bytes  # encoded { "args": [...], "kwargs": {...} }
    codec: Codec


@dataclass
class PyError:
    kind: str
    message: str
    traceback: str


@dataclass
class Ok:
    value: bytes  # encoded return value


@dataclass
class Raised:
    error: PyError


# WIT: variant outcome { ok(list<u8>), raised(py-error) }
Outcome = Union[Ok, Raised]

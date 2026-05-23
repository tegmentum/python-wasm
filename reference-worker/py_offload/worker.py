"""Reference implementation of the py-offload `offload.run` operation.

Resolves "package.module:callable", decodes the call payload, invokes the
callable, and encodes the result — mapping any exception that escapes to a
py-error. This is Phase 1: a plain native CPython worker, independent of v86 or
girder, that proves the WIT contract and the codecs end to end.
"""

from __future__ import annotations

import importlib
import traceback
from functools import reduce
from typing import Any, Callable

from . import codecs
from .types import Ok, Outcome, PyError, Raised, Task


def _resolve(entry: str) -> Callable[..., Any]:
    module_name, sep, attr_path = entry.partition(":")
    if not sep or not module_name or not attr_path:
        raise ValueError(f"entry {entry!r} must be 'package.module:callable'")
    obj: Any = importlib.import_module(module_name)
    obj = reduce(getattr, attr_path.split("."), obj)
    if not callable(obj):
        raise TypeError(f"entry {entry!r} resolved to a non-callable")
    return obj


def _split_call(payload: Any):
    """Normalize a decoded payload into (args, kwargs)."""
    if isinstance(payload, dict) and ("args" in payload or "kwargs" in payload):
        args = payload.get("args", [])
        kwargs = payload.get("kwargs", {})
    elif isinstance(payload, list):
        args, kwargs = payload, {}
    else:
        raise TypeError(
            "payload must be a list of positional args or a mapping with "
            "'args'/'kwargs' keys"
        )
    if not isinstance(args, list):
        raise TypeError("'args' must be a list")
    if not isinstance(kwargs, dict):
        raise TypeError("'kwargs' must be a mapping")
    return args, kwargs


def run(env: str, task: Task) -> Outcome:
    """Execute one offloaded call.

    `env` is opaque in Phase 1 (a single local environment) and is accepted but
    not used. Any exception raised while decoding, resolving, invoking, or
    encoding is mapped to ``Raised`` so it crosses the boundary as a normal
    outcome rather than crashing the worker.
    """
    del env  # opaque in Phase 1
    try:
        payload = codecs.decode(task.codec, task.args)
        args, kwargs = _split_call(payload)
        fn = _resolve(task.entry)
        result = fn(*args, **kwargs)
        return Ok(codecs.encode(task.codec, result))
    except Exception as exc:  # noqa: BLE001 — map any escape to the boundary
        return Raised(
            PyError(
                kind=type(exc).__name__,
                message=str(exc),
                traceback="".join(
                    traceback.format_exception(type(exc), exc, exc.__traceback__)
                ),
            )
        )

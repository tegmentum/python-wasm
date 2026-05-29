"""
python-wasm — pip-installable launcher for the WASI Preview 2 CPython interpreter.

The actual interpreter is a bundled python.composed.wasm artifact under
python_wasm/data/. This package is a thin Python wrapper that locates the
bundled artifacts, locates wasmtime on PATH, and execs the launcher script.

For most users `python-wasm` (the console script) is the only entry
point. Importing `python_wasm` is supported for tooling that wants to
discover the install layout — e.g.:

    from python_wasm import installed_prefix
    print(installed_prefix())
"""

from __future__ import annotations

import importlib.resources
import pathlib


__version__ = "0.1.0"


def installed_prefix() -> pathlib.Path:
    """Return the path where bundled data lives (under the installed package)."""
    return pathlib.Path(importlib.resources.files(__package__) / "data")

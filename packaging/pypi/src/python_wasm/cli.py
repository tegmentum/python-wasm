"""
python-wasm console entry point.

This is the pip-installed `python-wasm` command. It delegates to the
bundled launcher script (`data/bin/python-wasm`) because the launcher
already knows how to find the wasm artifact, dispatch subcommands, and
exec wasmtime with the right mounts.

This wrapper exists for two reasons:

1. `[project.scripts]` requires a Python callable as the entry point;
   the bundled launcher is a shell script.
2. The shell script expects its `$0` parent to be the install prefix,
   but pip-installs Python entry points as freshly-generated wrappers
   in `bin/` that don't preserve the package data layout. We resolve
   the data prefix via importlib.resources and then `os.execv` the
   shell script with PYTHON_WASM set, so it skips its own resolution.
"""

from __future__ import annotations

import os
import sys

from . import installed_prefix


def main() -> int:
    prefix = installed_prefix()
    launcher = prefix / "bin" / "python-wasm"
    if not launcher.is_file():
        sys.stderr.write(f"python-wasm: bundled launcher missing at {launcher}\n")
        return 1

    # The launcher script tries lib/ then data/ for the wasm artifact;
    # this package uses data/, so the launcher will find it. We just
    # exec the script with the same argv.
    env = os.environ.copy()
    env.setdefault("PYTHON_WASM_PIP_INSTALLED", "1")
    os.execve(str(launcher), [str(launcher), *sys.argv[1:]], env)
    return 0  # unreachable; execve replaces the process


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())

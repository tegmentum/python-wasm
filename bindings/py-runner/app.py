"""General script runner that exposes the wasm `zlib` capability in-process.

componentize-py builds this as a `wasi:cli/command`-shaped component whose extra
import is the compression dispatcher. We install `pyzlib` (the multiplexer-backed
shim) as the stdlib-style `zlib`, then run the user's Python script — so any
`import zlib` inside it is served IN-PROCESS by the wasm compression multiplexer
(Tier 0), no host, no native code.

  wasmtime run runner.composed.wasm /work/user.py [args...]
"""

import sys
import traceback

import pyzlib

# Route stdlib `zlib` imports to the multiplexer-backed shim.
sys.modules["zlib"] = pyzlib


class Run:
    """Implements `wasi:cli/run.run() -> result<_, _>`. Returning None = Ok."""

    def run(self) -> None:
        argv = sys.argv
        if len(argv) < 2:
            sys.stderr.write("usage: py-runner <script.py> [args...]\n")
            return

        script = argv[1]
        # The user script sees its own argv: argv[0] = script, then user args.
        sys.argv = argv[1:]
        try:
            with open(script, "rb") as f:
                code = compile(f.read(), script, "exec")
            globs = {"__name__": "__main__", "__file__": script}
            exec(code, globs)  # noqa: S102 — running user code is the entire job
        except SystemExit as e:
            ec = 0 if e.code is None else (int(e.code) if isinstance(e.code, int) else 1)
            if ec != 0:
                sys.stderr.write(f"py-runner: script exited with code {ec}\n")
        except BaseException:
            traceback.print_exc()

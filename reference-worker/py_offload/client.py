"""Host-side client that drives a resident worker over the framed transport.

`StreamClient` works against any reader/writer pair. `SubprocessClient` launches
`python -m py_offload.serve` and talks to it over stdio — a stand-in for a v86
guest reached over a serial/virtiofs byte stream: the transport is identical,
only the channel differs.
"""

from __future__ import annotations

import subprocess
import sys

from . import protocol
from .types import Outcome, Task


class StreamClient:
    def __init__(self, reader, writer):
        self._reader = reader
        self._writer = writer

    def run(self, env: str, task: Task) -> Outcome:
        protocol.write_frame(self._writer, protocol.encode_request(env, task))
        frame = protocol.read_frame(self._reader)
        if frame is None:
            raise EOFError("worker closed the connection")
        return protocol.decode_response(frame)


class SubprocessClient(StreamClient):
    def __init__(self, python: str = sys.executable):
        # stderr → DEVNULL: a task's incidental stdout/stderr is not part of the
        # contract (errors come back as `raised`); keep the protocol channel clean.
        self._proc = subprocess.Popen(
            [python, "-m", "py_offload.serve"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        super().__init__(self._proc.stdout, self._proc.stdin)

    def close(self) -> None:
        self._proc.stdin.close()  # EOF tells the resident loop to exit
        self._proc.wait(timeout=10)
        self._proc.stdout.close()

    def __enter__(self) -> "SubprocessClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

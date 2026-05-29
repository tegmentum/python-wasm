"""File-mailbox transport for py-offload over a shared directory.

A generic directory-as-channel transport: two parties exchange request/response
frames as files in a directory both can see. Originally written as the v86
guest⇄host channel over virtiofs (see docs/tier1-v86-integration.md for the
history of that — now-superseded — model); kept as a transport because it works
anywhere a shared filesystem is the cheapest IPC available, not only v86.

Protocol (one outstanding request at a time — the worker is serial):

    host   writes  req-<seq>.bin    (atomic: temp file + os.replace)
    guest  removes req-<seq>.bin, runs it, writes resp-<seq>.bin (atomic)
    host   reads + removes resp-<seq>.bin

Atomic rename guarantees a reader only ever observes a complete file (no torn
reads). Unlike the stdio transport, a task's stdout cannot corrupt this channel.
"""

from __future__ import annotations

import os
import sys
import time
from typing import Optional, Tuple

from . import protocol
from .types import Outcome, Task

# python-wasm only ships the client side of the mailbox transport. The
# guest-side serve loop (`worker.run`) lives outside the wasm guest — the
# host script that runs alongside python.composed.wasm provides it.
# Import `worker` lazily inside serve_mailbox so client-only callers in
# python-wasm don't need the worker module on disk.

_SHUTDOWN = ".shutdown"


def _write_atomic(directory: str, name: str, data: bytes) -> None:
    tmp = os.path.join(directory, name + ".tmp")
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, os.path.join(directory, name))  # atomic on one filesystem


def _read_file(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def _find_request(directory: str) -> Optional[Tuple[int, str]]:
    """The lowest-numbered pending req-<seq>.bin, if any."""
    best: Optional[Tuple[int, str]] = None
    for name in os.listdir(directory):
        if name.startswith("req-") and name.endswith(".bin"):
            try:
                seq = int(name[4:-4])
            except ValueError:
                continue
            if best is None or seq < best[0]:
                best = (seq, os.path.join(directory, name))
    return best


class MailboxClient:
    """Host-side client over a shared directory.

    Exposes the same ``run(env, task) -> Outcome`` shape as ``StreamClient``, so
    callers are transport-agnostic.
    """

    def __init__(self, directory: str, *, timeout: float = 30.0, poll: float = 0.005):
        self._dir = directory
        self._timeout = timeout
        self._poll = poll
        self._seq = 0

    def run(self, env: str, task: Task) -> Outcome:
        self._seq += 1
        seq = self._seq
        resp = os.path.join(self._dir, f"resp-{seq}.bin")
        _write_atomic(self._dir, f"req-{seq}.bin", protocol.encode_request(env, task))
        deadline = time.monotonic() + self._timeout
        while not os.path.exists(resp):
            if time.monotonic() >= deadline:
                raise TimeoutError(
                    f"no response for request {seq} within {self._timeout}s"
                )
            time.sleep(self._poll)
        data = _read_file(resp)
        os.remove(resp)
        return protocol.decode_response(data)


def serve_mailbox(directory: str, *, poll: float = 0.005, stop_check=None) -> None:
    """Guest-side resident loop: serve offload calls from a shared directory."""
    os.makedirs(directory, exist_ok=True)
    while True:
        if stop_check is not None and stop_check():
            return
        found = _find_request(directory)
        if found is None:
            time.sleep(poll)
            continue
        seq, path = found
        data = _read_file(path)
        os.remove(path)
        env, task = protocol.decode_request(data)
        from . import worker  # lazy — guest-side only
        outcome = worker.run(env, task)
        _write_atomic(directory, f"resp-{seq}.bin", protocol.encode_response(outcome))


def request_shutdown(directory: str) -> None:
    """Ask a serve_mailbox loop watching `directory` to exit."""
    open(os.path.join(directory, _SHUTDOWN), "wb").close()


def main(argv=None) -> None:
    args = sys.argv[1:] if argv is None else argv
    if not args:
        print("usage: python -m py_offload.mailbox <directory>", file=sys.stderr)
        raise SystemExit(2)
    directory = args[0]
    sentinel = os.path.join(directory, _SHUTDOWN)
    serve_mailbox(directory, stop_check=lambda: os.path.exists(sentinel))


if __name__ == "__main__":
    main()

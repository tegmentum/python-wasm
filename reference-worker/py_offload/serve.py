"""Resident worker loop — the guest-side dispatcher.

Reads request frames from a byte stream, runs each via `worker.run`, and writes
response frames back. One process handles many tasks (never restart per call),
which is the Tier-1 requirement: in v86 this loop runs inside a snapshot-restored
guest, reached over its serial/virtiofs channel.

Run as: `python -m py_offload.serve` (reads stdin, writes stdout).
"""

from __future__ import annotations

import sys

from . import protocol, worker


def serve(reader, writer) -> None:
    while True:
        frame = protocol.read_frame(reader)
        if frame is None:
            return
        env, task = protocol.decode_request(frame)
        outcome = worker.run(env, task)
        protocol.write_frame(writer, protocol.encode_response(outcome))


def main() -> None:
    # The protocol owns stdout, so redirect any stdout a task writes to stderr —
    # otherwise a print() inside an offloaded call would corrupt the frame stream
    # (the same hazard as a serial console shared with the guest's tty).
    transport_out = sys.stdout.buffer
    sys.stdout = sys.stderr
    serve(sys.stdin.buffer, transport_out)


if __name__ == "__main__":
    main()

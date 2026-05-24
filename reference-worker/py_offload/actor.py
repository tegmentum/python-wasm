"""girder turn-actor adapter for py-offload (Tier P).

girder runs each WASM instance as an actor that exports `init` and `handle`
(see ~/git/girder/wit/actor.wit). For Tier P a CPython-WASM instance is an actor
whose `handle` runs one offloaded call. This module is that adapter body: it
bridges the actor ABI (opaque message bytes in, response bytes out) to
`worker.run`, reusing the same wire frames as the other transports. The eventual
WASM component exports these as girder's `turn-actor` world (mapping the WIT
`message` record's payload to/from these bytes).
"""

from __future__ import annotations

from . import protocol, worker


def init(args: bytes) -> None:
    """Actor init. No per-actor setup yet; env/config will arrive here later."""
    del args
    return None


def handle(message: bytes) -> bytes:
    """Run one offloaded call: an encoded request frame in, response frame out."""
    env, task = protocol.decode_request(message)
    return protocol.encode_response(worker.run(env, task))

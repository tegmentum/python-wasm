# py-offload reference worker (Phase 1)

A plain-CPython implementation of the **`tegmentum:py-offload@0.1.0`** WIT contract
(`../wit/py-offload.wit`). It proves the interface and the json/msgpack codecs end
to end, independent of the eventual backends — the v86 native tier (Issue #2) and
the girder parallel tier (Issue #3) implement the *same* contract.

See `../docs/native-execution-and-parallelism.md` for the surrounding design
(§4 the contract, §10 the decisions that pin codecs and `env-id`).

## The contract

`offload.run(env, task) -> outcome` where:

- **`env`** — opaque environment id. Content-addressed in the full design; this
  reference worker accepts any value (a single local environment).
- **`task`** — `{ entry, args, codec }`:
  - `entry` — `"package.module:callable"`. The attribute after `:` may be dotted
    (e.g. `"builtins:str.format"`).
  - `args` — the call payload, encoded with `codec`: an object
    `{ "args": [...], "kwargs": {...} }` (a bare list is treated as positional
    args).
  - `codec` — encoding for both `args` and the return value.
- **`outcome`** — `ok(<encoded return value>)` or `raised({kind, message,
  traceback})`. Any exception while decoding, resolving, invoking, or encoding is
  mapped to `raised` so it crosses the boundary instead of crashing the worker.

## Codec support

| codec     | status                                            |
| --------- | ------------------------------------------------- |
| `json`    | implemented (stdlib)                              |
| `msgpack` | implemented (self-contained, `_msgpack.py`)       |
| `arrow`   | not implemented — Phase 3 (arrays/frames + SIR)   |
| `pickle`  | not implemented — opt-in, same-trust hops only    |

Requesting an unimplemented codec returns `raised(NotImplementedError)`.

## Layout

```
py_offload/
  __init__.py    public API: Codec, Task, Outcome, Ok, Raised, PyError, run
  types.py       dataclasses/enum mirroring the WIT types
  worker.py      run(): resolve entry, decode, invoke, encode, map exceptions
  codecs.py      json + msgpack encode/decode
  _msgpack.py    small self-contained MessagePack (no third-party dependency)
  protocol.py    length-prefixed frames over a byte stream (request/response)
  serve.py       resident worker loop (the guest-side dispatcher)
  client.py      host-side StreamClient / SubprocessClient
tests/
  test_worker.py     ok + raised paths over json and msgpack
  test_transport.py  resident worker driven over a subprocess byte stream
```

## Resident worker over a byte stream (Tier-1 transport)

Tier 1 (Issue #2) runs the worker as a long-lived process inside a v86 guest,
reached over the guest's serial/virtiofs channel. `serve.py` is that resident
loop and `protocol.py` is the wire format — a 4-byte length prefix plus a msgpack
control frame (the control plane is separate from the task's data-plane `codec`;
`args` and `ok` values ride as opaque bytes). `client.py` drives it from the host.

`SubprocessClient` stands in for the guest: it launches `python -m py_offload.serve`
and talks to it over stdio. The transport is identical to the v86 case; only the
channel differs.

```python
from py_offload import Codec, Ok, Task, codecs
from py_offload.client import SubprocessClient

with SubprocessClient() as client:                      # resident worker process
    task = Task("math:factorial", codecs.encode(Codec.JSON, {"args": [6]}), Codec.JSON)
    out = client.run("local", task)
    assert isinstance(out, Ok)
    assert codecs.decode(Codec.JSON, out.value) == 720  # one process, many calls
```

Note: `serve.py` redirects a task's stdout to stderr so a stray `print()` inside an
offloaded call can't corrupt the frame stream — the same hazard as a shared serial
console.

## Run the tests

```sh
cd reference-worker
python3 -m unittest discover -s tests -t .
```

No third-party dependencies — standard library only.

## Example

```python
from py_offload import Codec, Ok, Task, codecs, run

payload = {"args": [5]}                      # math.factorial(5)
task = Task("math:factorial", codecs.encode(Codec.JSON, payload), Codec.JSON)
out = run("local", task)
assert isinstance(out, Ok)
assert codecs.decode(Codec.JSON, out.value) == 120
```

## What's next

This is Phase 1 (Issue #1). Later phases reuse this exact contract:

- **Issue #2** — Tier 1 native exec via v86. The transport core is in place: the
  resident dispatcher (`serve.py`), the framed byte-stream protocol
  (`protocol.py`), and the host client (`client.py`), proven over a subprocess.
  Remaining: run `serve.py` on a native CPython inside a v86 guest, bind the
  client to the guest's serial/virtiofs channel, add snapshot restore, and
  benchmark.
- **Issue #3** — wrap CPython-WASM as a girder actor and fan calls across
  instances (Tier P); adds the `arrow` codec + SIR.
- **Issue #4** — an import hook that routes native-only package calls through this
  contract.

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
  serve.py       resident worker loop over a byte stream (stdio dispatcher)
  client.py      host-side StreamClient / SubprocessClient
  mailbox.py     file-mailbox transport over a shared dir (the Tier-1/virtiofs channel)
tests/
  test_worker.py     ok + raised paths over json and msgpack
  test_transport.py  resident worker driven over a subprocess byte stream
  test_mailbox.py    resident worker driven over a shared directory
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

### File mailbox over a shared directory (the Tier-1 channel)

`mailbox.py` is the transport Tier 1 actually uses: host and a guest-resident
worker exchange request/response frames as files in a directory the guest sees
over **virtiofs**. Because virtiofs is just a host directory mounted into the
guest, this needs **no changes inside the v86 emulator**. Frames are written with
a temp file + `os.replace`, so a reader never sees a torn file; and unlike the
stdio transport, a task's `print()` cannot corrupt the channel. The guest loop is
`serve_mailbox(dir)` (run via `python -m py_offload.mailbox <dir>`); the host uses
`MailboxClient(dir)`, which has the same `run(env, task)` shape as `StreamClient`.

`test_mailbox.py` exercises it with a tmpdir standing in for the virtiofs mount
and a subprocess standing in for the guest — identical transport, only the
directory's backing differs.

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

- **Issue #2** — Tier 1 native exec via v86. The transport is in place and proven
  locally: the resident dispatcher (`serve.py`), the framed protocol
  (`protocol.py`), the host clients (`client.py`), and the **virtiofs file-mailbox
  channel** (`mailbox.py`) that Tier 1 actually uses. Remaining (needs the v86
  runtime): a self-contained x86_64 CPython in the guest running
  `python -m py_offload.mailbox /run/py-offload`, wired via `workspace/init`, with
  snapshot restore and a latency benchmark. See `../docs/tier1-v86-integration.md`.
- **Issue #3** — wrap CPython-WASM as a girder actor and fan calls across
  instances (Tier P); adds the `arrow` codec + SIR.
- **Issue #4** — an import hook that routes native-only package calls through this
  contract.

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
  actor.py       girder turn-actor adapter: init/handle -> worker.run (Tier P)
  pool.py        host-side parallel map over a pool of resident workers (Tier P)
  importhook.py  meta_path finder: route native-only package calls via a backend
tests/
  test_worker.py      ok + raised paths over json and msgpack
  test_transport.py   resident worker driven over a subprocess byte stream
  test_mailbox.py     resident worker driven over a shared directory
  test_pool.py        actor adapter + parallel map across separate processes
  test_importhook.py  proxied import routes calls through an offload backend
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

## Parallel execution across workers (Tier P)

`actor.py` is the girder turn-actor adapter — `init` / `handle(message) -> bytes` —
that bridges girder's actor ABI to `worker.run`. A CPython-WASM instance running
this is one girder actor.

`pool.py` is the host-side fan-out: `WorkerPool.map(env, tasks)` distributes tasks
across N resident workers and collects results in order. Each worker is a separate
process — its own interpreter, its own GIL, its own memory — which is exactly
girder's shared-nothing model: the GIL never serializes work *across* workers.

```python
from py_offload import Codec, Task, codecs
from py_offload.pool import WorkerPool

def t(n):
    return Task("math:factorial", codecs.encode(Codec.MSGPACK, {"args": [n]}), Codec.MSGPACK)

with WorkerPool(size=4) as pool:                       # 4 separate processes
    outcomes = pool.map("local", [t(n) for n in range(10)])
```

`test_pool.py` proves the model: tasks run in distinct processes (distinct pids).
The CPU *speedup* measurement is the deferred benchmark — it needs multiple cores
and the girder runtime, so it is not a unit test.

## Import hook for native-only packages (Tier 4)

`importhook.py` makes a package whose native code has no wasip2 build usable from
the interpreter by routing its calls to a backend. `install(["numpy"], client)`
registers a `meta_path` finder so `import numpy` returns a proxy module; attribute
calls offload `numpy:<dotted.attr>(args, kwargs)` through any offload client and
return the decoded result. A remote exception is re-raised locally (as the matching
builtin type when one exists, else `OffloadError`).

```python
from py_offload import Codec
from py_offload.client import SubprocessClient
from py_offload.importhook import install

client = SubprocessClient()
with install(["math"], client, codec=Codec.MSGPACK):   # "math" stands in for a native pkg
    import math                                          # the proxy
    assert math.factorial(5) == 120                      # runs in the backend process
```

v1 is call-with-serializable-args. Transparent live-object proxying (ndarray views,
callbacks, stateful objects) is Issue #5. `test_importhook.py` proves the mechanism
with a stdlib module standing in for a native package; the numpy proof case needs
numpy installed in a backend environment.

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
- **Issue #3** — Tier P parallel via girder. The actor adapter (`actor.py`) and a
  parallel map over a worker pool (`pool.py`) are in place and proven across
  separate processes. Remaining (needs the girder runtime): componentize
  CPython-WASM to export girder's `turn-actor` world, run the pool on real girder
  actors, add the `arrow` codec + SIR for large read-only inputs, and benchmark
  multicore scaling.
- **Issue #4** — import hook / proxy. Implemented (`importhook.py`) and proven with
  a stdlib stand-in: a proxied import routes calls through an offload backend and
  re-raises remote exceptions. Remaining: the numpy proof case (numpy in a backend
  env) and richer attribute/dotted handling.

## End-to-end in the real wasip2 interpreter

The tests above run on host CPython. `run-wasip2-offload.sh` proves the same
contract inside the **actual `python.wasm`**: it starts a host mailbox worker and
runs `examples/offload_guest.py` under wasmtime. The guest imports
`examples/nativelib.py` — a package that is *not* built for wasip2 — through the
import hook, and its call is offloaded over the file mailbox to the host worker:

```
$ reference-worker/run-wasip2-offload.sh
offloaded nativelib.crunch(7) = {'squared': 49, 'ran_on': 'darwin', 'py': '3.13.0'}
caller interpreter: wasi  (the call ran on darwin)
```

So a package with no wasip2 build is usable from the wasip2 interpreter by
offloading its calls — the Tier-1 host/native-worker shape running against the
real target, using only `py_offload` (pure Python) + the file mailbox.

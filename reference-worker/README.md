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
tests/
  test_worker.py ok + raised paths over json and msgpack
```

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

- **Issue #2** — implement `offload.run` against a native CPython inside the v86
  wasmmachine (Tier 1).
- **Issue #3** — wrap CPython-WASM as a girder actor and fan calls across
  instances (Tier P); adds the `arrow` codec + SIR.
- **Issue #4** — an import hook that routes native-only package calls through this
  contract.

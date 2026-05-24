# Transparent object proxying — assessment (Issue #5)

Status: **evaluation, not a commitment to build.** Issue #5 exists to decide
whether call-with-serializable-args (Phases 1/4) is enough, or whether we need
transparent proxying of live backend objects. This documents where v1 breaks down,
what transparency would cost, and the recommendation.

## What v1 (call-offload) does well

`offload.run` / the import hook ship `entry(args, kwargs)` to a backend and return
a serialized result. This fits the motivating cases cleanly: coarse, mostly-pure
bulk compute where inputs and outputs are values —
`numpy.linalg.svd(a)`, `pandas.read_parquet(path)`, `scipy.optimize.minimize(f, x0)`.
Args/results cross as Arrow (arrays/frames) or msgpack (everything else).

## Where v1 breaks down

1. **Live object handles / identity.** `a = np.array(...)` returns an ndarray that
   lives in the backend. v1 serializes the whole array back; a follow-up
   `np.dot(a, a)` re-serializes it both ways. There is no way to keep the object
   resident in the backend and refer to it by reference.
2. **Stateful objects + methods.** `m = Model(...); m.fit(X); y = m.predict(Z)`.
   v1 cannot call a method on a backend-resident instance; `m` would have to be
   serialized between calls, which is usually impossible (C state, file handles,
   GPU buffers).
3. **Attribute access & mutation on results.** `a.shape` (read of a snapshot is
   fine) but `a[0] = 5` / `df.loc[...] = ...` mutate a backend object whose changes
   never propagate back.
4. **Callbacks / higher-order calls.** `np.vectorize(pyfn)`, `df.apply(fn)`,
   `sorted(x, key=fn)` pass a *host* callable into a *backend* call. That needs the
   backend to call back into the host mid-request — bidirectional RPC, which the
   one-shot request/response contract does not provide.
5. **Iterators / generators / context managers.** `for row in reader:`,
   `with open(p) as f:` are lazy/streaming protocols needing a round-trip per step.
6. **Non-serializable args/returns.** File handles, sockets, locks, memoryviews,
   anything with C-level identity.
7. **Rich exceptions.** v1 reconstructs `kind`/`message`/`traceback` only; custom
   exception attributes are lost.

## What transparency would require

Effectively a distributed-objects layer (the problem RPyC/Pyro solve):

- **Remote handles.** The backend keeps a table of live objects; returns a handle
  (id + type) instead of serializing; the host wraps it in a proxy. Every
  operation — `getattr`, `__call__`, `__getitem__`, `__setitem__`, operators,
  `__iter__`/`__next__`, `__enter__`/`__exit__`, `len`, `repr` — becomes an offload
  call referencing the handle. That is the whole Python data model over the wire.
- **Lifetime / GC.** Host proxy `__del__` must send "release handle"; needs
  refcounting and a session, with care around cycles and exceptions.
- **A bidirectional channel** for callbacks (backend → host calls). girder's actor
  ABI already has `host.call` (an actor calling the host), which is the natural fit;
  the stdio/mailbox one-shot transports would need full duplex.
- **A stateful session pinned to one worker.** Handles live in a specific worker,
  which **conflicts with the shared-nothing fan-out** that makes Tier P parallel —
  a real architectural tension.
- **By-value vs by-reference policy.** Decide per type: small/immutable by value,
  large/stateful by reference.

## Recommendation

**Do not build general transparent proxying now.** It is a large, chatty,
latency-sensitive distributed-objects system, and pinning handles to a worker
fights the parallelism model. Instead, in priority order:

1. **Keep v1 call-offload as the primary path** — it covers the bulk-compute sweet
   spot, which is the actual motivation for Tier 1.
2. **If stateful use cases prove necessary, add a minimal handle API** — not full
   transparency: `create(entry, args) -> handle`, `call_method(handle, name, args)
   -> value | handle`, `release(handle)`. This covers the create → few-method-calls
   → pull-serializable-result pattern (most ML/data flows) without proxying the
   entire data model, and it stays explicit about what is remote.
3. **Callbacks** only once a bidirectional channel exists (girder `host.call`);
   until then, require the callback's logic to be part of the offloaded entry.
4. **If full transparency is ever truly required**, evaluate adopting RPyC-style
   netrefs rather than hand-rolling — but expect it to be incompatible with
   shared-nothing fan-out.

## Decision

Transparent proxying is **deferred**. v1 (call-with-serializable-args) is the
contract; revisit only if concrete stateful/callback use cases appear, and then
prefer the **minimal explicit handle API** over general transparency. This issue
stays open as the record of that trigger condition.

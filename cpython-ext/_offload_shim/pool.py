"""
Offload-backed worker pool — the wasm-guest side of Phase 6.

Each "worker" is a host-side `serve_mailbox` process polling its own
mailbox directory. The pool fans out tasks across N workers by writing
each request to one of N MailboxClient instances (round-robin), then
waits for all responses.

Real parallelism comes from N independent host processes, not from
in-guest threading (which is single-threaded under our threading shim).
The submission step is non-blocking (atomic file write); only the wait
blocks, and we batch waits so workers run concurrently.

Public surface mirrors a subset of `multiprocessing.Pool`:

    OffloadPool(processes=4, packages=("numpy",))
        .map(func, iterable)      # blocks, returns list
        .imap(func, iterable)     # iterator, in order
        .apply(func, args, kwargs)
        .close() / .terminate() / .join()
        context manager

`func` is a string `"package.module:callable"` (the WIT entry shape).
For ergonomics with the importhook, _Call proxies expose `__call__` that
already encode this, so users can also pass a proxy callable:

    p = OffloadPool(4, packages=("numpy",))
    import numpy
    out = p.map(numpy.linalg.det, [[[1,2],[3,4]], [[5,6],[7,8]]])
"""

from __future__ import annotations

import os
from typing import Iterable, List, Sequence, Union

from . import codecs as _codecs
from .types import Codec, Task, Outcome, Raised
from .mailbox import MailboxClient


_DEFAULT_ENV = "local"


def _resolve_entry(target) -> str:
    """Accept a `"package.module:callable"` string or an importhook _Call proxy."""
    if isinstance(target, str):
        return target
    # importhook._Call exposes _package and _path; reconstruct entry.
    pkg = getattr(target, "_package", None)
    path = getattr(target, "_path", None)
    if pkg and path is not None:
        return f"{pkg}:{'.'.join(path)}"
    raise TypeError(
        "OffloadPool target must be 'package.module:callable' str or an "
        "importhook proxy callable"
    )


class OffloadPool:
    """N-worker pool over N mailbox directories.

    The host side spawns N `serve_mailbox` processes (see
    scripts/serve-offload-pool.sh). Each worker watches one of
    ``mailbox_root/mailbox-0/``, ``mailbox_root/mailbox-1/``, ….
    """

    def __init__(
        self,
        processes: int = 4,
        *,
        mailbox_root: str = "/work/pool",
        env: str = _DEFAULT_ENV,
        codec: Codec = Codec.MSGPACK,
    ):
        if processes < 1:
            raise ValueError("processes must be >= 1")
        self._codec = codec
        self._env = env
        self._workers: List[MailboxClient] = []
        for i in range(processes):
            mbox = os.path.join(mailbox_root, f"mailbox-{i}")
            if not os.path.isdir(mbox):
                raise FileNotFoundError(
                    f"OffloadPool: worker {i} mailbox {mbox} missing — "
                    f"is scripts/serve-offload-pool.sh running with N>={processes}?"
                )
            self._workers.append(MailboxClient(mbox))
        self._closed = False
        self._round_robin = 0

    @property
    def size(self) -> int:
        return len(self._workers)

    # ---- map / imap ----------------------------------------------------

    def map(self, target, iterable: Iterable, *, chunksize: int = 1) -> List:
        """Apply `target` to each item; returns a list of results in order.

        For real parallelism, submission has to come before any waits — so
        we write every request first, then collect every response. The
        host workers run concurrently in between.
        """
        if self._closed:
            raise RuntimeError("OffloadPool is closed")
        entry = _resolve_entry(target)
        items = list(iterable)
        n = len(items)
        if n == 0:
            return []

        # Phase 1: submit every task non-blockingly (atomic file writes).
        # We hand-roll the submit so we can fan out before waiting.
        seqs = [None] * n  # type: List[int]
        worker_index = [0] * n  # type: List[int]
        for i, arg in enumerate(items):
            w = i % self.size
            worker_index[i] = w
            payload = _codecs.encode(self._codec, {"args": [arg], "kwargs": {}})
            task = Task(entry=entry, args=payload, codec=self._codec)
            seqs[i] = self._workers[w]._submit(self._env, task)

        # Phase 2: collect responses in order. Each .wait_for(seq) blocks
        # until that specific request's response arrives. Since workers
        # run concurrently, by the time we wait for response i, response
        # j (j != i) may have already arrived — that's fine.
        results = []  # type: List
        for i in range(n):
            w = worker_index[i]
            outcome = self._workers[w]._wait_for(seqs[i])
            if isinstance(outcome, Raised):
                from .importhook import _raise_remote
                _raise_remote(outcome.error)
            value = _codecs.decode(self._codec, outcome.value)
            results.append(value)
        return results

    def imap(self, target, iterable: Iterable, *, chunksize: int = 1):
        # Simple ordered iterator. For now, materialize via map() — the
        # fan-out happens upfront so the laziness doesn't change wall-clock.
        for item in self.map(target, iterable, chunksize=chunksize):
            yield item

    # ---- apply -------------------------------------------------------

    def apply(self, target, args: Sequence = (), kwargs: dict = None):
        if self._closed:
            raise RuntimeError("OffloadPool is closed")
        if kwargs is None:
            kwargs = {}
        entry = _resolve_entry(target)
        payload = _codecs.encode(
            self._codec, {"args": list(args), "kwargs": dict(kwargs)}
        )
        task = Task(entry=entry, args=payload, codec=self._codec)
        w = self._round_robin % self.size
        self._round_robin += 1
        outcome = self._workers[w].run(self._env, task)
        if isinstance(outcome, Raised):
            from .importhook import _raise_remote
            _raise_remote(outcome.error)
        return _codecs.decode(self._codec, outcome.value)

    # ---- lifecycle ---------------------------------------------------

    def close(self) -> None:
        self._closed = True

    def terminate(self) -> None:
        self.close()

    def join(self) -> None:
        # Workers are external processes; nothing to join from the guest.
        pass

    def __enter__(self) -> "OffloadPool":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


__all__ = ("OffloadPool",)

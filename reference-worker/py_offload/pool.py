"""Host-side parallel map over a pool of resident workers (the Tier-P pattern).

girder spawns N isolated WASM-instance actors and fans calls across them (see
~/git/girder/crates/girder-runtime/examples/parallel_mapreduce.rs). Here each
"actor" is a resident worker process (a SubprocessClient) — a separate interpreter
with its own GIL and memory, exactly girder's shared-nothing model. `WorkerPool.map`
distributes tasks across the workers and collects results in order. Swap
`client_factory` for a girder-backed client to run on real actors.

This proves the orchestration and the shared-nothing model locally. The CPU
speedup measurement is the deferred benchmark (needs multiple cores and the girder
runtime), not a unit test.
"""

from __future__ import annotations

import queue
import threading
from typing import Callable, List, Sequence

from .client import SubprocessClient
from .types import Outcome, Task


class WorkerPool:
    def __init__(
        self, size: int = 4, *, client_factory: Callable[[], object] = SubprocessClient
    ):
        if size < 1:
            raise ValueError("pool size must be >= 1")
        self._clients = [client_factory() for _ in range(size)]

    @property
    def size(self) -> int:
        return len(self._clients)

    def map(self, env: str, tasks: Sequence[Task]) -> List[Outcome]:
        results: List[Outcome] = [None] * len(tasks)  # type: ignore[list-item]
        work: "queue.Queue[tuple[int, Task]]" = queue.Queue()
        for item in enumerate(tasks):
            work.put(item)
        errors: List[BaseException] = []

        def run_worker(client) -> None:
            while True:
                try:
                    index, task = work.get_nowait()
                except queue.Empty:
                    return
                try:
                    results[index] = client.run(env, task)
                except BaseException as exc:  # noqa: BLE001
                    errors.append(exc)
                    return

        threads = [
            threading.Thread(target=run_worker, args=(c,)) for c in self._clients
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        if errors:
            raise errors[0]
        return results

    def close(self) -> None:
        for client in self._clients:
            close = getattr(client, "close", None)
            if close is not None:
                close()

    def __enter__(self) -> "WorkerPool":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

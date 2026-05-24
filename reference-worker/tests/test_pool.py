import math
import unittest

from py_offload import Codec, Ok, Task, codecs, protocol
from py_offload.actor import handle, init
from py_offload.pool import WorkerPool


def _task(entry, payload, codec=Codec.MSGPACK):
    return Task(entry=entry, args=codecs.encode(codec, payload), codec=codec)


class TestActorAdapter(unittest.TestCase):
    """The girder turn-actor entry shape, exercised in-process."""

    def test_handle_roundtrip(self):
        init(b"")
        request = protocol.encode_request("local", _task("math:factorial", {"args": [5]}))
        outcome = protocol.decode_response(handle(request))
        self.assertIsInstance(outcome, Ok)
        self.assertEqual(codecs.decode(Codec.MSGPACK, outcome.value), 120)


class TestWorkerPool(unittest.TestCase):
    def test_map_results(self):
        with WorkerPool(size=4) as pool:
            tasks = [_task("math:factorial", {"args": [n]}) for n in range(10)]
            outcomes = pool.map("local", tasks)
            for n, outcome in enumerate(outcomes):
                self.assertIsInstance(outcome, Ok)
                self.assertEqual(codecs.decode(Codec.MSGPACK, outcome.value), math.factorial(n))

    def test_runs_in_separate_processes(self):
        # Each actor is its own process => its own interpreter and GIL (girder's
        # shared-nothing model). Brief sleeps spread the first round one-per-worker
        # so every worker is exercised; the distinct pids prove the model.
        with WorkerPool(size=4) as pool:
            tasks = [_task("tests._helpers:sleep_and_pid", {"args": [0.05]}) for _ in range(8)]
            outcomes = pool.map("local", tasks)
            pids = {codecs.decode(Codec.MSGPACK, o.value) for o in outcomes}
            self.assertEqual(len(pids), pool.size)


if __name__ == "__main__":
    unittest.main()

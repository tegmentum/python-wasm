import unittest

from py_offload import Codec, Ok, Raised, Task, codecs
from py_offload.client import SubprocessClient

CODECS = [Codec.JSON, Codec.MSGPACK]


def _task(entry, payload, codec):
    return Task(entry=entry, args=codecs.encode(codec, payload), codec=codec)


class TestTransport(unittest.TestCase):
    def test_roundtrip_over_subprocess(self):
        with SubprocessClient() as client:
            for codec in CODECS:
                with self.subTest(codec=codec):
                    out = client.run("local", _task("math:factorial", {"args": [6]}, codec))
                    self.assertIsInstance(out, Ok)
                    self.assertEqual(codecs.decode(codec, out.value), 720)

    def test_raised_over_subprocess(self):
        with SubprocessClient() as client:
            out = client.run("local", _task("math:factorial", {"args": [-1]}, Codec.JSON))
            self.assertIsInstance(out, Raised)
            self.assertEqual(out.error.kind, "ValueError")

    def test_resident_worker_handles_many_calls(self):
        # One process, many tasks — the Tier-1 "never restart per call" property.
        with SubprocessClient() as client:
            for n in range(5):
                out = client.run(
                    "local", _task("builtins:len", {"args": [[0] * n]}, Codec.MSGPACK)
                )
                self.assertEqual(codecs.decode(Codec.MSGPACK, out.value), n)

    def test_task_stdout_does_not_corrupt_protocol(self):
        # A print() inside an offloaded call must not break the frame stream.
        with SubprocessClient() as client:
            out = client.run("local", _task("builtins:print", {"args": ["noise"]}, Codec.JSON))
            self.assertIsInstance(out, Ok)
            self.assertIsNone(codecs.decode(Codec.JSON, out.value))


if __name__ == "__main__":
    unittest.main()

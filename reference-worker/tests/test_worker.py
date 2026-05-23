import unittest

from py_offload import Codec, Ok, Raised, Task, codecs, run

CODECS = [Codec.JSON, Codec.MSGPACK]


def _task(entry, payload, codec):
    return Task(entry=entry, args=codecs.encode(codec, payload), codec=codec)


class TestRun(unittest.TestCase):
    def test_ok_positional(self):
        for codec in CODECS:
            with self.subTest(codec=codec):
                out = run("local", _task("math:factorial", {"args": [5]}, codec))
                self.assertIsInstance(out, Ok)
                self.assertEqual(codecs.decode(codec, out.value), 120)

    def test_ok_kwargs(self):
        for codec in CODECS:
            with self.subTest(codec=codec):
                out = run(
                    "local",
                    _task(
                        "builtins:round",
                        {"args": [3.14159], "kwargs": {"ndigits": 2}},
                        codec,
                    ),
                )
                self.assertIsInstance(out, Ok)
                self.assertEqual(codecs.decode(codec, out.value), 3.14)

    def test_ok_dotted_attribute(self):
        # entry attribute path may be dotted: "module:Class.method"-style lookup.
        out = run(
            "local",
            _task("builtins:str.format", {"args": ["{}+{}", "a", "b"]}, Codec.JSON),
        )
        self.assertIsInstance(out, Ok)
        self.assertEqual(codecs.decode(Codec.JSON, out.value), "a+b")

    def test_raised_unserializable_result(self):
        # A result the codec cannot encode crosses the boundary as a raised error
        # rather than crashing the worker (Decimal is not JSON-serializable).
        out = run("local", _task("decimal:Decimal", {"args": ["1.5"]}, Codec.JSON))
        self.assertIsInstance(out, Raised)
        self.assertEqual(out.error.kind, "TypeError")

    def test_raised_from_callable(self):
        for codec in CODECS:
            with self.subTest(codec=codec):
                out = run("local", _task("math:factorial", {"args": [-1]}, codec))
                self.assertIsInstance(out, Raised)
                self.assertEqual(out.error.kind, "ValueError")
                self.assertIn("factorial", out.error.message)
                self.assertIn("Traceback", out.error.traceback)

    def test_raised_unresolvable_module(self):
        out = run("local", _task("no_such_module_xyz:fn", {"args": []}, Codec.JSON))
        self.assertIsInstance(out, Raised)
        self.assertEqual(out.error.kind, "ModuleNotFoundError")

    def test_raised_malformed_entry(self):
        out = run("local", _task("math.factorial", {"args": [5]}, Codec.JSON))
        self.assertIsInstance(out, Raised)
        self.assertEqual(out.error.kind, "ValueError")

    def test_msgpack_bytes_roundtrip(self):
        # msgpack carries bytes natively (json cannot); the result is bytes.
        out = run("local", _task("builtins:bytes", {"args": [[1, 2, 3]]}, Codec.MSGPACK))
        self.assertIsInstance(out, Ok)
        self.assertEqual(codecs.decode(Codec.MSGPACK, out.value), b"\x01\x02\x03")

    def test_unimplemented_codec_is_raised(self):
        # Requesting pickle/arrow surfaces as a raised boundary error, not a crash.
        out = run("local", Task(entry="math:factorial", args=b"", codec=Codec.PICKLE))
        self.assertIsInstance(out, Raised)
        self.assertEqual(out.error.kind, "NotImplementedError")


if __name__ == "__main__":
    unittest.main()

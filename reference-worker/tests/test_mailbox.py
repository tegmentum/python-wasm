import shutil
import subprocess
import sys
import tempfile
import unittest

from py_offload import Codec, Ok, Raised, Task, codecs
from py_offload.mailbox import MailboxClient, request_shutdown

CODECS = [Codec.JSON, Codec.MSGPACK]


def _task(entry, payload, codec):
    return Task(entry=entry, args=codecs.encode(codec, payload), codec=codec)


class TestMailbox(unittest.TestCase):
    """Drive a resident worker over a shared directory.

    The tmpdir stands in for the virtiofs mount; the subprocess stands in for the
    guest. The transport is identical to the v86 case — only the directory's
    backing differs.
    """

    def setUp(self):
        self._dir = tempfile.mkdtemp(prefix="pyoffload-mbx-")
        self._proc = subprocess.Popen(
            [sys.executable, "-m", "py_offload.mailbox", self._dir]
        )
        self._client = MailboxClient(self._dir, timeout=15.0)

    def tearDown(self):
        request_shutdown(self._dir)
        try:
            self._proc.wait(timeout=10)
        finally:
            shutil.rmtree(self._dir, ignore_errors=True)

    def test_roundtrip(self):
        for codec in CODECS:
            with self.subTest(codec=codec):
                out = self._client.run("local", _task("math:factorial", {"args": [6]}, codec))
                self.assertIsInstance(out, Ok)
                self.assertEqual(codecs.decode(codec, out.value), 720)

    def test_raised(self):
        out = self._client.run("local", _task("math:factorial", {"args": [-1]}, Codec.JSON))
        self.assertIsInstance(out, Raised)
        self.assertEqual(out.error.kind, "ValueError")

    def test_resident_worker_handles_many_calls(self):
        for n in range(5):
            out = self._client.run(
                "local", _task("builtins:len", {"args": [[0] * n]}, Codec.MSGPACK)
            )
            self.assertEqual(codecs.decode(Codec.MSGPACK, out.value), n)


if __name__ == "__main__":
    unittest.main()

import unittest

from py_offload import Codec
from py_offload.client import SubprocessClient
from py_offload.importhook import install


class TestImportHook(unittest.TestCase):
    """Route a 'native-only' package through an offload backend.

    `math` stands in for a package with no wasip2 build: it is imported as a proxy
    in this (host) process, while the real `math` runs in the backend worker
    process. Installed/uninstalled per test so other tests see the real module.
    """

    def setUp(self):
        self._client = SubprocessClient()
        self._hook = install(["math"], self._client, codec=Codec.MSGPACK)

    def tearDown(self):
        self._hook.uninstall()
        self._client.close()

    def test_proxied_call(self):
        import math  # the proxy

        self.assertEqual(math.factorial(5), 120)

    def test_proxied_kwargs(self):
        import math

        self.assertTrue(math.isclose(1.0, 1.001, rel_tol=0.01))
        self.assertFalse(math.isclose(1.0, 2.0, rel_tol=0.01))

    def test_remote_exception_reconstructed(self):
        import math

        with self.assertRaises(ValueError):
            math.factorial(-1)

    def test_real_module_restored_after_uninstall(self):
        self._hook.uninstall()
        import math

        self.assertEqual(math.factorial(5), 120)  # real, in-process math


if __name__ == "__main__":
    unittest.main()

"""Smoke tests for CPython running as a WASI Preview 2 component."""

import collections
import functools
import io
import itertools
import json
import math
import os
import sys


def test_platform():
    assert sys.platform == "wasi", f"Expected 'wasi', got {sys.platform!r}"


def test_math():
    assert abs(math.pi - 3.141592653589793) < 1e-10
    assert math.sqrt(144) == 12.0
    assert math.factorial(10) == 3628800


def test_collections():
    counter = collections.Counter("abracadabra")
    assert counter["a"] == 5
    deque = collections.deque([1, 2, 3])
    deque.appendleft(0)
    assert list(deque) == [0, 1, 2, 3]


def test_itertools():
    result = list(itertools.chain([1, 2], [3, 4]))
    assert result == [1, 2, 3, 4]
    result = list(itertools.islice(itertools.count(10), 5))
    assert result == [10, 11, 12, 13, 14]


def test_functools():
    @functools.lru_cache(maxsize=128)
    def fib(n):
        if n < 2:
            return n
        return fib(n - 1) + fib(n - 2)

    assert fib(30) == 832040


def test_io():
    buf = io.StringIO()
    buf.write("hello wasi")
    assert buf.getvalue() == "hello wasi"

    bbuf = io.BytesIO(b"\x00\x01\x02\x03")
    assert bbuf.read() == b"\x00\x01\x02\x03"


def test_json():
    data = {"name": "python-wasm", "version": 1, "tags": ["wasi", "p2"]}
    encoded = json.dumps(data, sort_keys=True)
    decoded = json.loads(encoded)
    assert decoded == data


def test_hashlib():
    import hashlib

    h = hashlib.sha256(b"hello wasi")
    assert len(h.hexdigest()) == 64


def test_os_environ():
    # Should be able to access environ dict (wasi:cli env)
    assert isinstance(os.environ, os._Environ)


def test_sys_args():
    # sys.argv should be populated
    assert isinstance(sys.argv, list)
    assert len(sys.argv) >= 1


if __name__ == "__main__":
    tests = [
        test_platform,
        test_math,
        test_collections,
        test_itertools,
        test_functools,
        test_io,
        test_json,
        test_hashlib,
        test_os_environ,
        test_sys_args,
    ]

    for test in tests:
        test()

    print(f"All {len(tests)} smoke tests passed")

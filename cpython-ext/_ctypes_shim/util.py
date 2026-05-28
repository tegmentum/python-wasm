"""ctypes.util stub for python-wasm.

find_library() returns None on every name because no native libraries are
loadable in wasm. Returning None matches the documented "not found"
contract, so callers that use the standard `if find_library(...): CDLL(...)
else: fall_back` pattern correctly take the fallback branch.
"""


def find_library(name):
    return None


def find_msvcrt():
    return None


def test():
    return None

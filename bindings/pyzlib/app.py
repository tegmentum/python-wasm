# Worker entrypoint demonstrating stdlib-style `zlib` over the compression
# multiplexer capability (Python -> WIT -> Rust, plugged via wac).
#
# The bundled CPython ships a builtin `zlib`, which would otherwise win over a
# `zlib.py` on sys.path. We install the multiplexer-backed shim as `zlib` in
# sys.modules so `import zlib` resolves to it — that is what "python-wasm with
# zlib support backed by the compression multiplexer" means in practice.
import sys

import wit_world
import pyzlib

sys.modules["zlib"] = pyzlib
import zlib  # now the multiplexer-backed shim


class WitWorld(wit_world.WitWorld):
    def run(self) -> str:
        original = b"hello hello hello zlib over the compression multiplexer " * 4
        compressed = zlib.compress(original, 9)
        restored = zlib.decompress(compressed)
        out = [
            "zlib via compression-multiplexer (DEFLATE / RFC 1950)",
            "zlib module = %s" % zlib.__name__,
            "original=%d bytes" % len(original),
            "compressed=%d bytes" % len(compressed),
            "header=%s" % compressed[:2].hex(),  # 789c => our shim produced it
            "roundtrip_ok=%s" % (restored == original),
            "crc32=%08x" % zlib.crc32(original),
            "adler32=%08x" % zlib.adler32(original),
        ]
        # raw-deflate path (negative wbits) round-trips too
        raw = zlib.compress(original)[2:-4]
        out.append("raw_inflate_ok=%s" % (zlib.decompress(raw, -zlib.MAX_WBITS) == original))
        return "\n".join(out)

import sys, zlib
original = b"hello zlib over the wasm multiplexer " * 8
compressed = zlib.compress(original, 9)
restored   = zlib.decompress(compressed)
print(f"runner on {sys.platform} (py {sys.version.split()[0]})")
print(f"  original   : {len(original)} bytes")
print(f"  compressed : {len(compressed)} bytes, header=0x{compressed[:2].hex()}")
print(f"  roundtrip  : {restored == original}")
print(f"  crc32      : 0x{zlib.crc32(original):08x}")
print(f"  zlib.__name__ = {zlib.__name__}")

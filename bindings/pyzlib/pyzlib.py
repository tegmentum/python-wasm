"""pyzlib — a drop-in `zlib` — Python stdlib-compatible compression backed by the compression
multiplexer capability component (tegmentum:compression-multiplexer/
compression-dispatcher) over WIT.

python-wasm provides `zlib` by routing DEFLATE to the already-built Rust
compression multiplexer instead of statically linking libz. `compress` emits
genuine zlib format (RFC 1950: a 2-byte header, raw DEFLATE, and an Adler-32
trailer) and `decompress` reads it (or raw DEFLATE with negative wbits), so the
output interoperates with the standard library's zlib.
"""

from wit_world.imports.compression_dispatcher import Algorithm, Compressor, Decompressor

DEFLATED = 8
MAX_WBITS = 15
Z_NO_COMPRESSION = 0
Z_BEST_SPEED = 1
Z_BEST_COMPRESSION = 9
Z_DEFAULT_COMPRESSION = -1


class error(Exception):
    """Raised on a zlib error (matches the standard library's ``zlib.error``)."""


def _level(level: int) -> int:
    if level == Z_DEFAULT_COMPRESSION:
        return 6
    if not 0 <= level <= 9:
        raise error("Bad compression level")
    return level


def _raw_deflate(data: bytes, level: int) -> bytes:
    with Compressor(Algorithm.DEFLATE, level) as c:
        return bytes(c.compress(data))


def _raw_inflate(data: bytes) -> bytes:
    try:
        with Decompressor(Algorithm.DEFLATE) as d:
            return bytes(d.decompress(data))
    except Exception as exc:  # the import raises Err(str)
        raise error(f"Error while decompressing data: {exc}") from None


def adler32(data, value: int = 1) -> int:
    a = value & 0xFFFF
    b = (value >> 16) & 0xFFFF
    for byte in bytes(data):
        a = (a + byte) % 65521
        b = (b + a) % 65521
    return ((b << 16) | a) & 0xFFFFFFFF


_CRC_TABLE = None


def _crc_table():
    global _CRC_TABLE
    if _CRC_TABLE is None:
        table = []
        for n in range(256):
            c = n
            for _ in range(8):
                c = (c >> 1) ^ 0xEDB88320 if (c & 1) else (c >> 1)
            table.append(c)
        _CRC_TABLE = table
    return _CRC_TABLE


def crc32(data, value: int = 0) -> int:
    table = _crc_table()
    crc = value ^ 0xFFFFFFFF
    for byte in bytes(data):
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF


def compress(data, level: int = Z_DEFAULT_COMPRESSION) -> bytes:
    """Compress *data* to zlib format (RFC 1950)."""
    data = bytes(data)
    body = _raw_deflate(data, _level(level))
    return b"\x78\x9c" + body + adler32(data).to_bytes(4, "big")


def decompress(data, wbits: int = MAX_WBITS, bufsize: int = 0) -> bytes:
    """Decompress zlib data (RFC 1950), or raw DEFLATE when *wbits* is negative."""
    data = bytes(data)
    if wbits < 0:
        return _raw_inflate(data)
    if len(data) < 6 or (data[0] & 0x0F) != DEFLATED:
        raise error("Error -3 while decompressing data: incorrect header check")
    out = _raw_inflate(data[2:-4])
    if adler32(out).to_bytes(4, "big") != data[-4:]:
        raise error("Error -3 while decompressing data: incorrect data check")
    return out

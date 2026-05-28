"""zlib shim — routes deflate through the compression-multiplexer capability.

Drop-in replacement for the stdlib `zlib` C extension. Uses
`_compress_cap.deflate_compress` / `deflate_decompress` for the raw
DEFLATE primitive, and implements the RFC 1950 (zlib) and RFC 1952
(gzip) wrappers, plus pure-Python adler32 and crc32, on top.

Once this shim is installed, CPython no longer needs `Modules/zlibmodule.c`
or static libz — saving ~150 KB on python.wasm and removing one more
non-capability link dependency.

Installed by `make install-python-shims` to `deps/cpython/Lib/zlib.py`.
Shadows the (now-disabled) static `zlibmodule.c` entry — to keep both
in place during A/B comparison, leave the C extension wired in
`Modules/Setup.local` and the Python loader will prefer the C module.
The intended end-state is `*disabled* zlib` in Setup.local + this file
serving every `import zlib`.

API parity:

  Module-level
    zlib.compress(data, level=-1, wbits=MAX_WBITS)         ✓
    zlib.decompress(data, wbits=MAX_WBITS, bufsize=...)    ✓ (bufsize ignored)
    zlib.crc32(data, value=0)                              ✓ pure-Python
    zlib.adler32(data, value=1)                            ✓ pure-Python

  Streaming objects
    zlib.compressobj(level=-1, method=DEFLATED, wbits=15,
                     memLevel=8, strategy=Z_DEFAULT_STRATEGY,
                     zdict=None)                            ✓ (buffered)
        .compress(data) / .flush(mode=Z_FINISH) / .copy()
    zlib.decompressobj(wbits=15, zdict=None)               ✓ (buffered)
        .decompress(data, max_length=0)
        .flush([length]) / .copy()
        .eof / .unused_data / .unconsumed_tail properties

  wbits semantics
        8..15        → zlib wrapper (RFC 1950)              ✓
       -15..-8       → raw DEFLATE                          ✓
       24..31 (|16)  → gzip wrapper (RFC 1952)              ✓
       40..47 (|32)  → auto-detect zlib/gzip on decompress  ✓
        0            → use header's preset (decompress only) ✓

  Limitations
    * `zdict` (preset dictionaries) raises NotImplementedError — would
      require the multiplexer to expose dict-aware deflate, which it
      doesn't today.
    * `memLevel` and `strategy` are accepted but ignored — the
      multiplexer doesn't expose those knobs.
    * `bufsize` on decompress is ignored — the cap is one-shot, output
      buffer is sized to fit.
    * Streaming compressobj/decompressobj buffer the full input;
      flush time runs one cap call. O(n) memory, fine for typical
      wheels (<10 MB) and bad for multi-GB streams.
"""

__all__ = [
    "DEFLATED", "DEF_BUF_SIZE", "DEF_MEM_LEVEL", "MAX_WBITS",
    "ZLIB_RUNTIME_VERSION", "ZLIB_VERSION",
    "Z_BEST_COMPRESSION", "Z_BEST_SPEED", "Z_BLOCK",
    "Z_DEFAULT_COMPRESSION", "Z_DEFAULT_STRATEGY",
    "Z_FILTERED", "Z_FINISH", "Z_FIXED", "Z_FULL_FLUSH",
    "Z_HUFFMAN_ONLY", "Z_NO_COMPRESSION", "Z_NO_FLUSH",
    "Z_PARTIAL_FLUSH", "Z_RLE", "Z_SYNC_FLUSH", "Z_TREES",
    "adler32", "compress", "compressobj", "crc32",
    "decompress", "decompressobj", "error",
]

import struct
import _compress_cap


# --------------------------------------------------------------------------
# Constants — values match RFC 1950 / CPython's zlibmodule.c so existing
# code that does e.g. `zlib.MAX_WBITS - 16` continues to work numerically.
# --------------------------------------------------------------------------
DEFLATED                = 8
MAX_WBITS               = 15
DEF_BUF_SIZE            = 16 * 1024
DEF_MEM_LEVEL           = 8

Z_NO_COMPRESSION        = 0
Z_BEST_SPEED            = 1
Z_BEST_COMPRESSION      = 9
Z_DEFAULT_COMPRESSION   = -1

Z_NO_FLUSH              = 0
Z_PARTIAL_FLUSH         = 1
Z_SYNC_FLUSH            = 2
Z_FULL_FLUSH            = 3
Z_FINISH                = 4
Z_BLOCK                 = 5
Z_TREES                 = 6

Z_DEFAULT_STRATEGY      = 0
Z_FILTERED              = 1
Z_HUFFMAN_ONLY          = 2
Z_RLE                   = 3
Z_FIXED                 = 4

# We don't link real libz; report a synthetic version string.
ZLIB_VERSION            = "0.0.0-capability"
ZLIB_RUNTIME_VERSION    = ZLIB_VERSION


class error(Exception):
    """Raised on invalid input or unsupported parameters."""


# --------------------------------------------------------------------------
# adler32 and crc32 — prefer the C implementations in _compress_cap (built
# in to the python-wasm cpython binary). Pure-Python fallback is kept as a
# safety net in case this shim is reused outside python-wasm without the
# capability extension. Both honor zlib's `value` running-state argument so
# callers can chain (e.g. streamed hash-of-large-file).
# --------------------------------------------------------------------------

try:
    crc32   = _compress_cap.crc32      # C-speed, table-based IEEE 802.3
    adler32 = _compress_cap.adler32    # C-speed
except AttributeError:
    # _compress_cap is present but predates the crc32/adler32 functions —
    # fall back to pure-Python implementations. Slow (10 MB takes ~2-3 s)
    # but correct; once the bundled _compress_cap is rebuilt these get
    # shadowed by the C versions on next import.

    _MOD_ADLER = 65521  # largest prime < 2^16

    def adler32(data, value=1):
        """Adler-32 (RFC 1950 §9). Pure-Python fallback."""
        s1 = value & 0xFFFF
        s2 = (value >> 16) & 0xFFFF
        CHUNK = 5552  # largest n s.t. n*(255+n) < 2^32 — defer modulo
        mv = memoryview(data)
        for off in range(0, len(mv), CHUNK):
            end = min(off + CHUNK, len(mv))
            for b in mv[off:end]:
                s1 += b
                s2 += s1
            s1 %= _MOD_ADLER
            s2 %= _MOD_ADLER
        return ((s2 << 16) | s1) & 0xFFFFFFFF

    def _build_crc32_table():
        table = [0] * 256
        for i in range(256):
            c = i
            for _ in range(8):
                c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
            table[i] = c
        return table

    _CRC32_TABLE = _build_crc32_table()

    def crc32(data, value=0):
        """CRC-32 (IEEE 802.3, RFC 1952 §8). Pure-Python fallback."""
        crc = (~value) & 0xFFFFFFFF
        t = _CRC32_TABLE
        for b in data:
            crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8)
        return crc ^ 0xFFFFFFFF


# --------------------------------------------------------------------------
# wbits dispatcher — translate the encoded wbits int into (raw|zlib|gzip)
# and the actual window bits in [8, 15].
# --------------------------------------------------------------------------

def _decode_wbits(wbits):
    """Return ('raw'|'zlib'|'gzip'|'auto', window_bits)."""
    if wbits == 0:
        # zlib's "use header's window" for decompress; treat as zlib auto.
        return ("zlib", MAX_WBITS)
    if -15 <= wbits <= -8:
        return ("raw", -wbits)
    if 8 <= wbits <= 15:
        return ("zlib", wbits)
    if 24 <= wbits <= 31:                # 16 + (8..15) → gzip
        return ("gzip", wbits - 16)
    if 40 <= wbits <= 47:                # 32 + (8..15) → auto-detect
        return ("auto", wbits - 32)
    raise ValueError(f"Invalid wbits value: {wbits}")


def _normalize_level(level):
    """zlib's -1 = Z_DEFAULT_COMPRESSION → 6 (multiplexer default)."""
    if level == Z_DEFAULT_COMPRESSION:
        return 6
    if not 0 <= level <= 9:
        raise error(f"Bad compression level: {level}")
    return level


# --------------------------------------------------------------------------
# zlib (RFC 1950) wrapping
# --------------------------------------------------------------------------

def _zlib_header(level):
    """Return the 2-byte RFC 1950 header (CMF, FLG) for `level`."""
    cmf = 0x78  # CINFO=7 (32K window) << 4 | CM=8 (deflate)
    if level >= 7:    flevel = 3
    elif level >= 4: flevel = 2
    elif level >= 2: flevel = 1
    else:            flevel = 0
    flg_base = flevel << 6  # FDICT=0
    fcheck = 31 - ((cmf * 256 + flg_base) % 31)  # makes (CMF*256+FLG) % 31 == 0
    return bytes((cmf, flg_base | fcheck))


def _zlib_unwrap(data):
    """Strip RFC 1950 zlib container, returning raw DEFLATE bytes."""
    if len(data) < 6:
        raise error("Invalid zlib stream: too short")
    cmf, flg = data[0], data[1]
    if (cmf & 0x0F) != 8:
        raise error(f"Invalid zlib CMF (not deflate): 0x{cmf:02x}")
    if (cmf * 256 + flg) % 31 != 0:
        raise error("Invalid zlib FCHECK")
    if flg & 0x20:  # FDICT set → preset dictionary required
        raise NotImplementedError("zlib preset dictionaries (zdict) not supported")
    body = data[2:-4]
    expected_adler = struct.unpack(">I", data[-4:])[0]
    return body, expected_adler


# --------------------------------------------------------------------------
# gzip (RFC 1952) wrapping
# --------------------------------------------------------------------------

_GZIP_MAGIC = b"\x1f\x8b"


def _gzip_wrap(raw, original_data, level):
    """Wrap raw DEFLATE bytes in the RFC 1952 gzip container."""
    if level == 9:    xfl = 2  # max compression
    elif level == 1: xfl = 4   # fastest
    else:            xfl = 0
    header = (
        _GZIP_MAGIC
        + bytes((DEFLATED, 0))   # CM=deflate, FLG=0
        + struct.pack("<I", 0)   # MTIME=0
        + bytes((xfl, 0xFF))     # XFL, OS=unknown
    )
    trailer = struct.pack("<II", crc32(original_data), len(original_data) & 0xFFFFFFFF)
    return header + raw + trailer


def _gzip_unwrap(data):
    """Strip RFC 1952 gzip container, returning (raw_deflate, crc32, isize)."""
    if len(data) < 18 or data[:2] != _GZIP_MAGIC:
        raise error("Not a gzip stream")
    if data[2] != DEFLATED:
        raise error(f"Unsupported gzip compression method: {data[2]}")
    flg = data[3]
    pos = 10
    # MTIME(4) XFL(1) OS(1) already consumed in the fixed 10-byte header
    if flg & 0x04:  # FEXTRA
        xlen = struct.unpack("<H", data[pos:pos+2])[0]
        pos += 2 + xlen
    if flg & 0x08:  # FNAME — NUL-terminated
        nul = data.index(0, pos); pos = nul + 1
    if flg & 0x10:  # FCOMMENT
        nul = data.index(0, pos); pos = nul + 1
    if flg & 0x02:  # FHCRC — 2-byte header CRC, ignore
        pos += 2
    body = data[pos:-8]
    expected_crc, expected_isize = struct.unpack("<II", data[-8:])
    return body, expected_crc, expected_isize


# --------------------------------------------------------------------------
# One-shot compress / decompress
# --------------------------------------------------------------------------

def compress(data, level=Z_DEFAULT_COMPRESSION, wbits=MAX_WBITS):
    """One-shot deflate. Wraps per wbits (zlib / raw / gzip)."""
    level = _normalize_level(level)
    mode, _win = _decode_wbits(wbits)
    if mode == "auto":
        raise ValueError("auto-detect wbits is decompress-only")
    raw = _compress_cap.deflate_compress(bytes(data), level)
    if mode == "raw":
        return raw
    if mode == "zlib":
        # zlib trailer is adler32 of the *original uncompressed* data.
        return _zlib_header(level) + raw + struct.pack(">I", adler32(data))
    if mode == "gzip":
        return _gzip_wrap(raw, data, level)
    raise AssertionError(f"unreachable mode {mode!r}")


def decompress(data, wbits=MAX_WBITS, bufsize=DEF_BUF_SIZE):
    """One-shot inflate. Unwraps per wbits (zlib / raw / gzip / auto)."""
    if not data:
        return b""
    mode, _win = _decode_wbits(wbits)
    if mode == "auto":
        # Sniff: gzip magic is 1F 8B; everything else assumed zlib.
        mode = "gzip" if data[:2] == _GZIP_MAGIC else "zlib"
    if mode == "raw":
        try:
            return _compress_cap.deflate_decompress(bytes(data))
        except RuntimeError as e:
            raise error(f"Error -3 while decompressing: {e}") from None
    if mode == "zlib":
        body, expected_adler = _zlib_unwrap(bytes(data))
        try:
            out = _compress_cap.deflate_decompress(body)
        except RuntimeError as e:
            raise error(f"Error -3 while decompressing zlib stream: {e}") from None
        if adler32(out) != expected_adler:
            raise error("Adler32 check failed")
        return out
    if mode == "gzip":
        body, expected_crc, expected_isize = _gzip_unwrap(bytes(data))
        try:
            out = _compress_cap.deflate_decompress(body)
        except RuntimeError as e:
            raise error(f"Error -3 while decompressing gzip stream: {e}") from None
        if crc32(out) != expected_crc:
            raise error("CRC check failed")
        if (len(out) & 0xFFFFFFFF) != expected_isize:
            raise error("Incorrect length")
        return out
    raise AssertionError(f"unreachable mode {mode!r}")


# --------------------------------------------------------------------------
# Streaming objects
# --------------------------------------------------------------------------

class _Compress:
    """Buffered-then-one-shot deflate object. Matches zlib.compressobj API."""

    def __init__(self, level=Z_DEFAULT_COMPRESSION, method=DEFLATED,
                 wbits=MAX_WBITS, memLevel=DEF_MEM_LEVEL,
                 strategy=Z_DEFAULT_STRATEGY, zdict=None):
        if method != DEFLATED:
            raise error(f"unsupported method: {method}")
        if zdict is not None:
            raise NotImplementedError("zlib preset dictionaries (zdict) not supported")
        # memLevel / strategy accepted but ignored.
        self._level = _normalize_level(level)
        self._mode, _win = _decode_wbits(wbits)
        if self._mode == "auto":
            raise ValueError("auto-detect wbits is decompress-only")
        self._buf = bytearray()
        self._closed = False

    def compress(self, data):
        if self._closed:
            raise error("Compress object has been flushed with Z_FINISH")
        self._buf.extend(data)
        return b""

    def flush(self, mode=Z_FINISH):
        if mode == Z_NO_FLUSH:
            return b""
        if self._closed:
            return b""
        # Any non-finish flush is degraded to "emit the buffered frame now".
        # The next .compress() will continue with a new sub-stream, which
        # won't be valid zlib if interleaved. For typical use (collect all,
        # then call flush(Z_FINISH)), the behavior matches stdlib.
        raw = _compress_cap.deflate_compress(bytes(self._buf), self._level)
        if mode == Z_FINISH:
            self._closed = True
        if self._mode == "raw":
            out = raw
        elif self._mode == "zlib":
            out = _zlib_header(self._level) + raw + struct.pack(">I", adler32(bytes(self._buf)))
        elif self._mode == "gzip":
            out = _gzip_wrap(raw, bytes(self._buf), self._level)
        else:
            raise AssertionError(f"unreachable mode {self._mode!r}")
        return out

    def copy(self):
        new = _Compress.__new__(_Compress)
        new._level = self._level
        new._mode = self._mode
        new._buf = bytearray(self._buf)
        new._closed = self._closed
        return new


class _Decompress:
    """Buffered-then-one-shot inflate object. Matches zlib.decompressobj API."""

    def __init__(self, wbits=MAX_WBITS, zdict=None):
        if zdict is not None:
            raise NotImplementedError("zlib preset dictionaries (zdict) not supported")
        self._mode, _win = _decode_wbits(wbits)
        self._buf = bytearray()
        self._out = b""
        self._eof = False
        self._unused = b""
        self._unconsumed = b""

    @property
    def eof(self):
        return self._eof

    @property
    def unused_data(self):
        return self._unused

    @property
    def unconsumed_tail(self):
        return self._unconsumed

    def _wbits(self):
        """Translate stored mode back into a zlib wbits value for the
        underlying one-shot decompress call."""
        return (-MAX_WBITS if self._mode == "raw"
                else MAX_WBITS if self._mode == "zlib"
                else MAX_WBITS | 16 if self._mode == "gzip"
                else MAX_WBITS | 32)  # auto

    def decompress(self, data, max_length=0):
        if self._eof:
            # Stream already finished. Any additional input becomes
            # unused_data (the gzip + multi-member compression cases
            # need this — they read trailers from unused_data).
            if data:
                self._unused += bytes(data)
            return b""
        self._buf.extend(data)
        try:
            full = decompress(bytes(self._buf), wbits=self._wbits())
        except error:
            # Likely truncated input — buffer for next call.
            return b""
        self._eof = True
        self._out = full
        # Find how many input bytes the deflate/zlib/gzip stream actually
        # consumed; the rest is `unused_data`. The capability's decompress
        # is one-shot and doesn't report consumed-bytes, so we binary-
        # search the smallest prefix of self._buf that decodes to the
        # same output. For typical gzip with an 8-byte trailer this is
        # ~log2(N) decompress calls — acceptable overhead vs. losing the
        # trailer entirely (which is what gzip.decompress reads to verify
        # CRC32 + length).
        consumed = _find_deflate_end(bytes(self._buf), self._wbits(), full)
        if consumed < len(self._buf):
            self._unused = bytes(self._buf[consumed:])
        if max_length and 0 < max_length < len(full):
            self._unconsumed = full[max_length:]
            return full[:max_length]
        return full

    def flush(self, length=DEF_BUF_SIZE):
        # Any pending unconsumed bytes get returned here.
        out = self._unconsumed
        self._unconsumed = b""
        return out

    def copy(self):
        new = _Decompress.__new__(_Decompress)
        new._mode = self._mode
        new._buf = bytearray(self._buf)
        new._out = self._out
        new._eof = self._eof
        new._unused = self._unused
        new._unconsumed = self._unconsumed
        return new


def _find_deflate_end(buf, wbits, expected_output):
    """Recover the deflate-stream-consumed byte count from `buf` given
    the known `expected_output`. Returns the prefix length (= where the
    stream ended and unused_data should start).

    Background: the underlying capability's `decompress` is one-shot and
    DOES NOT expose `total_in`. Worse, it's lenient about partial input:
    it returns whatever output it can produce even when the deflate
    end-of-block code isn't reached yet. Practical effect: binary-
    searching for the smallest prefix that produces the full output
    finds a prefix 0-1 bytes SHORTER than the true stream end (because
    the final byte may contain only Huffman-coded end-of-block bits,
    not output bits).

    Two-stage strategy:
      1. Binary search for the smallest prefix giving full output —
         this is a lower bound on the true stream length.
      2. CRC-validated trailer alignment: if the remaining bytes look
         like they could hold a gzip trailer (CRC32 + length), slide
         the boundary forward 0–4 bytes to find the position where
         `buf[pos:pos+4]` interpreted as little-endian uint32 equals
         crc32(expected_output). If found, that's the true stream end.

    The trailer alignment fixes the most common consumer (`gzip` and
    `gzip.decompress` for one-member streams). For raw-deflate streams
    with no trailer, binary search is fine — false positives are
    harmless since the consumer just sees a slightly-larger `unused_data`.

    A future cap revision exposing `total_in` would make this O(1) and
    eliminate the ambiguity entirely. Until then this is the best we
    can do at the Python layer."""
    # Stage 1: binary search for smallest-full-output prefix
    lo, hi = 1, len(buf)
    while lo < hi:
        mid = (lo + hi) // 2
        try:
            out = decompress(buf[:mid], wbits=wbits)
        except error:
            out = None
        if out == expected_output:
            hi = mid
        else:
            lo = mid + 1
    smallest_full = lo

    # Stage 2: gzip-trailer alignment (small window since the cap typically
    # under-reports by at most ~1 byte due to bit-level boundary alignment)
    remaining = len(buf) - smallest_full
    if remaining >= 8:
        # CRC32 of expected_output, little-endian (gzip trailer format)
        want_crc = _crc32_le(expected_output)
        for offset in range(min(remaining - 7, 5)):
            pos = smallest_full + offset
            if buf[pos:pos+4] == want_crc:
                # Validate length too — gzip trailer is CRC32 (4B) + length (4B)
                # length is uncompressed-output-length modulo 2^32
                want_len = (len(expected_output) & 0xFFFFFFFF).to_bytes(4, "little")
                if buf[pos+4:pos+8] == want_len:
                    return pos
    return smallest_full


def _crc32_le(data):
    """4-byte little-endian CRC32 (the encoding gzip writes in its trailer)."""
    return (_native_crc32(data) & 0xFFFFFFFF).to_bytes(4, "little")


# Pure-Python CRC32 — we can't recursively use our own zlib.crc32 here
# (would form a dependency cycle inside the shim). The compute is small
# enough to be O(n) with a precomputed table.
def _make_crc32_table():
    table = []
    for n in range(256):
        c = n
        for _ in range(8):
            c = (c >> 1) ^ (0xedb88320 if c & 1 else 0)
        table.append(c)
    return table


_CRC32_TABLE = _make_crc32_table()


def _native_crc32(data):
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC32_TABLE[(c ^ b) & 0xff] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def compressobj(level=Z_DEFAULT_COMPRESSION, method=DEFLATED, wbits=MAX_WBITS,
                memLevel=DEF_MEM_LEVEL, strategy=Z_DEFAULT_STRATEGY, zdict=None):
    return _Compress(level, method, wbits, memLevel, strategy, zdict)


def decompressobj(wbits=MAX_WBITS, zdict=None):
    return _Decompress(wbits, zdict)


class _ZlibDecompressor:
    """Streaming decompressor matching stdlib `zlib._ZlibDecompressor`.

    CPython's stdlib gzip module instantiates this via
    `compression._streams.DecompressReader(_PaddedFile(fp),
    zlib._ZlibDecompressor, wbits=-zlib.MAX_WBITS)`. The reader feeds
    file chunks to `.decompress(chunk, max_length=...)` and reads
    `.eof` / `.needs_input` / `.unused_data` between calls.

    Wraps `_Decompress` (which already accumulates input until the
    one-shot capability decompress succeeds), surfacing the stdlib's
    `needs_input` attribute. eof, unused_data, decompress(data,
    max_length=-1) semantics match `zlib._ZlibDecompressor` so the
    gzip module's incremental usage works."""

    def __init__(self, wbits=MAX_WBITS, zdict=None):
        self._inner = _Decompress(wbits=wbits, zdict=zdict)
        self._needs_input = True

    @property
    def eof(self):
        return self._inner.eof

    @property
    def needs_input(self):
        return self._needs_input

    @property
    def unused_data(self):
        return self._inner.unused_data

    def decompress(self, data, max_length=-1):
        # Stdlib uses max_length=-1 to mean "unlimited"; _Decompress
        # uses 0. Translate.
        if max_length == -1:
            max_length = 0
        out = self._inner.decompress(data, max_length=max_length)
        # needs_input becomes False when we have leftover output to
        # consume (max_length truncated and there's unconsumed_tail).
        self._needs_input = not self._inner.unconsumed_tail
        return out

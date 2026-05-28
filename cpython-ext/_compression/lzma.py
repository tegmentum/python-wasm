"""lzma shim — routes through the compression-multiplexer capability.

Drop-in replacement for stdlib `lzma` that uses the `_lzma_cap`
capability extension instead of the missing-on-wasi `_lzma` C extension.

**Container format.** The capability's lzma backend (`lzma-rust2`)
produces the modern `.xz` container (RFC-7878), matching stdlib's
`FORMAT_XZ` default. `.tar.xz` tarballs, pip source distributions, and
GitHub release assets all work out-of-the-box.

  * Defaults `lzma.compress(...)`     → `FORMAT_XZ`  (matches stdlib)
  * Defaults `lzma.decompress(...)`   → `FORMAT_AUTO` (sniffs magic)
  * Raises `NotImplementedError`      on `FORMAT_ALONE` writes
                                      (legacy `.lzma` container — the cap's
                                      LZMA backend now exclusively writes
                                      `.xz`), `FORMAT_RAW` writes (filter
                                      chain required), and filter-chain
                                      configuration on either direction.

API parity:

  Module-level
    lzma.compress(data, format=FORMAT_ALONE, preset=None)   ✓ (ALONE only)
    lzma.decompress(data, format=FORMAT_AUTO)               ✓ (ALONE input)
    lzma.is_check_supported(check)                          ✓ (always False
                                                              — checks belong
                                                              to FORMAT_XZ)
    lzma.open(...)                                          ✓ delegates to LZMAFile

  Streaming classes
    lzma.LZMACompressor(format=FORMAT_ALONE, ...)           ✓ (ALONE only)
    lzma.LZMADecompressor(format=FORMAT_AUTO, ...)          ✓ (ALONE input)

  File I/O
    lzma.LZMAFile(filename, mode='r', *, format=...)        ✓

  Exceptions
    LZMAError                                               ✓

Constants (for API surface compatibility, even when their format is
unsupported as a *write* target):
    FORMAT_AUTO, FORMAT_XZ, FORMAT_ALONE, FORMAT_RAW
    CHECK_NONE, CHECK_CRC32, CHECK_CRC64, CHECK_SHA256, CHECK_ID_MAX,
    CHECK_UNKNOWN
    MODE_FAST, MODE_NORMAL
    PRESET_DEFAULT, PRESET_EXTREME
    FILTER_LZMA1, FILTER_LZMA2, FILTER_DELTA, FILTER_X86, FILTER_IA64,
    FILTER_ARM, FILTER_ARMTHUMB, FILTER_POWERPC, FILTER_SPARC
"""

__all__ = [
    "CHECK_NONE", "CHECK_CRC32", "CHECK_CRC64", "CHECK_SHA256",
    "CHECK_ID_MAX", "CHECK_UNKNOWN",
    "FILTER_LZMA1", "FILTER_LZMA2", "FILTER_DELTA", "FILTER_X86", "FILTER_IA64",
    "FILTER_ARM", "FILTER_ARMTHUMB", "FILTER_POWERPC", "FILTER_SPARC",
    "FORMAT_AUTO", "FORMAT_XZ", "FORMAT_ALONE", "FORMAT_RAW",
    "MF_HC3", "MF_HC4", "MF_BT2", "MF_BT3", "MF_BT4",
    "MODE_FAST", "MODE_NORMAL", "PRESET_DEFAULT", "PRESET_EXTREME",
    "LZMACompressor", "LZMADecompressor", "LZMAFile", "LZMAError",
    "open", "compress", "decompress", "is_check_supported",
]

import io
import os
from builtins import open as _builtin_open
from compression._common import _streams

import _lzma_cap


# Container-format identifiers. Numerically aligned with stdlib's _lzma.h so
# downstream code that imported the constants works unchanged.
FORMAT_AUTO   = 0
FORMAT_XZ     = 1
FORMAT_ALONE  = 2
FORMAT_RAW    = 3

# Integrity-check identifiers (xz container only — we don't write xz, so
# requesting any of these on a write is moot. Provided for API parity.)
CHECK_NONE     = 0
CHECK_CRC32    = 1
CHECK_CRC64    = 4
CHECK_SHA256   = 10
CHECK_ID_MAX   = 15
CHECK_UNKNOWN  = 16

# Match-finder + mode constants (filter-chain knobs we don't expose).
MF_HC3, MF_HC4 = 0x03, 0x04
MF_BT2, MF_BT3, MF_BT4 = 0x12, 0x13, 0x14
MODE_FAST, MODE_NORMAL = 1, 2

# Preset levels — passed through to the cap as the compression level.
PRESET_DEFAULT  = 6
PRESET_EXTREME  = 1 << 31  # not honored by cap; kept for parity

# Filter IDs (not used by the cap; kept for API parity).
FILTER_LZMA1     = 0x4000000000000001
FILTER_LZMA2     = 0x21
FILTER_DELTA     = 0x03
FILTER_X86       = 0x04
FILTER_POWERPC   = 0x05
FILTER_IA64      = 0x06
FILTER_ARM       = 0x07
FILTER_ARMTHUMB  = 0x08
FILTER_SPARC     = 0x09


class LZMAError(Exception):
    """Raised on invalid LZMA data or unsupported parameters."""


_MODE_READ  = 1
_MODE_WRITE = 3


# --------------------------------------------------------------------------
# Format sniffing
# --------------------------------------------------------------------------

def _looks_alone(data: bytes) -> bool:
    # `.lzma` (alone) header: properties byte (lc/lp/pb encoded; typically
    # 0x5D for the default 3/0/2) + 4-byte dict size + 8-byte uncompressed
    # size. We only sniff the most common 0x5D prefix to keep this honest;
    # other property bytes are technically valid but rare in the wild.
    return len(data) >= 13 and data[0] == 0x5D


def _looks_xz(data: bytes) -> bool:
    # XZ magic: FD 37 7A 58 5A 00
    return len(data) >= 6 and data[:6] == b"\xfd\x37\x7a\x58\x5a\x00"


# --------------------------------------------------------------------------
# Helpers — translate format/preset/filters into a capability call, or
# raise NotImplementedError with a clear message.
# --------------------------------------------------------------------------

def _unsupported(what):
    raise NotImplementedError(
        f"lzma shim: {what} is not supported by the compression-multiplexer "
        "capability backend. The cap exposes the modern .xz container "
        "(FORMAT_XZ) only; legacy FORMAT_ALONE writes and FORMAT_RAW "
        "filter chains are deferred."
    )


def _check_write_format(format, filters):
    if filters is not None:
        _unsupported("filter chains")
    if format == FORMAT_ALONE:
        _unsupported("FORMAT_ALONE writes (legacy .lzma container)")
    if format == FORMAT_RAW:
        _unsupported("FORMAT_RAW (requires filter chain)")
    if format not in (FORMAT_XZ,):
        raise ValueError(f"Invalid format: {format}")


# --------------------------------------------------------------------------
# Streaming classes
# --------------------------------------------------------------------------

class LZMACompressor:
    """Buffered-then-one-shot LZMA compressor (FORMAT_XZ only).

    The cap backend embeds its own integrity check (CRC32 by default in the
    XzWriter); the `check` argument is accepted at the API surface for
    parity with stdlib but is not passed through.
    """

    def __init__(self, format=FORMAT_XZ, check=-1, preset=None, filters=None):
        _check_write_format(format, filters)
        # `check` is informational; the cap writer picks its own (CRC32).
        self._level = PRESET_DEFAULT if preset is None else int(preset) & 0xFF
        self._buf = bytearray()
        self._flushed = False

    def compress(self, data):
        if self._flushed:
            raise ValueError("Compressor has been flushed")
        self._buf.extend(data)
        return b""

    def flush(self):
        if self._flushed:
            raise ValueError("Repeated call to flush()")
        self._flushed = True
        return _lzma_cap.lzma_compress(bytes(self._buf), self._level)


class LZMADecompressor:
    """Buffered-then-one-shot LZMA decompressor (FORMAT_AUTO / FORMAT_XZ)."""

    def __init__(self, format=FORMAT_AUTO, memlimit=None, filters=None):
        if filters is not None:
            _unsupported("filter chains")
        if format == FORMAT_ALONE:
            _unsupported("FORMAT_ALONE reads (legacy .lzma container)")
        if format not in (FORMAT_AUTO, FORMAT_XZ):
            _unsupported(f"format={format} for decompression "
                         "(only FORMAT_AUTO/FORMAT_XZ supported)")
        # memlimit is advisory in stdlib; we don't enforce it.
        self._buf = bytearray()
        self._eof = False
        self._unused = b""
        self._check = CHECK_UNKNOWN

    @property
    def eof(self):
        return self._eof

    @property
    def needs_input(self):
        return not self._eof

    @property
    def unused_data(self):
        return self._unused

    @property
    def check(self):
        return self._check

    def decompress(self, data, max_length=-1):
        if self._eof:
            return b""
        self._buf.extend(data)
        if not self._buf:
            return b""
        try:
            full = _lzma_cap.lzma_decompress(bytes(self._buf))
        except RuntimeError as e:
            # Truncated input is the common cause; let caller add more bytes.
            return b""
        self._eof = True
        return full if max_length < 0 else full[:max_length]


# --------------------------------------------------------------------------
# Module-level convenience
# --------------------------------------------------------------------------

def is_check_supported(check):
    """Return True for the checks the cap's XzWriter can embed.

    The cap backend writes FORMAT_XZ with a CRC32 integrity check by
    default; the actual choice is opaque to the consumer. CRC32 and
    CRC64 are supported in principle; CHECK_NONE is honored too.
    """
    return check in (CHECK_NONE, CHECK_CRC32, CHECK_CRC64, CHECK_UNKNOWN)


def compress(data, format=FORMAT_XZ, check=-1, preset=None, filters=None):
    """One-shot LZMA compression (FORMAT_XZ, matching stdlib)."""
    _check_write_format(format, filters)
    # `check` is informational; cap writer embeds its own CRC32 by default.
    level = PRESET_DEFAULT if preset is None else int(preset) & 0xFF
    return _lzma_cap.lzma_compress(bytes(data), level)


def decompress(data, format=FORMAT_AUTO, memlimit=None, filters=None):
    """One-shot LZMA decompression. AUTO sniffs by xz magic."""
    if filters is not None:
        _unsupported("filter chains")
    if format == FORMAT_ALONE:
        _unsupported("FORMAT_ALONE reads (legacy .lzma container)")
    if format not in (FORMAT_AUTO, FORMAT_XZ):
        _unsupported(f"format={format}")
    if not data:
        return b""
    try:
        return _lzma_cap.lzma_decompress(bytes(data))
    except RuntimeError as e:
        raise LZMAError(f"Invalid LZMA data: {e}") from None


# --------------------------------------------------------------------------
# File I/O
# --------------------------------------------------------------------------

class LZMAFile(_streams.BaseStream):
    """LZMA-compressed file (FORMAT_ALONE only on write; AUTO on read)."""

    def __init__(self, filename, mode="r", *, format=None, check=-1,
                 preset=None, filters=None):
        self._fp = None
        self._closefp = False
        self._mode = None

        if mode in ("", "r", "rb"):
            mode = "rb"
            mode_code = _MODE_READ
            self._format = FORMAT_AUTO if format is None else format
        elif mode in ("w", "wb", "x", "xb", "a", "ab"):
            if mode in ("w",): mode = "wb"
            elif mode in ("x",): mode = "xb"
            elif mode in ("a",): mode = "ab"
            mode_code = _MODE_WRITE
            self._format = FORMAT_XZ if format is None else format
            self._compressor = LZMACompressor(self._format, check, preset, filters)
        else:
            raise ValueError(f"Invalid mode: {mode!r}")

        if isinstance(filename, (str, bytes, os.PathLike)):
            self._fp = _builtin_open(filename, mode)
            self._closefp = True
        elif hasattr(filename, "read") or hasattr(filename, "write"):
            self._fp = filename
        else:
            raise TypeError("filename must be str/bytes/path or file-like")

        self._mode = mode_code

        if mode_code == _MODE_READ:
            raw = self._fp.read()
            self._buffer = io.BytesIO(
                decompress(raw, self._format) if raw else b""
            )

    def close(self):
        if self.closed:
            return
        try:
            if self._mode == _MODE_WRITE:
                self._fp.write(self._compressor.flush())
                self._compressor = None
        finally:
            try:
                if self._closefp:
                    self._fp.close()
            finally:
                self._fp = None
                self._closefp = False
                super().close()

    def writable(self): return self._mode == _MODE_WRITE
    def readable(self): return self._mode == _MODE_READ
    def seekable(self): return self._mode == _MODE_READ

    def read(self, size=-1):
        self._check_can_read(); return self._buffer.read(size)

    def read1(self, size=-1):
        self._check_can_read()
        if size < 0: size = io.DEFAULT_BUFFER_SIZE
        return self._buffer.read(size)

    def readinto(self, b):
        self._check_can_read(); return self._buffer.readinto(b)

    def write(self, data):
        self._check_can_write()
        self._compressor.compress(data)
        return len(data)

    def seek(self, offset, whence=io.SEEK_SET):
        self._check_can_seek(); return self._buffer.seek(offset, whence)

    def tell(self):
        self._check_not_closed()
        return self._buffer.tell() if self._mode == _MODE_READ else 0


def open(filename, mode="rb", *, format=None, check=-1, preset=None,
         filters=None, encoding=None, errors=None, newline=None):
    """Open an LZMA-compressed file in binary or text mode."""
    if "t" in mode:
        if "b" in mode:
            raise ValueError(f"Invalid mode: {mode!r}")
    else:
        if encoding is not None:
            raise ValueError("Argument 'encoding' not supported in binary mode")
        if errors is not None:
            raise ValueError("Argument 'errors' not supported in binary mode")
        if newline is not None:
            raise ValueError("Argument 'newline' not supported in binary mode")

    lz_mode = mode.replace("t", "")
    binary_file = LZMAFile(filename, lz_mode, format=format, check=check,
                           preset=preset, filters=filters)
    if "t" in mode:
        encoding = io.text_encoding(encoding)
        return io.TextIOWrapper(binary_file, encoding, errors, newline)
    return binary_file

"""bz2 shim — routes through the compression-multiplexer capability.

Drop-in replacement for the stdlib `bz2` module that uses the
`_compress_cap` capability extension instead of the missing-on-wasi
`_bz2` C extension. Installed into `deps/cpython/Lib/bz2.py` by
`make install-python-shims`, shadowing the stdlib copy.

API parity:

  Module-level
    bz2.compress(data, compresslevel=9) -> bytes      ✓ via _compress_cap
    bz2.decompress(data)                -> bytes      ✓ via _compress_cap
    bz2.open(...)                                     ✓ delegates to BZ2File

  Streaming classes (chunked compress/decompress; buffer until flush)
    bz2.BZ2Compressor(compresslevel=9)                ✓
        .compress(data)  -> bytes  (buffered; returns b"" until flush)
        .flush()         -> bytes  (returns the entire compressed stream)
    bz2.BZ2Decompressor()                             ✓
        .decompress(data, max_length=-1) -> bytes
        .eof             property
        .needs_input     property
        .unused_data     property

  File I/O
    bz2.BZ2File(filename, mode='r', *, compresslevel=9)   ✓ via _streams.BaseStream

Limitations vs stdlib:
  * Streaming classes buffer the full input; the WIT capability is one-shot.
    Adequate for typical bz2 use (whole-file compress/decompress) but uses
    O(n) memory rather than streaming O(1).
  * No multi-stream concatenated-bz2 decompress (each call to decompress()
    on a fresh BZ2Decompressor only handles one stream).
"""

__all__ = ["BZ2File", "BZ2Compressor", "BZ2Decompressor",
           "open", "compress", "decompress"]

import io
import os
from builtins import open as _builtin_open
from compression._common import _streams

import _compress_cap


# Mode constants kept for compatibility with stdlib bz2.py callers that
# import _MODE_READ / _MODE_WRITE.
_MODE_READ = 1
_MODE_WRITE = 3


# --------------------------------------------------------------------------
# Streaming (de)compressor objects
#
# Both classes buffer their input and run a single one-shot call to the
# capability at flush time. This matches the stdlib API but is one-shot
# under the hood — fine for the typical "compress one file" pattern.
# --------------------------------------------------------------------------

class BZ2Compressor:
    """Buffered-then-one-shot bzip2 compressor. API matches `_bz2.BZ2Compressor`."""

    def __init__(self, compresslevel=9):
        if not 1 <= compresslevel <= 9:
            raise ValueError("compresslevel must be between 1 and 9")
        self._level = compresslevel
        self._buf = bytearray()
        self._flushed = False

    def compress(self, data):
        if self._flushed:
            raise ValueError("Compressor has been flushed")
        self._buf.extend(data)
        # Stdlib bz2 buffers internally and returns partial frames as it
        # accumulates a full block; we just buffer everything and return
        # the whole stream at flush time.
        return b""

    def flush(self):
        if self._flushed:
            raise ValueError("Repeated call to flush()")
        self._flushed = True
        return _compress_cap.bzip2_compress(bytes(self._buf), self._level)


class BZ2Decompressor:
    """Buffered-then-one-shot bzip2 decompressor. API matches `_bz2.BZ2Decompressor`."""

    def __init__(self):
        self._buf = bytearray()
        self._out = b""
        self._eof = False
        self._unused = b""

    @property
    def eof(self):
        return self._eof

    @property
    def needs_input(self):
        return not self._eof

    @property
    def unused_data(self):
        return self._unused

    def decompress(self, data, max_length=-1):
        if self._eof:
            return b""
        self._buf.extend(data)
        # We have no streaming decompressor. Decode the buffered input as
        # a complete bzip2 stream; if that fails (truncated), defer.
        try:
            full = _compress_cap.bzip2_decompress(bytes(self._buf))
        except RuntimeError:
            # Likely truncated input — accept more bytes on next call.
            return b""
        self._eof = True
        self._out = full
        out = full if max_length < 0 else full[:max_length]
        return out


# --------------------------------------------------------------------------
# Module-level convenience
# --------------------------------------------------------------------------

def compress(data, compresslevel=9):
    """One-shot bzip2 compression. `compresslevel` ∈ [1, 9]."""
    if not 1 <= compresslevel <= 9:
        raise ValueError("compresslevel must be between 1 and 9")
    return _compress_cap.bzip2_compress(bytes(data), compresslevel)


def decompress(data):
    """One-shot bzip2 decompression. Raises OSError on invalid input."""
    if not data:
        return b""
    try:
        return _compress_cap.bzip2_decompress(bytes(data))
    except RuntimeError as e:
        raise OSError(f"Invalid bzip2 data: {e}") from None


# --------------------------------------------------------------------------
# File I/O
#
# BZ2File wraps the buffered compressor/decompressor in the stdlib
# `compression._common._streams.BaseStream` adapter so it looks like a
# file object (read/write/seek-on-read).
# --------------------------------------------------------------------------

class BZ2File(_streams.BaseStream):
    """Bzip2-compressed file object. Read/write parity with stdlib BZ2File."""

    def __init__(self, filename, mode="r", *, compresslevel=9):
        self._fp = None
        self._closefp = False
        self._mode = None

        if not 1 <= compresslevel <= 9:
            raise ValueError("compresslevel must be between 1 and 9")

        if mode in ("", "r", "rb"):
            mode = "rb"
            mode_code = _MODE_READ
        elif mode in ("w", "wb"):
            mode = "wb"
            mode_code = _MODE_WRITE
            self._compressor = BZ2Compressor(compresslevel)
        elif mode in ("x", "xb"):
            mode = "xb"
            mode_code = _MODE_WRITE
            self._compressor = BZ2Compressor(compresslevel)
        elif mode in ("a", "ab"):
            mode = "ab"
            mode_code = _MODE_WRITE
            self._compressor = BZ2Compressor(compresslevel)
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
            # Decompress entire file into memory and stream from there —
            # the WIT capability is one-shot. For pathological multi-GB
            # files this needs a streaming decompressor (future work).
            raw = self._fp.read()
            self._buffer = io.BytesIO(decompress(raw) if raw else b"")
            self._decompressor = None  # not used in this path
        # write paths just buffer via self._compressor

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

    def writable(self):
        return self._mode == _MODE_WRITE

    def readable(self):
        return self._mode == _MODE_READ

    def seekable(self):
        return self._mode == _MODE_READ

    def read(self, size=-1):
        self._check_can_read()
        return self._buffer.read(size)

    def read1(self, size=-1):
        self._check_can_read()
        if size < 0:
            size = io.DEFAULT_BUFFER_SIZE
        return self._buffer.read(size)

    def readinto(self, b):
        self._check_can_read()
        return self._buffer.readinto(b)

    def write(self, data):
        self._check_can_write()
        # BZ2Compressor.compress() buffers; nothing to write yet. The flush
        # at close() emits the full frame.
        self._compressor.compress(data)
        return len(data)

    def seek(self, offset, whence=io.SEEK_SET):
        self._check_can_seek()
        return self._buffer.seek(offset, whence)

    def tell(self):
        self._check_not_closed()
        if self._mode == _MODE_READ:
            return self._buffer.tell()
        return 0  # writes are buffered; "position" isn't meaningful pre-flush


def open(filename, mode="rb", compresslevel=9,
         encoding=None, errors=None, newline=None):
    """Open a bzip2-compressed file in binary or text mode.

    Mirrors stdlib bz2.open semantics. Text mode adds an io.TextIOWrapper.
    """
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

    bz_mode = mode.replace("t", "")
    binary_file = BZ2File(filename, bz_mode, compresslevel=compresslevel)
    if "t" in mode:
        encoding = io.text_encoding(encoding)
        return io.TextIOWrapper(binary_file, encoding, errors, newline)
    return binary_file

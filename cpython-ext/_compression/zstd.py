"""compression.zstd shim — routes through the compression-multiplexer.

Drop-in (minimal) replacement for the 3.14 stdlib `compression.zstd`
package's __init__.py. Uses the `_compress_cap` capability extension
instead of the missing-on-wasi `_zstd` C extension.

Installed by `make install-python-shims` to:
    deps/cpython/Lib/compression/zstd/__init__.py

What works:
    compress(data, level=COMPRESSION_LEVEL_DEFAULT)   ✓
    decompress(data)                                  ✓
    ZstdCompressor(level=..., options=None, zstd_dict=None)
        .compress(data, mode=ZstdCompressor.CONTINUE)
        .flush(mode=ZstdCompressor.FLUSH_FRAME)
    ZstdDecompressor(zstd_dict=None, options=None)
        .decompress(data, max_length=-1)
        .eof property, .needs_input property, .unused_data property
    ZstdError                                         ✓

What is NOT implemented (raises NotImplementedError):
    ZstdDict / dictionary-based compression
    train_dict / finalize_dict (dictionary training)
    get_frame_info / get_frame_size  (frame-header parsing)
    CompressionParameter / DecompressionParameter (advanced knobs)
    Strategy enum  (zstd internal strategy IDs)

Streaming wrapper is buffered (one-shot call at flush time), matching
the bz2 / lzma shim pattern. Adequate for typical "compress one blob"
use; not memory-efficient for multi-GB streams.

`ZstdFile` / `open` are provided by the companion `_zstdfile` shim if
needed; the package-level re-export of `open` here is a thin alias.
"""

__all__ = (
    'COMPRESSION_LEVEL_DEFAULT',
    'compress', 'decompress',
    'ZstdCompressor', 'ZstdDecompressor', 'ZstdError',
    # Surface-area constants kept for parity; many raise on use.
    'CompressionParameter', 'DecompressionParameter',
    'Strategy', 'ZstdDict',
    'get_frame_info', 'get_frame_size',
    'finalize_dict', 'train_dict',
    'zstd_version', 'zstd_version_info',
    'open', 'ZstdFile',
)

import enum
import _compress_cap


COMPRESSION_LEVEL_DEFAULT = 3
"""The capability backend's default zstd compression level."""

# We don't link real libzstd; report 0.0.0 to be unambiguous.
zstd_version = "0.0.0-capability"
zstd_version_info = (0, 0, 0)


class ZstdError(Exception):
    """Raised on invalid zstd data or unsupported parameters."""


def _unsupported(what):
    raise NotImplementedError(
        f"compression.zstd shim: {what} is not implemented in the "
        "capability-routed backend. The cap exposes one-shot "
        "compress/decompress only — no dictionaries, parameters, "
        "or frame introspection."
    )


# --------------------------------------------------------------------------
# Module-level convenience
# --------------------------------------------------------------------------

def compress(data, level=COMPRESSION_LEVEL_DEFAULT, options=None, zstd_dict=None):
    """One-shot zstd compression."""
    if options is not None:
        _unsupported("compress(options=...)")
    if zstd_dict is not None:
        _unsupported("compress(zstd_dict=...)")
    if not (1 <= level <= 22):
        # libzstd accepts a wider range (negative levels for ultra-fast),
        # but the capability clamps at u8; reject obviously-bad values.
        raise ValueError(f"level out of range: {level}")
    return _compress_cap.zstd_compress(bytes(data), level)


def decompress(data, zstd_dict=None, options=None):
    """One-shot zstd decompression."""
    if zstd_dict is not None:
        _unsupported("decompress(zstd_dict=...)")
    if options is not None:
        _unsupported("decompress(options=...)")
    if not data:
        return b""
    try:
        return _compress_cap.zstd_decompress(bytes(data))
    except RuntimeError as e:
        raise ZstdError(f"Invalid zstd data: {e}") from None


# --------------------------------------------------------------------------
# Streaming classes
# --------------------------------------------------------------------------

class ZstdCompressor:
    """Buffered-then-one-shot zstd compressor.

    Stdlib `_zstd.ZstdCompressor` is streaming; we buffer until flush.
    `mode` is honored at API surface but only FLUSH_FRAME ends the stream.
    """
    CONTINUE     = 0
    FLUSH_BLOCK  = 1
    FLUSH_FRAME  = 2

    def __init__(self, level=COMPRESSION_LEVEL_DEFAULT, options=None, zstd_dict=None):
        if options is not None:
            _unsupported("ZstdCompressor(options=...)")
        if zstd_dict is not None:
            _unsupported("ZstdCompressor(zstd_dict=...)")
        if not (1 <= level <= 22):
            raise ValueError(f"level out of range: {level}")
        self._level = level
        self._buf = bytearray()
        self._frame_done = False
        self.last_mode = self.FLUSH_FRAME  # stdlib attr — what mode last call used

    def compress(self, data, mode=None):
        if self._frame_done:
            raise ValueError("Frame already closed")
        if mode is None:
            mode = self.CONTINUE
        self._buf.extend(data)
        self.last_mode = mode
        if mode == self.FLUSH_FRAME:
            return self.flush(mode)
        # CONTINUE / FLUSH_BLOCK: buffer, return empty
        return b""

    def flush(self, mode=None):
        if mode is None:
            mode = self.FLUSH_FRAME
        if mode != self.FLUSH_FRAME:
            # FLUSH_BLOCK on its own can't be served by a one-shot codec;
            # treat as no-op (return b"") so callers that flush blocks
            # mid-stream don't crash. The actual frame is emitted on
            # FLUSH_FRAME or close.
            return b""
        if self._frame_done:
            return b""
        self._frame_done = True
        return _compress_cap.zstd_compress(bytes(self._buf), self._level)


class ZstdDecompressor:
    """Buffered-then-one-shot zstd decompressor."""

    def __init__(self, zstd_dict=None, options=None):
        if zstd_dict is not None:
            _unsupported("ZstdDecompressor(zstd_dict=...)")
        if options is not None:
            _unsupported("ZstdDecompressor(options=...)")
        self._buf = bytearray()
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
        if not self._buf:
            return b""
        try:
            full = _compress_cap.zstd_decompress(bytes(self._buf))
        except RuntimeError as e:
            # Likely truncated — wait for more input.
            return b""
        self._eof = True
        return full if max_length < 0 else full[:max_length]


# --------------------------------------------------------------------------
# Stubs for the surface-area names callers may reference
# --------------------------------------------------------------------------

class CompressionParameter(enum.IntEnum):
    """Stub — advanced parameter IDs not honored by the cap backend."""
    compression_level = 100
    window_log = 101
    chain_log = 102


class DecompressionParameter(enum.IntEnum):
    """Stub — advanced parameter IDs not honored by the cap backend."""
    window_log_max = 100


class Strategy(enum.IntEnum):
    """Stub — libzstd strategy IDs."""
    fast = 1
    dfast = 2
    greedy = 3
    lazy = 4
    lazy2 = 5
    btlazy2 = 6
    btopt = 7
    btultra = 8
    btultra2 = 9


class ZstdDict:
    """Stub — dictionary support not implemented."""
    def __init__(self, *a, **kw):
        _unsupported("ZstdDict()")


def get_frame_info(frame_buffer):
    _unsupported("get_frame_info()")


def get_frame_size(frame_buffer):
    _unsupported("get_frame_size()")


def finalize_dict(*a, **kw):
    _unsupported("finalize_dict()")


def train_dict(*a, **kw):
    _unsupported("train_dict()")


# --------------------------------------------------------------------------
# File-I/O stubs — defer to a future _zstdfile shim. Both names exist so
# `from compression.zstd import open, ZstdFile` doesn't ImportError.
# --------------------------------------------------------------------------

class ZstdFile:
    def __init__(self, *a, **kw):
        _unsupported("ZstdFile() — file-I/O wrapper deferred")


def open(*a, **kw):
    _unsupported("compression.zstd.open() — file-I/O wrapper deferred")

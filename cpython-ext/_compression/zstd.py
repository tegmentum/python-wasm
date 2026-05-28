"""compression.zstd shim — routes through the compression-multiplexer.

Drop-in replacement for the 3.14 stdlib `compression.zstd` package's
__init__.py. Uses `_compress_cap` (capability extension) for the codec
and for dictionary-aware compress/decompress + dict training; uses
pure-Python frame-header parsing for `get_frame_info` (no cap roundtrip
needed — the zstd frame header is documented and small).

Installed by `make install-python-shims` to:
    deps/cpython/Lib/compression/zstd/__init__.py

What works:
    compress(data, level=COMPRESSION_LEVEL_DEFAULT, options=None, zstd_dict=None)
    decompress(data, zstd_dict=None, options=None)
    ZstdCompressor(level=..., options=None, zstd_dict=None)
        .compress(data, mode=ZstdCompressor.CONTINUE)
        .flush(mode=ZstdCompressor.FLUSH_FRAME)
    ZstdDecompressor(zstd_dict=None, options=None)
        .decompress(data, max_length=-1)
        .eof / .needs_input / .unused_data properties
    ZstdDict(dict_content, *, is_raw=False)            ✓ (was: stub)
        .dict_id   property
        .dict_content property
    train_dict(samples, dict_size)                     ✓ (was: stub)
    get_frame_info(frame_buffer) -> FrameInfo          ✓ (was: stub)
    ZstdError                                          ✓

Full stdlib API coverage — every entry point routes through the cap:
    finalize_dict, get_frame_size, get_frame_info
    CompressionParameter / DecompressionParameter / Strategy enums with the
    canonical libzstd ZSTD_c_* / ZSTD_d_* / ZSTD_strategy values; pass via
    `compress(data, options={...})` / `decompress(data, options={...})` and
    `ZstdCompressor(options={...})` / `ZstdDecompressor(options={...})`.
"""

__all__ = (
    'COMPRESSION_LEVEL_DEFAULT',
    'compress', 'decompress',
    'ZstdCompressor', 'ZstdDecompressor', 'ZstdError', 'ZstdDict',
    'FrameInfo', 'get_frame_info',
    'train_dict', 'finalize_dict',
    'get_frame_size',
    # Surface-area constants kept for parity; some raise on use.
    'CompressionParameter', 'DecompressionParameter', 'Strategy',
    'zstd_version', 'zstd_version_info',
    'open', 'ZstdFile',
)

import enum
import io
import os
import struct
from builtins import open as _builtin_open
from compression._common import _streams

import _compress_cap  # advanced/dict ops until _zstd_cap implements them
import _zstd_cap      # basic compress/decompress via zstd-wasm directly


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
        "capability-routed backend. Would need a multiplexer-side change "
        "to extend the `zstd-extras` WIT interface."
    )


# --------------------------------------------------------------------------
# Dictionary support
# --------------------------------------------------------------------------

class ZstdDict:
    """A zstd dictionary, wrapping raw bytes.

    The bytes are passed through the cap on every compress/decompress call;
    we don't keep a long-lived cap-side handle (would require exposing the
    WIT resource as a Python type). For typical "use dict for one batch"
    workloads this is fine; for very hot loops, caching is on the roadmap.

    `is_raw=True` says "treat this as raw content bytes, not a real dict
    with the libzstd header" — the cap impl falls back to raw-content mode
    automatically (dict_id() returns 0 for such dicts).
    """

    def __init__(self, dict_content, *, is_raw=False):
        if not isinstance(dict_content, (bytes, bytearray, memoryview)):
            raise TypeError("dict_content must be bytes-like")
        self._bytes = bytes(dict_content)
        self._is_raw = bool(is_raw)
        # Cache the dict ID (cheap; only requires header parse).
        self._dict_id = _compress_cap.zstd_dict_id(self._bytes) if not is_raw else 0

    @property
    def dict_content(self):
        return self._bytes

    @property
    def dict_id(self):
        return self._dict_id

    def __repr__(self):
        return f"<ZstdDict id={self._dict_id} size={len(self._bytes)}>"


def train_dict(samples, dict_size):
    """Train a zstd dictionary from `samples` (iterable of bytes).

    Returns a `ZstdDict`. `dict_size` is the target dictionary size in
    bytes (typical: 16 KB to 110 KB). Provide at least 10-100x dict_size
    worth of total sample data for a quality dictionary.
    """
    raw = _compress_cap.zstd_train_dict(samples, int(dict_size))
    return ZstdDict(raw)


def finalize_dict(zstd_dict, samples, dict_size, level):
    """Refine an existing dict's content using sample statistics.

    `zstd_dict` is the input dictionary (its `dict_content` is the seed);
    `samples` is an iterable of representative bytes; `dict_size` is the
    target output size; `level` is the compression level to tune for.
    Returns a new `ZstdDict`.
    """
    if not isinstance(zstd_dict, ZstdDict):
        raise TypeError("zstd_dict must be a ZstdDict")
    raw = _compress_cap.zstd_finalize_dict(
        zstd_dict.dict_content, samples, int(dict_size), int(level))
    return ZstdDict(raw)


# --------------------------------------------------------------------------
# Module-level convenience
# --------------------------------------------------------------------------

def compress(data, level=COMPRESSION_LEVEL_DEFAULT, options=None, zstd_dict=None):
    """One-shot zstd compression, with optional dictionary and/or
    advanced parameters (`options`). Any of the four shapes works:
    no extras, options only, dict only, options + dict."""
    if not (1 <= level <= 22):
        raise ValueError(f"level out of range: {level}")
    if zstd_dict is not None and not isinstance(zstd_dict, ZstdDict):
        raise TypeError("zstd_dict must be a ZstdDict")
    params = _normalize_options(options)
    try:
        if params is not None and zstd_dict is not None:
            return _compress_cap.zstd_compress_advanced_with_dict(
                bytes(data), zstd_dict.dict_content, level, params)
        if params is not None:
            return _compress_cap.zstd_compress_advanced(bytes(data), level, params)
        if zstd_dict is not None:
            return _compress_cap.zstd_compress_with_dict(
                bytes(data), zstd_dict.dict_content, level)
        return _zstd_cap.zstd_compress(bytes(data), level)
    except RuntimeError as e:
        raise ZstdError(f"compress failed: {e}") from None


def decompress(data, zstd_dict=None, options=None):
    """One-shot zstd decompression, with optional dictionary and/or
    advanced parameters (`options`). Mirror of `compress`."""
    if zstd_dict is not None and not isinstance(zstd_dict, ZstdDict):
        raise TypeError("zstd_dict must be a ZstdDict")
    if not data:
        return b""
    params = _normalize_options(options)
    try:
        if params is not None and zstd_dict is not None:
            return _compress_cap.zstd_decompress_advanced_with_dict(
                bytes(data), zstd_dict.dict_content, params)
        if params is not None:
            return _compress_cap.zstd_decompress_advanced(bytes(data), params)
        if zstd_dict is not None:
            return _compress_cap.zstd_decompress_with_dict(
                bytes(data), zstd_dict.dict_content)
        return _zstd_cap.zstd_decompress(bytes(data))
    except RuntimeError as e:
        raise ZstdError(f"Invalid zstd data: {e}") from None


# --------------------------------------------------------------------------
# Streaming classes
# --------------------------------------------------------------------------

class ZstdCompressor:
    """Buffered-then-one-shot zstd compressor.

    Stdlib `_zstd.ZstdCompressor` is streaming; we buffer until flush.
    `mode` is honored at the API surface but only FLUSH_FRAME ends the stream.
    """
    CONTINUE     = 0
    FLUSH_BLOCK  = 1
    FLUSH_FRAME  = 2

    def __init__(self, level=COMPRESSION_LEVEL_DEFAULT, options=None, zstd_dict=None):
        if not (1 <= level <= 22):
            raise ValueError(f"level out of range: {level}")
        if zstd_dict is not None and not isinstance(zstd_dict, ZstdDict):
            raise TypeError("zstd_dict must be a ZstdDict")
        self._level = level
        self._dict = zstd_dict
        self._params = _normalize_options(options)
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
        if self._params is not None and self._dict is not None:
            return _compress_cap.zstd_compress_advanced_with_dict(
                bytes(self._buf), self._dict.dict_content,
                self._level, self._params)
        if self._params is not None:
            return _compress_cap.zstd_compress_advanced(
                bytes(self._buf), self._level, self._params)
        if self._dict is not None:
            return _compress_cap.zstd_compress_with_dict(
                bytes(self._buf), self._dict.dict_content, self._level)
        return _zstd_cap.zstd_compress(bytes(self._buf), self._level)


class ZstdDecompressor:
    """Buffered-then-one-shot zstd decompressor."""

    def __init__(self, zstd_dict=None, options=None):
        if zstd_dict is not None and not isinstance(zstd_dict, ZstdDict):
            raise TypeError("zstd_dict must be a ZstdDict")
        self._dict = zstd_dict
        self._params = _normalize_options(options)
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
            if self._params is not None and self._dict is not None:
                full = _compress_cap.zstd_decompress_advanced_with_dict(
                    bytes(self._buf), self._dict.dict_content, self._params)
            elif self._params is not None:
                full = _compress_cap.zstd_decompress_advanced(
                    bytes(self._buf), self._params)
            elif self._dict is not None:
                full = _compress_cap.zstd_decompress_with_dict(
                    bytes(self._buf), self._dict.dict_content)
            else:
                full = _zstd_cap.zstd_decompress(bytes(self._buf))
        except RuntimeError:
            # Likely truncated — wait for more input.
            return b""
        self._eof = True
        return full if max_length < 0 else full[:max_length]


# --------------------------------------------------------------------------
# Frame header parsing (pure Python; the zstd frame header is small and
# documented, no need to round-trip through the cap)
#
# Format reference: RFC 8478 §3.1.1 (Frame Header).
# --------------------------------------------------------------------------

_ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"

# (FCS_flag, single_segment) -> FCS field width in bytes
_FCS_SIZE = {
    (0, 0): 0,
    (0, 1): 1,
    (1, 0): 2, (1, 1): 2,
    (2, 0): 4, (2, 1): 4,
    (3, 0): 8, (3, 1): 8,
}
# DID_flag -> DID field width in bytes
_DID_SIZE = {0: 0, 1: 1, 2: 2, 3: 4}


class FrameInfo:
    """Information about a Zstandard frame header."""
    __slots__ = ('decompressed_size', 'dictionary_id')

    def __init__(self, decompressed_size, dictionary_id):
        super().__setattr__('decompressed_size', decompressed_size)
        super().__setattr__('dictionary_id', dictionary_id)

    def __setattr__(self, name, _):
        raise AttributeError(f"can't set attribute {name!r}")

    def __repr__(self):
        return (f"FrameInfo(decompressed_size={self.decompressed_size}, "
                f"dictionary_id={self.dictionary_id})")


def get_frame_info(frame_buffer):
    """Parse a zstd frame header, returning a FrameInfo.

    `frame_buffer` must start at the beginning of a frame and include at
    least the frame header (6-18 bytes depending on flags).

    `decompressed_size` is the declared content size (may be None if the
    frame doesn't carry one — FCS_flag=0 + single_segment=0). `dictionary_id`
    is the dict ID referenced by the frame (0 if no dict).
    """
    buf = bytes(frame_buffer)
    if len(buf) < 6:
        raise ZstdError("frame buffer too short for header")
    if buf[:4] != _ZSTD_MAGIC:
        raise ZstdError("not a zstd frame (bad magic)")
    desc = buf[4]
    fcs_flag       = (desc >> 6) & 0x3
    single_segment = (desc >> 5) & 0x1
    did_flag       = desc & 0x3
    pos = 5
    if not single_segment:
        # Window_Descriptor byte
        pos += 1
    did_size = _DID_SIZE[did_flag]
    fcs_size = _FCS_SIZE[(fcs_flag, single_segment)]
    if len(buf) < pos + did_size + fcs_size:
        raise ZstdError("frame buffer too short for declared header fields")
    # Dictionary ID (little-endian)
    if did_size == 0:
        did = 0
    elif did_size == 1:
        did = buf[pos]
    elif did_size == 2:
        did = struct.unpack("<H", buf[pos:pos+2])[0]
    else:  # 4
        did = struct.unpack("<I", buf[pos:pos+4])[0]
    pos += did_size
    # Frame_Content_Size (little-endian; the 2-byte variant has +256 added)
    if fcs_size == 0:
        fcs = None  # size unknown — single_segment=0 + FCS_flag=0
    elif fcs_size == 1:
        fcs = buf[pos]
    elif fcs_size == 2:
        fcs = struct.unpack("<H", buf[pos:pos+2])[0] + 256
    elif fcs_size == 4:
        fcs = struct.unpack("<I", buf[pos:pos+4])[0]
    else:  # 8
        fcs = struct.unpack("<Q", buf[pos:pos+8])[0]
    return FrameInfo(decompressed_size=fcs, dictionary_id=did)


def get_frame_size(frame_buffer):
    """Return the number of bytes occupied by the first frame.

    Routes through libzstd's `ZSTD_findFrameCompressedSize` for correctness
    on edge cases (skippable frames, trailing checksum bytes, etc.) — pure-
    Python block-walking would work for most frames but miss those.
    """
    return _compress_cap.zstd_get_frame_size(bytes(frame_buffer))


# --------------------------------------------------------------------------
# Stubs for the surface-area names callers may reference
# --------------------------------------------------------------------------

class CompressionParameter(enum.IntEnum):
    """libzstd ZSTD_cParameter IDs. Values are numerically stable across
    libzstd versions; pass via `compress(data, options={...})` etc."""
    compression_level    = 100
    window_log           = 101
    chain_log            = 102
    hash_log             = 103
    search_log           = 104
    min_match            = 105
    target_length        = 106
    strategy             = 107
    enable_long_distance_matching = 160
    ldm_hash_log         = 161
    ldm_min_match        = 162
    ldm_bucket_size_log  = 163
    ldm_hash_rate_log    = 164
    content_size_flag    = 200
    checksum_flag        = 201
    dict_id_flag         = 202
    nb_workers           = 400
    job_size             = 401
    overlap_log          = 402


class DecompressionParameter(enum.IntEnum):
    """libzstd ZSTD_dParameter IDs."""
    window_log_max = 100


class Strategy(enum.IntEnum):
    """libzstd ZSTD_strategy IDs (values for CompressionParameter.strategy)."""
    fast      = 1
    dfast     = 2
    greedy    = 3
    lazy      = 4
    lazy2     = 5
    btlazy2   = 6
    btopt     = 7
    btultra   = 8
    btultra2  = 9


def _normalize_options(options):
    """Coerce a stdlib-style `options` mapping or iterable of pairs to a
    list of (int, int) tuples the cap accepts. Returns None for an empty
    set so callers can short-circuit to the non-advanced path."""
    if options is None:
        return None
    if isinstance(options, dict):
        items = options.items()
    else:
        items = options
    pairs = [(int(k), int(v)) for k, v in items]
    return pairs if pairs else None


# --------------------------------------------------------------------------
# File I/O
#
# Buffered-then-one-shot wrapper on top of ZstdCompressor / ZstdDecompressor,
# following the same pattern the bz2.py and lzma.py shims use here. On read,
# the full file is decompressed into a BytesIO at open time; on write, all
# writes are buffered and the frame is emitted on close. Adequate for the
# typical "compress one file" workload; not memory-efficient for multi-GB
# streams. Surface matches stdlib `compression.zstd.ZstdFile` /
# `compression.zstd.open` so existing code paths work unchanged.
# --------------------------------------------------------------------------

_MODE_CLOSED = 0
_MODE_READ   = 1
_MODE_WRITE  = 2


class ZstdFile(_streams.BaseStream):
    """A file-like object providing transparent zstd (de)compression.

    Wraps either a file path or an existing file-like object. Binary-mode
    only at this layer; text mode is added by `open()` via TextIOWrapper.
    """

    FLUSH_BLOCK = ZstdCompressor.FLUSH_BLOCK
    FLUSH_FRAME = ZstdCompressor.FLUSH_FRAME

    def __init__(self, file, mode="r", *,
                 level=None, options=None, zstd_dict=None):
        self._fp = None
        self._close_fp = False
        self._mode = _MODE_CLOSED
        self._buffer = None

        if not isinstance(mode, str):
            raise ValueError("mode must be a str")
        if options is not None and not isinstance(options, dict):
            raise TypeError("options must be a dict or None")
        bin_mode = mode.removesuffix("b")
        if bin_mode == "r":
            if level is not None:
                raise TypeError("level is illegal in read mode")
            self._mode = _MODE_READ
            file_mode = "rb"
        elif bin_mode in ("w", "a", "x"):
            if level is not None and not isinstance(level, int):
                raise TypeError("level must be int or None")
            self._mode = _MODE_WRITE
            file_mode = bin_mode + "b"
            comp_level = COMPRESSION_LEVEL_DEFAULT if level is None else level
            self._compressor = ZstdCompressor(
                level=comp_level, options=options, zstd_dict=zstd_dict)
        else:
            raise ValueError(f"Invalid mode: {mode!r}")

        if isinstance(file, (str, bytes, os.PathLike)):
            self._fp = _builtin_open(file, file_mode)
            self._close_fp = True
        elif hasattr(file, "read") or hasattr(file, "write"):
            self._fp = file
        else:
            raise TypeError("file must be str/bytes/path or file-like")

        if self._mode == _MODE_READ:
            raw = self._fp.read()
            if raw:
                self._buffer = io.BytesIO(decompress(raw,
                                                     zstd_dict=zstd_dict,
                                                     options=options))
            else:
                self._buffer = io.BytesIO(b"")

    # ---- BaseStream contract --------------------------------------------

    def close(self):
        if self.closed:
            return
        try:
            if self._mode == _MODE_WRITE:
                self._fp.write(self._compressor.flush())
                self._compressor = None
            elif self._mode == _MODE_READ:
                self._buffer = None
        finally:
            try:
                if self._close_fp:
                    self._fp.close()
            finally:
                self._fp = None
                self._close_fp = False
                self._mode = _MODE_CLOSED
                super().close()

    def writable(self):
        self._check_not_closed()
        return self._mode == _MODE_WRITE

    def readable(self):
        self._check_not_closed()
        return self._mode == _MODE_READ

    def seekable(self):
        return self._mode == _MODE_READ

    # ---- read API --------------------------------------------------------

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

    def readline(self, size=-1):
        self._check_can_read()
        return self._buffer.readline(size)

    # ---- write API -------------------------------------------------------

    def write(self, data):
        self._check_can_write()
        # ZstdCompressor.compress() buffers; the frame is flushed on close.
        self._compressor.compress(data)
        # Match stdlib: return number of *uncompressed* bytes accepted.
        return _nbytes(data)

    def flush(self, mode=None):
        """No-op on read or after close; on write, drain the buffered
        frame to the underlying file. Called both explicitly by user code
        and implicitly by BaseStream.close(), so it has to be idempotent.

        The buffered-then-one-shot model can't truly mid-flush a sub-frame
        without ending the stream; FLUSH_FRAME closes this object's frame
        and the next write starts a new one. For interactive use, leave
        this to close() rather than calling explicitly.
        """
        if self._mode != _MODE_WRITE or self._fp is None:
            return  # read mode, already closed, or close()-internal re-call
        if self._compressor is None:
            return  # already drained
        if mode is None or mode == ZstdCompressor.FLUSH_FRAME:
            self._fp.write(self._compressor.flush())
            # Start a fresh frame on the next write
            self._compressor = ZstdCompressor()

    # ---- seek/tell (read mode only) -------------------------------------

    def seek(self, offset, whence=io.SEEK_SET):
        self._check_can_seek()
        return self._buffer.seek(offset, whence)

    def tell(self):
        self._check_not_closed()
        if self._mode == _MODE_READ:
            return self._buffer.tell()
        # In write mode, the "position" pre-flush is just the accumulated
        # uncompressed input the cap will compress on flush. Stdlib reports
        # this as the compressor's pledged input count; we approximate.
        return 0


def _nbytes(dat):
    if isinstance(dat, (bytes, bytearray)):
        return len(dat)
    with memoryview(dat) as mv:
        return mv.nbytes


def open(file, mode="rb", *,
         level=None, options=None, zstd_dict=None,
         encoding=None, errors=None, newline=None):
    """Open a zstd-compressed file in binary or text mode.

    Mirrors stdlib `compression.zstd.open`. Text mode wraps the binary
    ZstdFile in an io.TextIOWrapper.
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

    zstd_mode = mode.replace("t", "")
    binary_file = ZstdFile(file, zstd_mode,
                           level=level, options=options, zstd_dict=zstd_dict)
    if "t" in mode:
        encoding = io.text_encoding(encoding)
        return io.TextIOWrapper(binary_file, encoding, errors, newline)
    return binary_file

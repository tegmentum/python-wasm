# zlib-wasm → CPython zlib mapping

Phase A of the multiplexer-retirement work. Maps what `zlib-wasm`'s WIT exports to what CPython's stdlib `zlib` module exposes, so we can decide what stays in Python (in the `Lib/zlib.py` shim) and what gets handled at the cap boundary.

## zlib-wasm WIT exports (package `zlib:compression@0.1.0`)

```wit
interface simple {
    compress: func(data: list<u8>, level: compression-level) -> result<list<u8>, error-code>;
    decompress: func(data: list<u8>) -> result<list<u8>, error-code>;
    compress-bound: func(source-len: u64) -> u64;
}

interface checksum {
    crc32:           func(data: list<u8>) -> u32;
    crc32-update:    func(crc: u32, data: list<u8>) -> u32;
    crc32-combine:   func(crc1: u32, crc2: u32, len2: u64) -> u32;
    adler32:         func(data: list<u8>) -> u32;
    adler32-update:  func(adler: u32, data: list<u8>) -> u32;
    adler32-combine: func(adler1: u32, adler2: u32, len2: u64) -> u32;
}

interface deflate {
    resource compressor {
        constructor(level: s32);
        new-with-options: static func(level, method, window-bits, mem-level, strategy) -> result<compressor, error-code>;
        compress-chunk: func(input: list<u8>, flush: flush-mode) -> result<list<u8>, error-code>;
        reset: func() -> result<_, error-code>;
        total-in: func() -> u64;
        total-out: func() -> u64;
    }
    resource decompressor {
        constructor();
        new-with-window-bits: static func(window-bits: s32) -> result<decompressor, error-code>;
        decompress-chunk: func(input: list<u8>, flush: flush-mode) -> result<list<u8>, error-code>;
        reset: func() -> result<_, error-code>;
        total-in: func() -> u64;
        total-out: func() -> u64;
    }
}

interface info {
    version: func() -> string;
    compile-flags: func() -> u32;
    get-info: func() -> algorithm-info;
}
```

## CPython `zlib` module surface (Lib/zlib.py callers)

### Module-level

| CPython API                                      | zlib-wasm match                                | Gap            |
|--------------------------------------------------|------------------------------------------------|----------------|
| `compress(data, level=-1, wbits=MAX_WBITS)`      | `simple.compress(data, level)`                 | wbits handled in Python (frame wrapping for zlib/raw/gzip) |
| `decompress(data, wbits=MAX_WBITS, bufsize=…)`   | `simple.decompress(data)`                      | wbits handled in Python; bufsize ignored (one-shot returns all) |
| `crc32(data, value=0)`                           | `checksum.crc32-update(value, data)`           | exact match    |
| `adler32(data, value=1)`                         | `checksum.adler32-update(value, data)`         | exact match    |
| `MAX_WBITS`, `Z_*` constants                     | Python-side only                               | constants stay in shim |

### `compressobj(level=-1, method=DEFLATED, wbits=MAX_WBITS, memLevel=DEF_MEM_LEVEL, strategy=Z_DEFAULT_STRATEGY, zdict=None)`

| CPython method      | zlib-wasm match                                  | Gap                |
|---------------------|--------------------------------------------------|--------------------|
| Construction        | `compressor.new-with-options(...)`               | `zdict` parameter not in WIT — Python wraps via prefix-feed if needed |
| `.compress(data)`   | `compressor.compress-chunk(data, no-flush)`      | exact              |
| `.flush(mode)`      | `compressor.compress-chunk(b"", <map mode>)`     | exact (mode mapping in Python) |
| `.copy()`           | not in WIT                                        | implement in Python by replaying the input log (slow path) OR skip on first cut |

### `decompressobj(wbits=MAX_WBITS, zdict=None)`

| CPython method/attr          | zlib-wasm match                                              | Gap                |
|------------------------------|--------------------------------------------------------------|--------------------|
| Construction                 | `decompressor.new-with-window-bits(wbits)`                   | `zdict` not in WIT |
| `.decompress(data, max_length=0)` | `decompressor.decompress-chunk(data, no-flush)` + Python state tracking for unconsumed_tail/eof | max_length in Python by chunking + checking total-out |
| `.flush(length=DEF_BUF_SIZE)`| `decompressor.decompress-chunk(b"", finish)`                 | exact              |
| `.unused_data`               | Python tracks via total-in vs bytes fed                      | derive from `total-in` |
| `.unconsumed_tail`           | Python tracks via total-in vs input bytes                    | derive from `total-in` |
| `.eof`                       | Python tracks (set after stream-end error or flush)          | observable via error-code on next feed |
| `.copy()`                    | not in WIT                                                    | same as compressor — defer |

## What the new `_zlib_cap` C extension exposes to Python

Minimum surface to satisfy the existing `Lib/zlib.py` shim:

```python
# Functional (one-shot)
_zlib_cap.compress(data: bytes, level: int = -1) -> bytes
_zlib_cap.decompress(data: bytes) -> bytes
_zlib_cap.crc32(data: bytes, value: int = 0) -> int
_zlib_cap.adler32(data: bytes, value: int = 1) -> int

# Resources (streaming) - returned by constructors, opaque to Python
_zlib_cap.compressor_new(level, method, window_bits, mem_level, strategy) -> Compressor
_zlib_cap.decompressor_new(window_bits) -> Decompressor

# Compressor methods
Compressor.compress_chunk(data: bytes, flush: int) -> bytes
Compressor.total_in() -> int
Compressor.total_out() -> int

# Decompressor methods
Decompressor.decompress_chunk(data: bytes, flush: int) -> bytes
Decompressor.total_in() -> int
Decompressor.total_out() -> int
```

`Lib/zlib.py` continues to wrap these into the stdlib API (handling wbits framing in Python for raw/zlib/gzip via header/trailer bytes, tracking `unused_data`/`unconsumed_tail`/`eof` from total-in deltas).

## Decision: no WIT changes needed for Phase A

The existing zlib-wasm WIT is sufficient for stdlib-equivalent `compress`/`decompress`/`crc32`/`adler32` AND streaming `compressobj`/`decompressobj` with `.unused_data`/`.unconsumed_tail`/`.eof` semantics (Python side tracks via `total-in`/`total-out`).

Deferred to future WIT extensions (not blocking Phase A):

- `zdict` (preset dictionary) — used by some streaming protocols (Postgres wire, HTTP) but not by anything in stdlib's default usage.
- `.copy()` — used by some advanced consumers; can be added via a `compressor.copy()` static method later.

Phase A scope is therefore: build the `_zlib_cap` extension, rewrite the `Lib/zlib.py` shim to call it, replace the multiplexer plug with `zlib-wasm` in compose. No zlib-wasm rebuild needed.

# Per-codec compression cap WIT design

Standardized WIT shape for `zlib-wasm`, `bzip2-wasm`, `lzma-wasm`, `zstd-wasm`. All four caps follow the same skeleton so python-wasm's `cpython-ext/_<codec>_cap/` extensions can be near-identical clones, and so consumers can swap one cap for another without re-architecting their import side.

## Skeleton

Each cap declares one WIT package and four-to-five interfaces:

```
<codec>:compression@0.1.0/
    simple        # one-shot compress/decompress + compress-bound
    streaming     # compressor/decompressor resources with chunk feed + total counters
    info          # library version + algorithm metadata
    [checksum]    # only zlib: crc32, adler32, _update, _combine variants
    [advanced]    # codec-specific knobs that don't generalize:
                  #   - lzma: format selection (xz/alone/raw), check, preset, filters, memlimit
                  #   - zstd: dictionaries, training, advanced parameters
                  #   - zlib: zdict (preset dictionary) for streaming
                  #   - bzip2: (none — bzip2 has no relevant advanced knobs)
```

Worlds:

```wit
world <codec>-component {
    export simple;
    export streaming;
    export info;
    // optional:
    export checksum;   // zlib only
    export advanced;   // lzma, zstd, zlib
}

world <codec>-library {
    // Slim variant used by composers that don't need the resources
    export simple;
    [export checksum;]  // zlib only
}
```

## `simple` interface (identical across all four codecs)

```wit
interface simple {
    use streaming.{error-code, compression-level};

    /// Compress `data` in one call at the named level. Returns the
    /// codec's "raw" output frame (no zlib RFC-1950 envelope for the
    /// zlib codec — use `streaming` with positive wbits for that).
    compress: func(data: list<u8>, level: compression-level) -> result<list<u8>, error-code>;

    /// Decompress `data` in one call. Auto-detects the format where the
    /// codec supports it (lzma_auto, zstd dictless), otherwise raw.
    decompress: func(data: list<u8>) -> result<list<u8>, error-code>;

    /// Upper bound on output size for input of `source-len` bytes.
    /// Used by callers that need to size an output buffer up front.
    compress-bound: func(source-len: u64) -> u64;
}
```

The level enum is the named-step shape from zlib-wasm (none / best-speed / default / best-compression). Numeric levels are exposed via `streaming.new-with-options` where the codec accepts them.

## `streaming` interface (uniform across all four)

```wit
interface streaming {
    enum error-code {
        ok, stream-end, need-dict, stream-error,
        data-error, mem-error, buf-error, version-error,
    }

    enum compression-level { none, best-speed, default, best-compression }

    enum flush-mode {
        no-flush, partial-flush, sync-flush, full-flush, finish, block, trees,
    }

    record decompress-result {
        output:   list<u8>,
        consumed: u64,    // bytes from `input` actually consumed
        eof:      bool,   // true if the decompressor saw stream-end
    }

    resource compressor {
        constructor(level: s32);

        /// Stream-specific options. window-bits and mem-level honored
        /// only by zlib; other codecs ignore them. strategy honored by
        /// zlib (Z_DEFAULT_STRATEGY etc.) and otherwise ignored.
        new-with-options: static func(
            level: s32, method: s32, window-bits: s32, mem-level: s32,
            strategy: strategy,
        ) -> result<compressor, error-code>;

        compress-chunk: func(input: list<u8>, flush: flush-mode)
                            -> result<list<u8>, error-code>;
        reset: func() -> result<_, error-code>;
        total-in:  func() -> u64;
        total-out: func() -> u64;
    }

    resource decompressor {
        constructor();

        /// For zlib: window-bits selects raw (-15) / zlib (15) / gzip
        /// (15+16) / auto (15+32) modes. Other codecs ignore.
        new-with-window-bits: static func(window-bits: s32)
                                -> result<decompressor, error-code>;

        /// Decompress a chunk; flush controls partial output flushing.
        decompress-chunk: func(input: list<u8>, flush: flush-mode)
                            -> result<list<u8>, error-code>;

        /// Decompress a chunk and report `consumed` + `eof`. The
        /// `decompress-chunk` variant doesn't return these but
        /// `total-in` after the call gives the cumulative consumed count.
        decompress-chunk-counted: func(input: list<u8>, flush: flush-mode)
                                    -> result<decompress-result, error-code>;

        reset: func() -> result<_, error-code>;
        total-in:  func() -> u64;
        total-out: func() -> u64;
    }

    enum strategy {
        default-strategy, filtered, huffman-only, rle, fixed,
    }
}
```

The `decompress-chunk-counted` variant exposes the consumed/eof information CPython's stdlib decompressor objects need (`unused_data`, `unconsumed_tail`, `eof` attributes). The plain `decompress-chunk` stays for callers that don't need them.

## `info` interface (identical across all four)

```wit
interface info {
    record algorithm-info {
        name:                string,
        version:             string,
        min-level:           s32,
        max-level:           s32,
        default-level:       s32,
        supports-streaming:  bool,
        supports-dictionary: bool,
    }

    version:        func() -> string;
    compile-flags:  func() -> u32;
    get-info:       func() -> algorithm-info;
}
```

## Codec-specific `checksum` interface (zlib only)

```wit
interface checksum {
    crc32:           func(data: list<u8>) -> u32;
    crc32-update:    func(crc: u32, data: list<u8>) -> u32;
    crc32-combine:   func(crc1: u32, crc2: u32, len2: u64) -> u32;

    adler32:         func(data: list<u8>) -> u32;
    adler32-update:  func(adler: u32, data: list<u8>) -> u32;
    adler32-combine: func(adler1: u32, adler2: u32, len2: u64) -> u32;
}
```

bzip2, lzma, and zstd do NOT export a checksum interface — their format checks (CRC32 in zstd frames, CRC64/SHA256 in XZ) are computed and verified internally during decode, not exposed as standalone functions. For users who want standalone CRC-32 / CRC-32C, the `tegmentum:hashing-multiplexer/hashing-dispatcher` is the right home, not the codec caps.

## Codec-specific `advanced` interface — per codec

### zlib `advanced`

```wit
interface advanced {
    use streaming.{compressor, decompressor, error-code};

    /// Set the preset dictionary for a compressor or decompressor.
    /// Must be called before any compress/decompress.
    compressor-set-dictionary:   func(c: borrow<compressor>, dict: list<u8>)
                                    -> result<_, error-code>;
    decompressor-set-dictionary: func(d: borrow<decompressor>, dict: list<u8>)
                                    -> result<_, error-code>;
}
```

### bzip2 `advanced`

(none — bzip2's API is essentially compresslevel and that's it. No advanced interface exported.)

### lzma `advanced`

```wit
interface advanced {
    use streaming.{error-code};

    enum format { xz, alone, raw, auto }
    enum check  { none, crc32, crc64, sha256 }

    /// Filter chain for raw-format LZMA. Each filter has an id (the
    /// LZMA filter constant, e.g. 0x21 for LZMA2) and an options blob
    /// (codec-specific; for LZMA2 it's the preset/dict-size struct).
    record filter {
        id:      u64,
        options: list<u8>,
    }

    /// One-shot compress with full options.
    compress-full: func(data: list<u8>, format: format, check: check,
                        preset: u32, filters: list<filter>)
                    -> result<list<u8>, string>;

    /// One-shot decompress with format selection + memory limit.
    decompress-full: func(data: list<u8>, format: format,
                          memlimit: option<u64>)
                      -> result<list<u8>, string>;
}
```

### zstd `advanced`

Lifted from the existing `tegmentum:compression-multiplexer/zstd-extras`. zstd has the richest codec-specific surface so this interface is the largest.

```wit
interface advanced {
    use streaming.{error-code};

    record zstd-param { id: u32, value: s32 }

    resource zstd-dict {
        constructor(bytes: list<u8>);
        id: func() -> u32;
        as-bytes: func() -> list<u8>;
    }

    compress-with-dict:   func(input: list<u8>, dict: borrow<zstd-dict>, level: s32)
                              -> result<list<u8>, string>;
    decompress-with-dict: func(input: list<u8>, dict: borrow<zstd-dict>)
                              -> result<list<u8>, string>;

    train-dict:    func(samples: list<list<u8>>, dict-size: u32)
                       -> result<list<u8>, string>;
    finalize-dict: func(dict-content: list<u8>, samples: list<list<u8>>,
                        dict-size: u32, level: s32)
                       -> result<list<u8>, string>;
    get-frame-size: func(frame: list<u8>) -> result<u64, string>;

    compress-advanced:   func(input: list<u8>, level: s32, params: list<zstd-param>)
                            -> result<list<u8>, string>;
    decompress-advanced: func(input: list<u8>, params: list<zstd-param>)
                            -> result<list<u8>, string>;

    compress-advanced-with-dict:   func(input: list<u8>, level: s32,
                                        params: list<zstd-param>,
                                        dict: borrow<zstd-dict>)
                                        -> result<list<u8>, string>;
    decompress-advanced-with-dict: func(input: list<u8>,
                                        params: list<zstd-param>,
                                        dict: borrow<zstd-dict>)
                                        -> result<list<u8>, string>;
}
```

## Naming conventions

- Each cap's WIT package is `<codec>:compression@0.1.0` (e.g., `zlib:compression@0.1.0`, `bzip2:compression@0.1.0`).
- Interfaces use unhyphenated lowercase: `simple`, `streaming`, `info`, `checksum`, `advanced`.
- Error type: `error-code` (enum), declared in `streaming` and re-used by `simple`. Codec-specific advanced interfaces may use `string` errors when the underlying codec returns rich error messages (zstd, lzma) rather than mapping to the limited error-code enum.

## Versioning

- `@0.1.0` for the initial release.
- Additive WIT changes (new functions, new resource methods, new advanced-interface members) bump minor: `@0.1.1`, `@0.1.2`, …
- Breaking changes (signature change, removal, renumbered enum) bump major: `@0.2.0`.
- The four caps version independently — `zlib:compression@0.1.3` is fine alongside `bzip2:compression@0.1.0`.

## Vendoring into python-wasm

Each `cpython-ext/_<codec>_cap/wit/deps/<codec>/` contains the cap's WIT files, vendored from `~/git/<codec>-wasm/wit/`. The vendored copy MUST match the cap-side version exactly — otherwise `wac plug` fails with "instance export has the wrong type."

When a cap bumps minor or major, the python-wasm side gets re-vendored as part of the migration commit.

## Reference implementation: zlib-wasm (Phase B)

zlib-wasm is the first cap to ship under this shape. Its existing WIT (`zlib:compression@0.1.0` with `simple`, `streaming`/`deflate`, `checksum`, `info`) already matches this design almost exactly — the rename of `deflate` to `streaming` is the only naming alignment needed, and the `decompress-chunk-counted` variant is the only function to add.

Phases C/D/E (bzip2, lzma, zstd) implement against this design from the start.

/* _compress_cap — Python C extension routing to the compression-multiplexer
 * capability over the Component Model.
 *
 * Module is named `_compress_cap` (NOT `_compression`) — even though stdlib
 * `_compression.py` was moved to `compression._common._streams` in 3.14,
 * the `_cap` suffix marks this as the capability-routed extension and
 * disambiguates from the `compression` package.
 *
 * Statically linked into wasi-sdk CPython (see Modules/Setup.local). The
 * imported WIT functions appear as wasm imports on python-component.wasm,
 * satisfied at compose time by wac/composectl plugging
 * compression_multiplexer.wasm into the import.
 *
 * Python surface — one (compress, decompress) pair per algorithm exposed by
 * the multiplexer's `algorithm` enum. The Python-side shims `zlib.py`,
 * `bz2.py`, `lzma.py`, `zstd.py` adapt each to its stdlib API contract.
 *
 *   _compress_cap.deflate_compress (data: bytes, level: int = 6) -> bytes
 *   _compress_cap.deflate_decompress (data: bytes)              -> bytes
 *   _compress_cap.bzip2_compress    (data: bytes, level: int = 9) -> bytes
 *   _compress_cap.bzip2_decompress  (data: bytes)               -> bytes
 *   _compress_cap.lzma_compress     (data: bytes, level: int = 6) -> bytes
 *   _compress_cap.lzma_decompress   (data: bytes)               -> bytes
 *   _compress_cap.zstd_compress     (data: bytes, level: int = 3) -> bytes
 *   _compress_cap.zstd_decompress   (data: bytes)               -> bytes
 *   _compress_cap.lz4_compress      (data: bytes, level: int = 0) -> bytes
 *   _compress_cap.lz4_decompress    (data: bytes)               -> bytes
 *   _compress_cap.openzl_compress   (data: bytes, level: int = 3) -> bytes
 *   _compress_cap.openzl_decompress (data: bytes)               -> bytes
 *   _compress_cap.store_compress    (data: bytes)               -> bytes  (passthrough)
 *
 * Back-compat aliases (kept for callers using the original 2-function API):
 *   _compress_cap.deflate_raw   = deflate_compress
 *   _compress_cap.inflate_raw   = deflate_decompress
 *
 * All output is the raw codec frame (no zlib RFC1950 wrapper for deflate,
 * no .xz container for lzma, etc.) — the Python shim adds the wrapper as
 * needed.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "gen/compression_import.h"

typedef tegmentum_compression_multiplexer_compression_dispatcher_algorithm_t algo_t;

/* Helpers ----------------------------------------------------------------- */

/* Copy a Python bytes-like into a malloc'd list<u8> the canonical ABI owns. */
static int bytes_to_list_u8(PyObject *src, compression_import_list_u8_t *out)
{
    Py_buffer view;
    if (PyObject_GetBuffer(src, &view, PyBUF_SIMPLE | PyBUF_C_CONTIGUOUS) < 0) {
        return -1;
    }
    if (view.len > 0) {
        out->ptr = (uint8_t *) malloc((size_t) view.len);
        if (out->ptr == NULL) {
            PyBuffer_Release(&view);
            PyErr_NoMemory();
            return -1;
        }
        memcpy(out->ptr, view.buf, (size_t) view.len);
    } else {
        out->ptr = NULL;
    }
    out->len = (size_t) view.len;
    PyBuffer_Release(&view);
    return 0;
}

static PyObject *list_u8_to_bytes(compression_import_list_u8_t *src)
{
    PyObject *r = PyBytes_FromStringAndSize((const char *) src->ptr,
                                            (Py_ssize_t) src->len);
    compression_import_list_u8_free(src);
    return r;
}

static PyObject *raise_from_wit(const char *prefix, compression_import_string_t *err)
{
    PyObject *e = PyUnicode_FromStringAndSize((const char *) err->ptr,
                                              (Py_ssize_t) err->len);
    compression_import_string_free(err);
    PyErr_SetObject(PyExc_RuntimeError,
                    e ? e : PyUnicode_FromString(prefix));
    Py_XDECREF(e);
    return NULL;
}

/* Generic helpers parameterized on algorithm. ----------------------------- */

static PyObject *do_compress(algo_t algo, PyObject *data, int level)
{
    if (level < 0 || level > 22) {
        PyErr_SetString(PyExc_ValueError, "level out of range");
        return NULL;
    }
    compression_import_list_u8_t input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;

    tegmentum_compression_multiplexer_compression_dispatcher_own_compressor_t own =
        tegmentum_compression_multiplexer_compression_dispatcher_constructor_compressor(
            algo, (uint8_t) level);

    compression_import_list_u8_t output;
    compression_import_string_t err;
    tegmentum_compression_multiplexer_compression_dispatcher_borrow_compressor_t borrow =
        tegmentum_compression_multiplexer_compression_dispatcher_borrow_compressor(own);
    bool is_ok = tegmentum_compression_multiplexer_compression_dispatcher_method_compressor_compress(
        borrow, &input, &output, &err);
    tegmentum_compression_multiplexer_compression_dispatcher_compressor_drop_own(own);

    if (!is_ok) return raise_from_wit("compress failed", &err);
    return list_u8_to_bytes(&output);
}

static PyObject *do_decompress(algo_t algo, PyObject *data)
{
    compression_import_list_u8_t input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;

    tegmentum_compression_multiplexer_compression_dispatcher_own_decompressor_t own =
        tegmentum_compression_multiplexer_compression_dispatcher_constructor_decompressor(algo);

    compression_import_list_u8_t output;
    compression_import_string_t err;
    tegmentum_compression_multiplexer_compression_dispatcher_borrow_decompressor_t borrow =
        tegmentum_compression_multiplexer_compression_dispatcher_borrow_decompressor(own);
    bool is_ok = tegmentum_compression_multiplexer_compression_dispatcher_method_decompressor_decompress(
        borrow, &input, &output, &err);
    tegmentum_compression_multiplexer_compression_dispatcher_decompressor_drop_own(own);

    if (!is_ok) return raise_from_wit("decompress failed", &err);
    return list_u8_to_bytes(&output);
}

/* Per-algorithm wrappers. Each picks the right Algorithm constant + default
 * level matching the format's idiomatic default. ------------------------- */

#define DEF_COMPRESS(name, algo_const, default_level)                          \
static PyObject *name##_compress(PyObject *self, PyObject *args, PyObject *kw) { \
    static char *kwl[] = {"data", "level", NULL};                              \
    PyObject *data; int level = (default_level);                               \
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i:" #name "_compress",       \
                                     kwl, &data, &level)) return NULL;         \
    return do_compress((algo_const), data, level);                             \
}

#define DEF_DECOMPRESS(name, algo_const)                                       \
static PyObject *name##_decompress(PyObject *self, PyObject *args, PyObject *kw) { \
    static char *kwl[] = {"data", NULL};                                       \
    PyObject *data;                                                            \
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:" #name "_decompress",       \
                                     kwl, &data)) return NULL;                 \
    return do_decompress((algo_const), data);                                  \
}

DEF_COMPRESS  (deflate, TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_DEFLATE, 6)
DEF_DECOMPRESS(deflate, TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_DEFLATE)
DEF_COMPRESS  (bzip2,   TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_BZIP2,   9)
DEF_DECOMPRESS(bzip2,   TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_BZIP2)
DEF_COMPRESS  (lzma,    TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_LZMA,    6)
DEF_DECOMPRESS(lzma,    TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_LZMA)
DEF_COMPRESS  (zstd,    TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_ZSTD,    3)
DEF_DECOMPRESS(zstd,    TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_ZSTD)
DEF_COMPRESS  (lz4,     TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_LZ4,     0)
DEF_DECOMPRESS(lz4,     TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_LZ4)
DEF_COMPRESS  (openzl,  TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_OPENZL,  3)
DEF_DECOMPRESS(openzl,  TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_OPENZL)

/* `store` is passthrough — exposed for completeness / API parity with the
 * multiplexer. Calling compress with level=0 against ALGORITHM_STORE returns
 * the input unchanged. */
DEF_COMPRESS  (store,   TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_STORE,   0)
DEF_DECOMPRESS(store,   TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_STORE)

#undef DEF_COMPRESS
#undef DEF_DECOMPRESS

/* Back-compat aliases — preserve the API the original 2-function module had. */
#define ALIAS(new_name, old_name) \
    static PyObject *old_name(PyObject *s, PyObject *a, PyObject *k) { return new_name(s, a, k); }
ALIAS(deflate_compress,   deflate_raw)
ALIAS(deflate_decompress, inflate_raw)
#undef ALIAS

/* CRC32 / Adler32 ---------------------------------------------------------
 *
 * Pure compute (no WIT call). Co-located here so the zlib.py shim has C-speed
 * checksums available — pure-Python crc32 over a 10 MB pip wheel runs in
 * seconds vs sub-millisecond in C, and zlib.crc32 is on the install hot path.
 * Static libz is retired so we can't borrow its impls; reimplement here.
 */

static uint32_t CRC32_TABLE[256];

static void init_crc32_table(void)
{
    /* IEEE 802.3 polynomial in reversed bit order (0xEDB88320). Matches the
     * libz / RFC 1952 CRC-32 used by gzip + zip. */
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t) i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        CRC32_TABLE[i] = c;
    }
}

static PyObject *cap_crc32(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "value", NULL};
    PyObject *data;
    unsigned long value = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|k:crc32", kwl, &data, &value)) {
        return NULL;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE | PyBUF_C_CONTIGUOUS) < 0) {
        return NULL;
    }
    uint32_t crc = (~(uint32_t) value) & 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *) view.buf;
    const Py_ssize_t n = view.len;
    for (Py_ssize_t i = 0; i < n; i++) {
        crc = CRC32_TABLE[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    PyBuffer_Release(&view);
    return PyLong_FromUnsignedLong((unsigned long) ((crc ^ 0xFFFFFFFFu) & 0xFFFFFFFFu));
}

static PyObject *cap_adler32(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "value", NULL};
    PyObject *data;
    unsigned long value = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|k:adler32", kwl, &data, &value)) {
        return NULL;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE | PyBUF_C_CONTIGUOUS) < 0) {
        return NULL;
    }
    uint32_t s1 = (uint32_t) (value & 0xFFFFu);
    uint32_t s2 = (uint32_t) ((value >> 16) & 0xFFFFu);
    const uint8_t *p = (const uint8_t *) view.buf;
    Py_ssize_t remaining = view.len;
    /* 5552 = largest n such that 255*n + n*(n+1)/2 stays below 2^32, so we
     * can defer the modulo to the end of each chunk. */
    while (remaining > 0) {
        Py_ssize_t chunk = remaining > 5552 ? 5552 : remaining;
        for (Py_ssize_t i = 0; i < chunk; i++) {
            s1 += p[i];
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
        p += chunk;
        remaining -= chunk;
    }
    PyBuffer_Release(&view);
    return PyLong_FromUnsignedLong((unsigned long) (((s2 << 16) | s1) & 0xFFFFFFFFu));
}

/* Module plumbing --------------------------------------------------------- */

#define M(name, fn, doc) { name, (PyCFunction) fn, METH_VARARGS | METH_KEYWORDS, doc }

static PyMethodDef compression_methods[] = {
    /* New per-algorithm API */
    M("deflate_compress",   deflate_compress,
      "deflate_compress(data: bytes, level: int = 6) -> bytes\n"
      "Raw DEFLATE compression via the multiplexer."),
    M("deflate_decompress", deflate_decompress, "Raw INFLATE decompression."),
    M("bzip2_compress",     bzip2_compress,
      "bzip2_compress(data: bytes, level: int = 9) -> bytes  (bz2 backend)"),
    M("bzip2_decompress",   bzip2_decompress, "bzip2 decompression."),
    M("lzma_compress",      lzma_compress,
      "lzma_compress(data: bytes, level: int = 6) -> bytes  (raw LZMA / .lzma container)"),
    M("lzma_decompress",    lzma_decompress, "lzma decompression."),
    M("zstd_compress",      zstd_compress,
      "zstd_compress(data: bytes, level: int = 3) -> bytes"),
    M("zstd_decompress",    zstd_decompress, "zstd decompression."),
    M("lz4_compress",       lz4_compress,
      "lz4_compress(data: bytes, level: int = 0) -> bytes  (level 0 = default)"),
    M("lz4_decompress",     lz4_decompress, "lz4 decompression."),
    M("openzl_compress",    openzl_compress,
      "openzl_compress(data: bytes, level: int = 3) -> bytes"),
    M("openzl_decompress",  openzl_decompress, "openzl decompression."),
    M("store_compress",     store_compress,
      "store_compress(data: bytes) -> bytes  (passthrough; for API parity)"),
    M("store_decompress",   store_decompress, "store decompression (passthrough)."),
    /* Back-compat aliases */
    M("deflate_raw",        deflate_raw, "Alias for deflate_compress (back-compat)."),
    M("inflate_raw",        inflate_raw, "Alias for deflate_decompress (back-compat)."),
    /* CRC32 / Adler32 — pure compute, no WIT call. */
    M("crc32",   cap_crc32,
      "crc32(data: bytes, value: int = 0) -> int\n"
      "CRC-32 (IEEE 802.3 / RFC 1952). C-speed; matches libz."),
    M("adler32", cap_adler32,
      "adler32(data: bytes, value: int = 1) -> int\n"
      "Adler-32 (RFC 1950). C-speed; matches libz."),
    {NULL, NULL, 0, NULL}
};

#undef M

static struct PyModuleDef compress_cap_module = {
    PyModuleDef_HEAD_INIT,
    "_compress_cap",
    "CPython binding for the tegmentum:compression-multiplexer WIT capability.\n"
    "All codecs (deflate/bzip2/lzma/zstd/lz4/openzl/store) provided by composing in\n"
    "compression_multiplexer.wasm at component-build time; this module exposes them\n"
    "as raw frames per format. Python-side shims (zlib.py, bz2.py, lzma.py, zstd.py)\n"
    "adapt each to its stdlib API contract.",
    -1,
    compression_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__compress_cap(void)
{
    init_crc32_table();
    return PyModule_Create(&compress_cap_module);
}

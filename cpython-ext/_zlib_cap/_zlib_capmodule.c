/* _zlib_cap — Python C extension routing zlib codec calls through the
 * zlib-wasm capability component (zlib:compression@0.1.0).
 *
 * Replaces the compression-multiplexer-routed _compress_cap.deflate_* /
 * _compress_cap.crc32 / _compress_cap.adler32 path. The zlib-wasm cap
 * exposes a richer per-codec WIT (simple compress/decompress + deflate
 * streaming resources + crc32/adler32 checksums + zlib version info)
 * than the multiplexer's least-common-denominator interface, so the
 * Python-side Lib/zlib.py shim can drop several workarounds it had to
 * carry on top of the multiplexer.
 *
 * Surface (matches the names Lib/zlib.py currently reaches for, so the
 * shim swap is a one-line search/replace):
 *
 *   _zlib_cap.deflate_compress(data: bytes, level: int = 6) -> bytes
 *   _zlib_cap.deflate_decompress(data: bytes) -> bytes
 *   _zlib_cap.deflate_decompress_counted(data: bytes) -> (bytes, int)
 *   _zlib_cap.crc32(data: bytes, value: int = 0) -> int
 *   _zlib_cap.adler32(data: bytes, value: int = 1) -> int
 *
 * Streaming compressor/decompressor objects (compressobj/decompressobj)
 * stay implemented in Lib/zlib.py on top of these primitives; deferring
 * resource-typed Python objects keeps Phase A focused on the multiplexer
 * removal. Phase B can promote those objects to wrap zlib-wasm's
 * deflate.compressor / deflate.decompressor resources directly.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include "gen/zlib_cap_import.h"

/* Convenience aliases — wit-bindgen-c's namespaced type names are very
 * long. Keep these short to make the call sites readable. */
#define DEFLATE_OWN_COMPRESSOR   zlib_compression_deflate_own_compressor_t
#define DEFLATE_BRW_COMPRESSOR   zlib_compression_deflate_borrow_compressor_t
#define DEFLATE_OWN_DECOMPRESSOR zlib_compression_deflate_own_decompressor_t
#define DEFLATE_BRW_DECOMPRESSOR zlib_compression_deflate_borrow_decompressor_t
#define DEFLATE_ERROR            zlib_compression_deflate_error_code_t
#define DEFLATE_FLUSH            zlib_compression_deflate_flush_mode_t
#define DEFLATE_STRATEGY         zlib_compression_deflate_strategy_t
#define LIST_U8                  zlib_cap_import_list_u8_t

/* Raw-DEFLATE wbits constant — negative window bits selects no header
 * (matches CPython zlib's wbits=-15 raw mode). zlib-wasm's
 * new-with-options / new-with-window-bits both accept negative values. */
#define WBITS_RAW_DEFLATE       (-15)
#define DEFAULT_MEM_LEVEL       (8)
#define ZLIB_METHOD_DEFLATE     (8)


/* --- byte<->list helpers ------------------------------------------------- */

static int bytes_to_list_u8(PyObject *data, LIST_U8 *out)
{
    Py_buffer buf;
    if (PyObject_GetBuffer(data, &buf, PyBUF_SIMPLE) < 0) return -1;
    out->ptr = (uint8_t *) PyMem_Malloc(buf.len);
    if (!out->ptr) {
        PyBuffer_Release(&buf);
        PyErr_NoMemory();
        return -1;
    }
    memcpy(out->ptr, buf.buf, buf.len);
    out->len = (size_t) buf.len;
    PyBuffer_Release(&buf);
    return 0;
}

static PyObject *list_u8_to_bytes(const LIST_U8 *lst)
{
    return PyBytes_FromStringAndSize((const char *) lst->ptr,
                                      (Py_ssize_t) lst->len);
}

static PyObject *raise_zlib_err(const char *prefix, DEFLATE_ERROR err)
{
    /* Map zlib-wasm error codes to readable messages. */
    const char *msg = "unknown error";
    switch (err) {
        case ZLIB_COMPRESSION_DEFLATE_ERROR_CODE_OK:            msg = "ok";            break;
        case ZLIB_COMPRESSION_DEFLATE_ERROR_CODE_STREAM_END:    msg = "stream end";    break;
        case ZLIB_COMPRESSION_DEFLATE_ERROR_CODE_NEED_DICT:     msg = "need dictionary"; break;
        case ZLIB_COMPRESSION_DEFLATE_ERROR_CODE_STREAM_ERROR:  msg = "stream error";  break;
        case ZLIB_COMPRESSION_DEFLATE_ERROR_CODE_DATA_ERROR:    msg = "data error";    break;
        case ZLIB_COMPRESSION_DEFLATE_ERROR_CODE_MEM_ERROR:     msg = "memory error";  break;
        case ZLIB_COMPRESSION_DEFLATE_ERROR_CODE_BUF_ERROR:     msg = "buffer error";  break;
        case ZLIB_COMPRESSION_DEFLATE_ERROR_CODE_VERSION_ERROR: msg = "version error"; break;
    }
    PyErr_Format(PyExc_RuntimeError, "%s: %s", prefix, msg);
    return NULL;
}


/* --- deflate_compress(data, level=6) -------------------------------------
 *
 * Constructs a raw-DEFLATE compressor (wbits=-15), feeds the full input,
 * flushes with FINISH. Returns the complete raw-DEFLATE stream. */

static PyObject *deflate_compress(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "level", NULL};
    PyObject *data;
    int level = 6;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i:deflate_compress",
                                     kwl, &data, &level)) return NULL;

    DEFLATE_OWN_COMPRESSOR own;
    DEFLATE_ERROR err;
    bool ok = zlib_compression_deflate_static_compressor_new_with_options(
        (int32_t) level,
        (int32_t) ZLIB_METHOD_DEFLATE,
        (int32_t) WBITS_RAW_DEFLATE,
        (int32_t) DEFAULT_MEM_LEVEL,
        (DEFLATE_STRATEGY) ZLIB_COMPRESSION_DEFLATE_STRATEGY_DEFAULT_STRATEGY,
        &own, &err);
    if (!ok) return raise_zlib_err("deflate_compress: new_with_options", err);

    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) {
        zlib_compression_deflate_compressor_drop_own(own);
        return NULL;
    }

    LIST_U8 output;
    DEFLATE_BRW_COMPRESSOR borrow =
        zlib_compression_deflate_borrow_compressor(own);
    ok = zlib_compression_deflate_method_compressor_compress_chunk(
        borrow, &input,
        (DEFLATE_FLUSH) ZLIB_COMPRESSION_DEFLATE_FLUSH_MODE_FINISH,
        &output, &err);
    zlib_compression_deflate_compressor_drop_own(own);

    if (!ok) return raise_zlib_err("deflate_compress: compress_chunk", err);
    return list_u8_to_bytes(&output);
}


/* --- deflate_decompress(data) -------------------------------------------- */

static PyObject *deflate_decompress(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", NULL};
    PyObject *data;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:deflate_decompress",
                                     kwl, &data)) return NULL;

    DEFLATE_OWN_DECOMPRESSOR own;
    DEFLATE_ERROR err;
    bool ok = zlib_compression_deflate_static_decompressor_new_with_window_bits(
        (int32_t) WBITS_RAW_DEFLATE, &own, &err);
    if (!ok) return raise_zlib_err("deflate_decompress: new_with_window_bits", err);

    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) {
        zlib_compression_deflate_decompressor_drop_own(own);
        return NULL;
    }

    LIST_U8 output;
    DEFLATE_BRW_DECOMPRESSOR borrow =
        zlib_compression_deflate_borrow_decompressor(own);
    ok = zlib_compression_deflate_method_decompressor_decompress_chunk(
        borrow, &input,
        (DEFLATE_FLUSH) ZLIB_COMPRESSION_DEFLATE_FLUSH_MODE_FINISH,
        &output, &err);
    zlib_compression_deflate_decompressor_drop_own(own);

    if (!ok) return raise_zlib_err("deflate_decompress: decompress_chunk", err);
    return list_u8_to_bytes(&output);
}


/* --- deflate_decompress_counted(data) -> (output, consumed) --------------
 *
 * Like deflate_decompress, but also returns the count of input bytes the
 * decompressor actually consumed. Used by Lib/zlib.py's gzip-trailer
 * boundary detection (Blocked-4). zlib-wasm's WIT exposes this naturally:
 * after decompress_chunk returns, total_in() reports the consumed byte
 * count. */

static PyObject *deflate_decompress_counted(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", NULL};
    PyObject *data;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:deflate_decompress_counted",
                                     kwl, &data)) return NULL;

    DEFLATE_OWN_DECOMPRESSOR own;
    DEFLATE_ERROR err;
    bool ok = zlib_compression_deflate_static_decompressor_new_with_window_bits(
        (int32_t) WBITS_RAW_DEFLATE, &own, &err);
    if (!ok) return raise_zlib_err("deflate_decompress_counted: new_with_window_bits", err);

    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) {
        zlib_compression_deflate_decompressor_drop_own(own);
        return NULL;
    }

    LIST_U8 output;
    DEFLATE_BRW_DECOMPRESSOR borrow =
        zlib_compression_deflate_borrow_decompressor(own);
    ok = zlib_compression_deflate_method_decompressor_decompress_chunk(
        borrow, &input,
        (DEFLATE_FLUSH) ZLIB_COMPRESSION_DEFLATE_FLUSH_MODE_FINISH,
        &output, &err);
    uint64_t consumed = 0;
    if (ok) {
        consumed = zlib_compression_deflate_method_decompressor_total_in(borrow);
    }
    zlib_compression_deflate_decompressor_drop_own(own);

    if (!ok) return raise_zlib_err("deflate_decompress_counted: decompress_chunk", err);
    PyObject *out_bytes = list_u8_to_bytes(&output);
    if (!out_bytes) return NULL;
    PyObject *tup = Py_BuildValue("(OK)", out_bytes, (unsigned long long) consumed);
    Py_DECREF(out_bytes);
    return tup;
}


/* --- crc32 / adler32 ------------------------------------------------------
 *
 * zlib-wasm exposes both. The *_update variant matches Python's signature
 * (existing seed value + new data → updated value). */

static PyObject *cap_crc32(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "value", NULL};
    PyObject *data;
    unsigned long value = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|k:crc32",
                                     kwl, &data, &value)) return NULL;

    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;
    uint32_t out = zlib_compression_checksum_crc32_update((uint32_t) value, &input);
    PyMem_Free(input.ptr);
    return PyLong_FromUnsignedLong((unsigned long) out);
}

static PyObject *cap_adler32(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "value", NULL};
    PyObject *data;
    unsigned long value = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|k:adler32",
                                     kwl, &data, &value)) return NULL;

    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;
    uint32_t out = zlib_compression_checksum_adler32_update((uint32_t) value, &input);
    PyMem_Free(input.ptr);
    return PyLong_FromUnsignedLong((unsigned long) out);
}


/* --- info() — sanity hook + diagnostics ---------------------------------- */

static PyObject *cap_info(PyObject *self, PyObject *Py_UNUSED(args))
{
    zlib_cap_import_string_t ver;
    zlib_compression_info_version(&ver);
    PyObject *ver_py = PyUnicode_FromStringAndSize((const char *) ver.ptr,
                                                     (Py_ssize_t) ver.len);
    return ver_py;
}


/* --- module table -------------------------------------------------------- */

#define M(name, fn, doc) {name, (PyCFunction) (fn), METH_VARARGS | METH_KEYWORDS, doc}

static PyMethodDef module_methods[] = {
    M("deflate_compress",   deflate_compress,
      "deflate_compress(data: bytes, level: int = 6) -> bytes\n"
      "Raw DEFLATE (wbits=-15) compression via zlib-wasm."),
    M("deflate_decompress", deflate_decompress,
      "deflate_decompress(data: bytes) -> bytes\n"
      "Raw INFLATE decompression."),
    M("deflate_decompress_counted", deflate_decompress_counted,
      "deflate_decompress_counted(data: bytes) -> (output: bytes, consumed: int)\n\n"
      "Like deflate_decompress, plus the count of input bytes the stream\n"
      "actually consumed. Powers Lib/zlib.py's gzip-trailer boundary detection."),
    M("crc32",   cap_crc32,
      "crc32(data: bytes, value: int = 0) -> int\n"
      "CRC-32 (IEEE 802.3) via zlib-wasm's checksum interface."),
    M("adler32", cap_adler32,
      "adler32(data: bytes, value: int = 1) -> int\n"
      "Adler-32 via zlib-wasm's checksum interface."),
    {"info", (PyCFunction) cap_info, METH_NOARGS,
     "info() -> str\nzlib library version string from the cap."},
    {NULL, NULL, 0, NULL}
};

#undef M

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "_zlib_cap",
    "Direct zlib codec access via the zlib-wasm capability component "
    "(zlib:compression@0.1.0).",
    -1,
    module_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__zlib_cap(void)
{
    return PyModule_Create(&module_def);
}

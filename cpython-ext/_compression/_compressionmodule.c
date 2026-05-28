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


/* do_decompress_counted: returns (output_bytes, consumed_input_bytes).
 * Powers `deflate_decompress_counted`, which gives the Python shim O(1)
 * access to the "where did the deflate stream end" boundary so gzip's
 * trailer parsing doesn't need binary-search post-decode. */
static PyObject *do_decompress_counted(algo_t algo, PyObject *data)
{
    compression_import_list_u8_t input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;

    tegmentum_compression_multiplexer_compression_dispatcher_own_decompressor_t own =
        tegmentum_compression_multiplexer_compression_dispatcher_constructor_decompressor(algo);

    tegmentum_compression_multiplexer_compression_dispatcher_decompress_result_t result;
    compression_import_string_t err;
    tegmentum_compression_multiplexer_compression_dispatcher_borrow_decompressor_t borrow =
        tegmentum_compression_multiplexer_compression_dispatcher_borrow_decompressor(own);
    bool is_ok = tegmentum_compression_multiplexer_compression_dispatcher_method_decompressor_decompress_counted(
        borrow, &input, &result, &err);
    tegmentum_compression_multiplexer_compression_dispatcher_decompressor_drop_own(own);

    if (!is_ok) return raise_from_wit("decompress_counted failed", &err);
    PyObject *out_bytes = list_u8_to_bytes(&result.output);
    if (out_bytes == NULL) return NULL;
    PyObject *tup = Py_BuildValue("(OK)", out_bytes, (unsigned long long) result.consumed);
    Py_DECREF(out_bytes);
    return tup;
}


static PyObject *deflate_decompress_counted(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", NULL};
    PyObject *data;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:deflate_decompress_counted",
                                     kwl, &data)) return NULL;
    return do_decompress_counted(
        TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_DEFLATE,
        data);
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

/* Zstd dictionary support -------------------------------------------------
 *
 * Routes Python's `compression.zstd` dictionary API through the multiplexer's
 * `zstd-extras` interface. Dict resources are constructed + dropped per call;
 * keeping a Python-side handle on a prepared cap-side dict would require
 * exposing the WIT resource as a Python type (PyType + capsule). The current
 * "pass bytes" model is simpler and adequate for the typical compress-once /
 * decompress-once flow — for high-volume reuse, callers can be promoted to
 * the resource-handle pattern later without breaking source compat.
 */

typedef tegmentum_compression_multiplexer_zstd_extras_own_zstd_dict_t  zd_own_t;
typedef tegmentum_compression_multiplexer_zstd_extras_borrow_zstd_dict_t zd_borrow_t;

/* Construct a zstd-dict from Python bytes, returning the owned handle.
 * On failure leaves a Python exception set and returns the zero-valued
 * handle (callers must check via PyErr_Occurred() before using). */
static zd_own_t make_zstd_dict(PyObject *dict_bytes_obj, int *ok)
{
    zd_own_t zero = {0};
    *ok = 0;
    compression_import_list_u8_t dict_list;
    if (bytes_to_list_u8(dict_bytes_obj, &dict_list) < 0) return zero;
    zd_own_t own = tegmentum_compression_multiplexer_zstd_extras_constructor_zstd_dict(&dict_list);
    *ok = 1;
    return own;
}

static PyObject *zstd_compress_with_dict(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "dict_bytes", "level", NULL};
    PyObject *data, *dict_bytes;
    int level = 3;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|i:zstd_compress_with_dict",
                                     kwl, &data, &dict_bytes, &level)) {
        return NULL;
    }
    int ok = 0;
    zd_own_t own = make_zstd_dict(dict_bytes, &ok);
    if (!ok) return NULL;
    compression_import_list_u8_t input;
    if (bytes_to_list_u8(data, &input) < 0) {
        tegmentum_compression_multiplexer_zstd_extras_zstd_dict_drop_own(own);
        return NULL;
    }
    zd_borrow_t borrow =
        tegmentum_compression_multiplexer_zstd_extras_borrow_zstd_dict(own);
    compression_import_list_u8_t output;
    compression_import_string_t err;
    bool is_ok = tegmentum_compression_multiplexer_zstd_extras_compress_with_dict(
        &input, borrow, (int32_t) level, &output, &err);
    tegmentum_compression_multiplexer_zstd_extras_zstd_dict_drop_own(own);
    if (!is_ok) return raise_from_wit("compress-with-dict failed", &err);
    return list_u8_to_bytes(&output);
}

static PyObject *zstd_decompress_with_dict(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "dict_bytes", NULL};
    PyObject *data, *dict_bytes;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO:zstd_decompress_with_dict",
                                     kwl, &data, &dict_bytes)) {
        return NULL;
    }
    int ok = 0;
    zd_own_t own = make_zstd_dict(dict_bytes, &ok);
    if (!ok) return NULL;
    compression_import_list_u8_t input;
    if (bytes_to_list_u8(data, &input) < 0) {
        tegmentum_compression_multiplexer_zstd_extras_zstd_dict_drop_own(own);
        return NULL;
    }
    zd_borrow_t borrow =
        tegmentum_compression_multiplexer_zstd_extras_borrow_zstd_dict(own);
    compression_import_list_u8_t output;
    compression_import_string_t err;
    bool is_ok = tegmentum_compression_multiplexer_zstd_extras_decompress_with_dict(
        &input, borrow, &output, &err);
    tegmentum_compression_multiplexer_zstd_extras_zstd_dict_drop_own(own);
    if (!is_ok) return raise_from_wit("decompress-with-dict failed", &err);
    return list_u8_to_bytes(&output);
}

static PyObject *zstd_dict_id(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"dict_bytes", NULL};
    PyObject *dict_bytes;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:zstd_dict_id", kwl, &dict_bytes)) {
        return NULL;
    }
    int ok = 0;
    zd_own_t own = make_zstd_dict(dict_bytes, &ok);
    if (!ok) return NULL;
    zd_borrow_t borrow =
        tegmentum_compression_multiplexer_zstd_extras_borrow_zstd_dict(own);
    uint32_t id = tegmentum_compression_multiplexer_zstd_extras_method_zstd_dict_id(borrow);
    tegmentum_compression_multiplexer_zstd_extras_zstd_dict_drop_own(own);
    return PyLong_FromUnsignedLong((unsigned long) id);
}

/* Build a list_list_u8 from a Python iterable of bytes-like. Caller owns
 * the returned struct's .ptr (free via free(); each .ptr[i].ptr is similarly
 * malloc'd). On error returns out.ptr=NULL with Python error set. */
static int build_samples_list(PyObject *samples_obj, compression_import_list_list_u8_t *out)
{
    out->ptr = NULL;
    out->len = 0;
    PyObject *fast = PySequence_Fast(samples_obj, "samples must be iterable");
    if (fast == NULL) return -1;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    if (n == 0) {
        Py_DECREF(fast);
        PyErr_SetString(PyExc_ValueError, "no samples provided");
        return -1;
    }
    out->ptr = (compression_import_list_u8_t *)
        malloc((size_t) n * sizeof(compression_import_list_u8_t));
    if (out->ptr == NULL) {
        Py_DECREF(fast);
        PyErr_NoMemory();
        return -1;
    }
    out->len = (size_t) n;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(fast, i);  /* borrowed */
        if (bytes_to_list_u8(item, &out->ptr[i]) < 0) {
            for (Py_ssize_t j = 0; j < i; j++) free(out->ptr[j].ptr);
            free(out->ptr);
            out->ptr = NULL;
            Py_DECREF(fast);
            return -1;
        }
    }
    Py_DECREF(fast);
    return 0;
}

static PyObject *zstd_train_dict(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"samples", "dict_size", NULL};
    PyObject *samples_obj;
    unsigned int dict_size;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OI:zstd_train_dict",
                                     kwl, &samples_obj, &dict_size)) {
        return NULL;
    }
    compression_import_list_list_u8_t samples;
    if (build_samples_list(samples_obj, &samples) < 0) return NULL;
    compression_import_list_u8_t output;
    compression_import_string_t err;
    bool is_ok = tegmentum_compression_multiplexer_zstd_extras_train_dict(
        &samples, (uint32_t) dict_size, &output, &err);
    /* samples buffer is consumed by the canonical ABI; do not free here. */
    if (!is_ok) return raise_from_wit("train-dict failed", &err);
    return list_u8_to_bytes(&output);
}

static PyObject *zstd_finalize_dict(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"dict_content", "samples", "dict_size", "level", NULL};
    PyObject *dict_content_obj, *samples_obj;
    unsigned int dict_size;
    int level;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OOIi:zstd_finalize_dict",
                                     kwl, &dict_content_obj, &samples_obj,
                                     &dict_size, &level)) {
        return NULL;
    }
    compression_import_list_u8_t dict_content;
    if (bytes_to_list_u8(dict_content_obj, &dict_content) < 0) return NULL;
    compression_import_list_list_u8_t samples;
    if (build_samples_list(samples_obj, &samples) < 0) {
        free(dict_content.ptr);
        return NULL;
    }
    compression_import_list_u8_t output;
    compression_import_string_t err;
    bool is_ok = tegmentum_compression_multiplexer_zstd_extras_finalize_dict(
        &dict_content, &samples, (uint32_t) dict_size, (int32_t) level,
        &output, &err);
    if (!is_ok) return raise_from_wit("finalize-dict failed", &err);
    return list_u8_to_bytes(&output);
}

static PyObject *zstd_get_frame_size(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"frame", NULL};
    PyObject *frame_obj;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:zstd_get_frame_size",
                                     kwl, &frame_obj)) {
        return NULL;
    }
    compression_import_list_u8_t frame;
    if (bytes_to_list_u8(frame_obj, &frame) < 0) return NULL;
    uint64_t size;
    compression_import_string_t err;
    bool is_ok = tegmentum_compression_multiplexer_zstd_extras_get_frame_size(
        &frame, &size, &err);
    if (!is_ok) return raise_from_wit("get-frame-size failed", &err);
    return PyLong_FromUnsignedLongLong((unsigned long long) size);
}

/* Build a list<zstd-param> from a Python iterable of (int, int) pairs.
 * Caller owns .ptr (free() it; the WIT call consumes the lift on success). */
static int build_params_list(PyObject *params_obj,
                             tegmentum_compression_multiplexer_zstd_extras_list_zstd_param_t *out)
{
    out->ptr = NULL;
    out->len = 0;
    if (params_obj == Py_None) return 0;  /* empty list is fine */
    PyObject *fast = PySequence_Fast(params_obj, "params must be iterable");
    if (fast == NULL) return -1;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    if (n == 0) {
        Py_DECREF(fast);
        return 0;
    }
    out->ptr = (tegmentum_compression_multiplexer_zstd_extras_zstd_param_t *)
        malloc((size_t) n *
               sizeof(tegmentum_compression_multiplexer_zstd_extras_zstd_param_t));
    if (out->ptr == NULL) {
        Py_DECREF(fast);
        PyErr_NoMemory();
        return -1;
    }
    out->len = (size_t) n;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(fast, i);  /* borrowed */
        PyObject *id_obj = PyTuple_GetItem(item, 0);
        PyObject *val_obj = PyTuple_GetItem(item, 1);
        if (id_obj == NULL || val_obj == NULL) {
            free(out->ptr);
            out->ptr = NULL;
            Py_DECREF(fast);
            return -1;
        }
        long id_l = PyLong_AsLong(id_obj);
        long val_l = PyLong_AsLong(val_obj);
        if (PyErr_Occurred()) {
            free(out->ptr);
            out->ptr = NULL;
            Py_DECREF(fast);
            return -1;
        }
        out->ptr[i].id    = (uint32_t) id_l;
        out->ptr[i].value = (int32_t) val_l;
    }
    Py_DECREF(fast);
    return 0;
}

static PyObject *zstd_compress_advanced(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "level", "params", NULL};
    PyObject *data, *params_obj = Py_None;
    int level = 3;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|iO:zstd_compress_advanced",
                                     kwl, &data, &level, &params_obj)) {
        return NULL;
    }
    compression_import_list_u8_t input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;
    tegmentum_compression_multiplexer_zstd_extras_list_zstd_param_t params;
    if (build_params_list(params_obj, &params) < 0) {
        free(input.ptr);
        return NULL;
    }
    compression_import_list_u8_t output;
    compression_import_string_t err;
    bool is_ok = tegmentum_compression_multiplexer_zstd_extras_compress_advanced(
        &input, (int32_t) level, &params, &output, &err);
    if (!is_ok) return raise_from_wit("compress-advanced failed", &err);
    return list_u8_to_bytes(&output);
}

static PyObject *zstd_decompress_advanced(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "params", NULL};
    PyObject *data, *params_obj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|O:zstd_decompress_advanced",
                                     kwl, &data, &params_obj)) {
        return NULL;
    }
    compression_import_list_u8_t input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;
    tegmentum_compression_multiplexer_zstd_extras_list_zstd_param_t params;
    if (build_params_list(params_obj, &params) < 0) {
        free(input.ptr);
        return NULL;
    }
    compression_import_list_u8_t output;
    compression_import_string_t err;
    bool is_ok = tegmentum_compression_multiplexer_zstd_extras_decompress_advanced(
        &input, &params, &output, &err);
    if (!is_ok) return raise_from_wit("decompress-advanced failed", &err);
    return list_u8_to_bytes(&output);
}

/* Combined advanced + dict path. Single WIT call that constructs the
 * temporary zstd-dict resource, applies params, loads the dict into the
 * CCtx/DCtx, and runs compress2/decompressDCtx. */

static PyObject *zstd_compress_advanced_with_dict(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "dict_bytes", "level", "params", NULL};
    PyObject *data, *dict_bytes, *params_obj = Py_None;
    int level = 3;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|iO:zstd_compress_advanced_with_dict",
                                     kwl, &data, &dict_bytes, &level, &params_obj)) {
        return NULL;
    }
    int ok = 0;
    zd_own_t own = make_zstd_dict(dict_bytes, &ok);
    if (!ok) return NULL;
    compression_import_list_u8_t input;
    if (bytes_to_list_u8(data, &input) < 0) {
        tegmentum_compression_multiplexer_zstd_extras_zstd_dict_drop_own(own);
        return NULL;
    }
    tegmentum_compression_multiplexer_zstd_extras_list_zstd_param_t params;
    if (build_params_list(params_obj, &params) < 0) {
        free(input.ptr);
        tegmentum_compression_multiplexer_zstd_extras_zstd_dict_drop_own(own);
        return NULL;
    }
    zd_borrow_t borrow =
        tegmentum_compression_multiplexer_zstd_extras_borrow_zstd_dict(own);
    compression_import_list_u8_t output;
    compression_import_string_t err;
    bool is_ok = tegmentum_compression_multiplexer_zstd_extras_compress_advanced_with_dict(
        &input, (int32_t) level, &params, borrow, &output, &err);
    tegmentum_compression_multiplexer_zstd_extras_zstd_dict_drop_own(own);
    if (!is_ok) return raise_from_wit("compress-advanced-with-dict failed", &err);
    return list_u8_to_bytes(&output);
}

static PyObject *zstd_decompress_advanced_with_dict(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "dict_bytes", "params", NULL};
    PyObject *data, *dict_bytes, *params_obj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|O:zstd_decompress_advanced_with_dict",
                                     kwl, &data, &dict_bytes, &params_obj)) {
        return NULL;
    }
    int ok = 0;
    zd_own_t own = make_zstd_dict(dict_bytes, &ok);
    if (!ok) return NULL;
    compression_import_list_u8_t input;
    if (bytes_to_list_u8(data, &input) < 0) {
        tegmentum_compression_multiplexer_zstd_extras_zstd_dict_drop_own(own);
        return NULL;
    }
    tegmentum_compression_multiplexer_zstd_extras_list_zstd_param_t params;
    if (build_params_list(params_obj, &params) < 0) {
        free(input.ptr);
        tegmentum_compression_multiplexer_zstd_extras_zstd_dict_drop_own(own);
        return NULL;
    }
    zd_borrow_t borrow =
        tegmentum_compression_multiplexer_zstd_extras_borrow_zstd_dict(own);
    compression_import_list_u8_t output;
    compression_import_string_t err;
    bool is_ok = tegmentum_compression_multiplexer_zstd_extras_decompress_advanced_with_dict(
        &input, &params, borrow, &output, &err);
    tegmentum_compression_multiplexer_zstd_extras_zstd_dict_drop_own(own);
    if (!is_ok) return raise_from_wit("decompress-advanced-with-dict failed", &err);
    return list_u8_to_bytes(&output);
}


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
    M("deflate_decompress_counted", deflate_decompress_counted,
      "deflate_decompress_counted(data: bytes) -> (output: bytes, consumed: int)\n\n"
      "Like deflate_decompress, but also returns the number of input bytes\n"
      "the deflate stream actually consumed. The remaining data[consumed:]\n"
      "is unused (gzip trailer, next deflate member, or garbage). Enables\n"
      "O(1) trailer detection instead of the binary-search workaround in\n"
      "Lib/zlib.py."),
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
    /* Zstd dictionary support — routes to the zstd-extras WIT interface. */
    M("zstd_compress_with_dict",   zstd_compress_with_dict,
      "zstd_compress_with_dict(data: bytes, dict_bytes: bytes, level: int = 3) -> bytes"),
    M("zstd_decompress_with_dict", zstd_decompress_with_dict,
      "zstd_decompress_with_dict(data: bytes, dict_bytes: bytes) -> bytes"),
    M("zstd_dict_id",              zstd_dict_id,
      "zstd_dict_id(dict_bytes: bytes) -> int\n"
      "Read the embedded dict ID; 0 for raw dicts."),
    M("zstd_train_dict",           zstd_train_dict,
      "zstd_train_dict(samples: Iterable[bytes], dict_size: int) -> bytes\n"
      "Train a zstd dictionary from samples."),
    M("zstd_finalize_dict",        zstd_finalize_dict,
      "zstd_finalize_dict(dict_content: bytes, samples: Iterable[bytes],\n"
      "                    dict_size: int, level: int) -> bytes\n"
      "Refine a custom-content dictionary with sample statistics."),
    M("zstd_get_frame_size",       zstd_get_frame_size,
      "zstd_get_frame_size(frame: bytes) -> int\n"
      "Size of the first zstd frame in `frame` (libzstd ZSTD_findFrameCompressedSize)."),
    M("zstd_compress_advanced",    zstd_compress_advanced,
      "zstd_compress_advanced(data: bytes, level: int = 3,\n"
      "                        params: Iterable[tuple[int, int]] = None) -> bytes\n"
      "Compress with libzstd advanced API: list of (ZSTD_cParameter id, value)."),
    M("zstd_decompress_advanced",  zstd_decompress_advanced,
      "zstd_decompress_advanced(data: bytes,\n"
      "                          params: Iterable[tuple[int, int]] = None) -> bytes\n"
      "Decompress with libzstd advanced API: list of (ZSTD_dParameter id, value)."),
    M("zstd_compress_advanced_with_dict",   zstd_compress_advanced_with_dict,
      "zstd_compress_advanced_with_dict(data, dict_bytes, level=3, params=None) -> bytes\n"
      "Advanced compress + dictionary in one call."),
    M("zstd_decompress_advanced_with_dict", zstd_decompress_advanced_with_dict,
      "zstd_decompress_advanced_with_dict(data, dict_bytes, params=None) -> bytes\n"
      "Advanced decompress + dictionary in one call."),
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

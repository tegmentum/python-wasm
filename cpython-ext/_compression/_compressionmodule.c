/* _compression — Python C extension routing to the compression-multiplexer
 * capability over the Component Model.
 *
 * Statically linked into wasi-sdk CPython (see Modules/Setup.local). The
 * imported WIT functions appear as wasm imports on python-component.wasm,
 * satisfied at compose time by wac/composectl plugging
 * compression_multiplexer.wasm into the import.
 *
 * Python surface (intentionally minimal — the public `zlib` API is built on
 * top of this in Lib/zlib.py):
 *   _compression.deflate_raw(data: bytes, level: int = 6) -> bytes
 *   _compression.inflate_raw(data: bytes) -> bytes
 *
 * Both call DEFLATE via the multiplexer (`algorithm.deflate`). Raw — no
 * RFC 1950 zlib header/trailer; the Python wrapper adds those.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "gen/compression_import.h"

/* Helpers ----------------------------------------------------------------- */

/* Borrow a Python `bytes` as a wit-bindgen list<u8> (no copy, valid for the
 * duration of the call). The component-model function may consume / free its
 * input, so we duplicate into a malloc'd buffer that ownership is handed to
 * the canonical ABI. */
static int bytes_to_list_u8(PyObject *src,
                            compression_import_list_u8_t *out)
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

/* deflate_raw / inflate_raw ---------------------------------------------- */

static PyObject *deflate_raw(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwlist[] = {"data", "level", NULL};
    PyObject *data;
    int level = 6;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i:deflate_raw", kwlist,
                                     &data, &level)) {
        return NULL;
    }
    if (level < 0 || level > 22) {
        PyErr_SetString(PyExc_ValueError, "level out of range");
        return NULL;
    }

    compression_import_list_u8_t input;
    if (bytes_to_list_u8(data, &input) < 0) {
        return NULL;
    }

    tegmentum_compression_multiplexer_compression_dispatcher_own_compressor_t own =
        tegmentum_compression_multiplexer_compression_dispatcher_constructor_compressor(
            TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_DEFLATE,
            (uint8_t) level);

    /* compress consumes `input` (canonical ABI: caller-owned -> callee). The
     * wit-bindgen-c result<T,E> convention returns `true` on Ok (ret valid),
     * `false` on Err (err valid). */
    compression_import_list_u8_t output;
    compression_import_string_t err;
    tegmentum_compression_multiplexer_compression_dispatcher_borrow_compressor_t borrow =
        tegmentum_compression_multiplexer_compression_dispatcher_borrow_compressor(own);
    bool is_ok = tegmentum_compression_multiplexer_compression_dispatcher_method_compressor_compress(
        borrow, &input, &output, &err);
    tegmentum_compression_multiplexer_compression_dispatcher_compressor_drop_own(own);

    if (!is_ok) {
        PyObject *e = PyUnicode_FromStringAndSize((const char *) err.ptr,
                                                  (Py_ssize_t) err.len);
        compression_import_string_free(&err);
        PyErr_SetObject(PyExc_RuntimeError,
                        e ? e : PyUnicode_FromString("compress failed"));
        Py_XDECREF(e);
        return NULL;
    }
    return list_u8_to_bytes(&output);
}

static PyObject *inflate_raw(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwlist[] = {"data", NULL};
    PyObject *data;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:inflate_raw", kwlist,
                                     &data)) {
        return NULL;
    }

    compression_import_list_u8_t input;
    if (bytes_to_list_u8(data, &input) < 0) {
        return NULL;
    }

    tegmentum_compression_multiplexer_compression_dispatcher_own_decompressor_t own =
        tegmentum_compression_multiplexer_compression_dispatcher_constructor_decompressor(
            TEGMENTUM_COMPRESSION_MULTIPLEXER_COMPRESSION_DISPATCHER_ALGORITHM_DEFLATE);

    compression_import_list_u8_t output;
    compression_import_string_t err;
    tegmentum_compression_multiplexer_compression_dispatcher_borrow_decompressor_t borrow =
        tegmentum_compression_multiplexer_compression_dispatcher_borrow_decompressor(own);
    bool is_ok = tegmentum_compression_multiplexer_compression_dispatcher_method_decompressor_decompress(
        borrow, &input, &output, &err);
    tegmentum_compression_multiplexer_compression_dispatcher_decompressor_drop_own(own);

    if (!is_ok) {
        PyObject *e = PyUnicode_FromStringAndSize((const char *) err.ptr,
                                                  (Py_ssize_t) err.len);
        compression_import_string_free(&err);
        PyErr_SetObject(PyExc_RuntimeError,
                        e ? e : PyUnicode_FromString("decompress failed"));
        Py_XDECREF(e);
        return NULL;
    }
    return list_u8_to_bytes(&output);
}

/* Module plumbing --------------------------------------------------------- */

static PyMethodDef compression_methods[] = {
    {"deflate_raw", (PyCFunction) deflate_raw, METH_VARARGS | METH_KEYWORDS,
     "Raw DEFLATE compress via the compression-multiplexer capability.\n\n"
     "deflate_raw(data: bytes, level: int = 6) -> bytes"},
    {"inflate_raw", (PyCFunction) inflate_raw, METH_VARARGS | METH_KEYWORDS,
     "Raw INFLATE decompress via the compression-multiplexer capability.\n\n"
     "inflate_raw(data: bytes) -> bytes"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef compression_module = {
    PyModuleDef_HEAD_INIT,
    "_compression",
    "CPython binding for the tegmentum:compression-multiplexer WIT capability.\n"
    "The actual DEFLATE codec is provided by composing in compression_multiplexer.wasm\n"
    "at component-build time; this module only exposes raw deflate/inflate.",
    -1,
    compression_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__compression(void)
{
    return PyModule_Create(&compression_module);
}

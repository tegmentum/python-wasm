/* _bz2_cap — Python C extension routing bzip2 codec calls through
 * the bzip2-wasm capability component (bzip2:compression@0.1.0).
 *
 * Replaces the compression-multiplexer-routed _compress_cap.bzip2_*
 * path. Exposes the same function names so Lib/bz2.py migration is a
 * single search-and-replace.
 *
 * Surface:
 *   _bz2_cap.bzip2_compress(data: bytes, level: int = 9) -> bytes
 *   _bz2_cap.bzip2_decompress(data: bytes) -> bytes
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include "gen/bz2_cap_import.h"

#define LIST_U8 bz2_cap_import_list_u8_t
#define ERR     bzip2_compression_simple_error_code_t
#define LEVEL   bzip2_compression_simple_compression_level_t

static int bytes_to_list_u8(PyObject *data, LIST_U8 *out)
{
    Py_buffer buf;
    if (PyObject_GetBuffer(data, &buf, PyBUF_SIMPLE) < 0) return -1;
    out->ptr = (uint8_t *) PyMem_Malloc(buf.len);
    if (!out->ptr) { PyBuffer_Release(&buf); PyErr_NoMemory(); return -1; }
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

static PyObject *raise_err(const char *prefix, ERR err)
{
    PyErr_Format(PyExc_RuntimeError, "%s: bzip2 error %u", prefix, (unsigned) err);
    return NULL;
}

static LEVEL int_level_to_wit(int level)
{
    /* bzip2 has 3 named levels in our WIT. Map best-effort. */
    if (level <= 1) return BZIP2_COMPRESSION_STREAMING_COMPRESSION_LEVEL_BEST_SPEED;
    if (level >= 9) return BZIP2_COMPRESSION_STREAMING_COMPRESSION_LEVEL_BEST_COMPRESSION;
    return BZIP2_COMPRESSION_STREAMING_COMPRESSION_LEVEL_DEFAULT_COMPRESSION;
}


static PyObject *bzip2_compress_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "level", NULL};
    PyObject *data;
    int level = 9;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i:bzip2_compress",
                                     kwl, &data, &level)) return NULL;

    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;

    LIST_U8 output;
    ERR err;
    bool ok = bzip2_compression_simple_compress(&input, int_level_to_wit(level),
                                                 &output, &err);
    PyMem_Free(input.ptr);
    if (!ok) return raise_err("bzip2_compress", err);
    return list_u8_to_bytes(&output);
}

static PyObject *bzip2_decompress_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", NULL};
    PyObject *data;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:bzip2_decompress",
                                     kwl, &data)) return NULL;

    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;

    LIST_U8 output;
    ERR err;
    bool ok = bzip2_compression_simple_decompress(&input, &output, &err);
    PyMem_Free(input.ptr);
    if (!ok) return raise_err("bzip2_decompress", err);
    return list_u8_to_bytes(&output);
}

#define M(name, fn, doc) {name, (PyCFunction) (fn), METH_VARARGS | METH_KEYWORDS, doc}

static PyMethodDef module_methods[] = {
    M("bzip2_compress",   bzip2_compress_py,
      "bzip2_compress(data: bytes, level: int = 9) -> bytes\n"
      "One-shot bzip2 compression via bzip2-wasm."),
    M("bzip2_decompress", bzip2_decompress_py,
      "bzip2_decompress(data: bytes) -> bytes\n"
      "One-shot bzip2 decompression."),
    {NULL, NULL, 0, NULL}
};
#undef M

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "_bz2_cap",
    "Direct bzip2 codec access via the bzip2-wasm capability "
    "(bzip2:compression@0.1.0).",
    -1,
    module_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__bz2_cap(void)
{
    return PyModule_Create(&module_def);
}

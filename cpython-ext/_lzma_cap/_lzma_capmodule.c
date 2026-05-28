/* _lzma_cap — Python C extension routing lzma codec through
 * the lzma-wasm capability (lzma:compression@0.1.0).
 *
 * Surface:
 *   _lzma_cap.lzma_compress(data, level=6) -> bytes
 *   _lzma_cap.lzma_decompress(data) -> bytes
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include "gen/lzma_cap_import.h"

#define LIST_U8 lzma_cap_import_list_u8_t
#define ERR     lzma_compression_simple_error_code_t
#define LEVEL   lzma_compression_simple_compression_level_t

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
    PyErr_Format(PyExc_RuntimeError, "%s: lzma error %u", prefix, (unsigned) err);
    return NULL;
}

static LEVEL int_level_to_wit(int level)
{
    if (level <= 1) return LZMA_COMPRESSION_STREAMING_COMPRESSION_LEVEL_BEST_SPEED;
    if (level >= 9) return LZMA_COMPRESSION_STREAMING_COMPRESSION_LEVEL_BEST_COMPRESSION;
    return LZMA_COMPRESSION_STREAMING_COMPRESSION_LEVEL_DEFAULT_COMPRESSION;
}


static PyObject *lzma_compress_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "level", NULL};
    PyObject *data;
    int level = 6;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i:lzma_compress",
                                     kwl, &data, &level)) return NULL;

    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;

    LIST_U8 output;
    ERR err;
    bool ok = lzma_compression_simple_compress(&input, int_level_to_wit(level),
                                                &output, &err);
    PyMem_Free(input.ptr);
    if (!ok) return raise_err("lzma_compress", err);
    return list_u8_to_bytes(&output);
}

static PyObject *lzma_decompress_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", NULL};
    PyObject *data;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:lzma_decompress",
                                     kwl, &data)) return NULL;

    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;

    LIST_U8 output;
    ERR err;
    bool ok = lzma_compression_simple_decompress(&input, &output, &err);
    PyMem_Free(input.ptr);
    if (!ok) return raise_err("lzma_decompress", err);
    return list_u8_to_bytes(&output);
}

#define M(name, fn, doc) {name, (PyCFunction) (fn), METH_VARARGS | METH_KEYWORDS, doc}

static PyMethodDef module_methods[] = {
    M("lzma_compress",   lzma_compress_py,
      "lzma_compress(data: bytes, level: int = 6) -> bytes\n"
      "One-shot XZ-format compression via lzma-wasm."),
    M("lzma_decompress", lzma_decompress_py,
      "lzma_decompress(data: bytes) -> bytes\n"
      "One-shot XZ decompression."),
    {NULL, NULL, 0, NULL}
};
#undef M

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "_lzma_cap",
    "Direct lzma codec access via the lzma-wasm capability "
    "(lzma:compression@0.1.0).",
    -1,
    module_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__lzma_cap(void)
{
    return PyModule_Create(&module_def);
}

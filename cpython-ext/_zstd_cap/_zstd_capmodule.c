/* _zstd_cap — Python C extension routing zstd codec through the
 * zstd-wasm capability (zstd:compression@0.1.0).
 *
 * Phase A scope: compress + decompress. The shim's dict/training/
 * advanced operations are left to fall back to NotImplementedError
 * (or, transitionally, the multiplexer-routed _compress_cap path)
 * until a follow-up implements them here against zstd-wasm's
 * advanced interface.
 *
 * Surface:
 *   _zstd_cap.zstd_compress(data: bytes, level: int = 3) -> bytes
 *   _zstd_cap.zstd_decompress(data: bytes) -> bytes
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include "gen/zstd_cap_import.h"

#define LIST_U8 zstd_cap_import_list_u8_t
#define ERR     zstd_compression_simple_error_code_t
#define LEVEL   zstd_compression_simple_compression_level_t

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
    PyErr_Format(PyExc_RuntimeError, "%s: zstd error %u", prefix, (unsigned) err);
    return NULL;
}

static LEVEL int_level_to_wit(int level)
{
    if (level <= 1)  return ZSTD_COMPRESSION_STREAMING_COMPRESSION_LEVEL_BEST_SPEED;
    if (level >= 19) return ZSTD_COMPRESSION_STREAMING_COMPRESSION_LEVEL_BEST_COMPRESSION;
    return ZSTD_COMPRESSION_STREAMING_COMPRESSION_LEVEL_DEFAULT_COMPRESSION;
}


static PyObject *zstd_compress_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "level", NULL};
    PyObject *data;
    int level = 3;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i:zstd_compress",
                                     kwl, &data, &level)) return NULL;

    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;

    LIST_U8 output;
    ERR err;
    bool ok = zstd_compression_simple_compress(&input, int_level_to_wit(level),
                                                &output, &err);
    PyMem_Free(input.ptr);
    if (!ok) return raise_err("zstd_compress", err);
    return list_u8_to_bytes(&output);
}

static PyObject *zstd_decompress_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", NULL};
    PyObject *data;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:zstd_decompress",
                                     kwl, &data)) return NULL;

    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;

    LIST_U8 output;
    ERR err;
    bool ok = zstd_compression_simple_decompress(&input, &output, &err);
    PyMem_Free(input.ptr);
    if (!ok) return raise_err("zstd_decompress", err);
    return list_u8_to_bytes(&output);
}

#define M(name, fn, doc) {name, (PyCFunction) (fn), METH_VARARGS | METH_KEYWORDS, doc}

static PyMethodDef module_methods[] = {
    M("zstd_compress",   zstd_compress_py,
      "zstd_compress(data: bytes, level: int = 3) -> bytes\n"
      "One-shot zstd compression via zstd-wasm."),
    M("zstd_decompress", zstd_decompress_py,
      "zstd_decompress(data: bytes) -> bytes\n"
      "One-shot zstd decompression."),
    {NULL, NULL, 0, NULL}
};
#undef M

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "_zstd_cap",
    "Direct zstd codec access via the zstd-wasm capability "
    "(zstd:compression@0.1.0). Phase A: compress/decompress; dicts + "
    "advanced API are follow-ups.",
    -1,
    module_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__zstd_cap(void)
{
    return PyModule_Create(&module_def);
}

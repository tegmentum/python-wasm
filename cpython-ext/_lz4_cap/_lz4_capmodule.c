/* _lz4_cap — Python C extension routing lz4 through the lz4-wasm
 * capability (lz4:compression@0.1.0). LZ4 isn't a CPython stdlib
 * module, so this extension is consumed directly by users who
 * `import _lz4_cap`.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include "gen/lz4_cap_import.h"

#define LIST_U8 lz4_cap_import_list_u8_t
#define ERR     lz4_compression_simple_error_code_t
#define LEVEL   lz4_compression_simple_compression_level_t

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
    return PyBytes_FromStringAndSize((const char *) lst->ptr, (Py_ssize_t) lst->len);
}

static PyObject *raise_err(const char *prefix, ERR err)
{
    PyErr_Format(PyExc_RuntimeError, "%s: lz4 error %u", prefix, (unsigned) err);
    return NULL;
}

static LEVEL int_level_to_wit(int level)
{
    /* lz4_flex doesn't honor levels in its simple API; map all-the-same. */
    if (level >= 9) return LZ4_COMPRESSION_STREAMING_COMPRESSION_LEVEL_BEST_COMPRESSION;
    return LZ4_COMPRESSION_STREAMING_COMPRESSION_LEVEL_DEFAULT_COMPRESSION;
}

static PyObject *lz4_compress_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "level", NULL};
    PyObject *data; int level = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i:lz4_compress",
                                     kwl, &data, &level)) return NULL;
    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;
    LIST_U8 output;
    ERR err;
    bool ok = lz4_compression_simple_compress(&input, int_level_to_wit(level), &output, &err);
    PyMem_Free(input.ptr);
    if (!ok) return raise_err("lz4_compress", err);
    return list_u8_to_bytes(&output);
}

static PyObject *lz4_decompress_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", NULL};
    PyObject *data;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:lz4_decompress", kwl, &data)) return NULL;
    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;
    LIST_U8 output;
    ERR err;
    bool ok = lz4_compression_simple_decompress(&input, &output, &err);
    PyMem_Free(input.ptr);
    if (!ok) return raise_err("lz4_decompress", err);
    return list_u8_to_bytes(&output);
}

#define M(name, fn, doc) {name, (PyCFunction)(fn), METH_VARARGS | METH_KEYWORDS, doc}

static PyMethodDef module_methods[] = {
    M("lz4_compress",   lz4_compress_py,   "lz4_compress(data, level=0) -> bytes"),
    M("lz4_decompress", lz4_decompress_py, "lz4_decompress(data) -> bytes"),
    {NULL, NULL, 0, NULL}
};
#undef M

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT, "_lz4_cap",
    "Direct LZ4 codec access via the lz4-wasm capability (lz4:compression@0.1.0).",
    -1, module_methods, NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__lz4_cap(void) { return PyModule_Create(&module_def); }

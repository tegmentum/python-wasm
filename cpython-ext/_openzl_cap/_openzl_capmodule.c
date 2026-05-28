/* _openzl_cap — Python C extension routing openzl through the
 * openzl-wasm capability (openzl:compression@0.1.0). Not a stdlib
 * module; users `import _openzl_cap` directly.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include "gen/openzl_cap_import.h"

#define LIST_U8 openzl_cap_import_list_u8_t
#define ERR     openzl_compression_simple_error_code_t
#define LEVEL   openzl_compression_simple_compression_level_t

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
    PyErr_Format(PyExc_RuntimeError, "%s: openzl error %u", prefix, (unsigned) err);
    return NULL;
}

static LEVEL int_level_to_wit(int level)
{
    if (level <= 1) return OPENZL_COMPRESSION_STREAMING_COMPRESSION_LEVEL_BEST_SPEED;
    if (level >= 9) return OPENZL_COMPRESSION_STREAMING_COMPRESSION_LEVEL_BEST_COMPRESSION;
    return OPENZL_COMPRESSION_STREAMING_COMPRESSION_LEVEL_DEFAULT_COMPRESSION;
}

static PyObject *openzl_compress_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "level", NULL};
    PyObject *data; int level = 3;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i:openzl_compress",
                                     kwl, &data, &level)) return NULL;
    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;
    LIST_U8 output;
    ERR err;
    bool ok = openzl_compression_simple_compress(&input, int_level_to_wit(level), &output, &err);
    PyMem_Free(input.ptr);
    if (!ok) return raise_err("openzl_compress", err);
    return list_u8_to_bytes(&output);
}

static PyObject *openzl_decompress_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", NULL};
    PyObject *data;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:openzl_decompress",
                                     kwl, &data)) return NULL;
    LIST_U8 input;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;
    LIST_U8 output;
    ERR err;
    bool ok = openzl_compression_simple_decompress(&input, &output, &err);
    PyMem_Free(input.ptr);
    if (!ok) return raise_err("openzl_decompress", err);
    return list_u8_to_bytes(&output);
}

#define M(name, fn, doc) {name, (PyCFunction)(fn), METH_VARARGS | METH_KEYWORDS, doc}

static PyMethodDef module_methods[] = {
    M("openzl_compress",   openzl_compress_py,
      "openzl_compress(data, level=3) -> bytes"),
    M("openzl_decompress", openzl_decompress_py,
      "openzl_decompress(data) -> bytes"),
    {NULL, NULL, 0, NULL}
};
#undef M

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT, "_openzl_cap",
    "Direct OpenZL codec access via the openzl-wasm capability (openzl:compression@0.1.0).",
    -1, module_methods, NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__openzl_cap(void) { return PyModule_Create(&module_def); }

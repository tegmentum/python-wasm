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


/* --- Advanced: dictionaries + frame inspection ---------------------------
 *
 * The shim calls these for ZstdDict construction and dict-based compress/
 * decompress paths. Train-dict + compress-advanced (zstd-param API) stay
 * deferred at the cap side; this extension exposes them as
 * NotImplementedError-raising stubs so the shim raises a clear message
 * to the caller.
 *
 * Convention for cap-side errors: zstd-wasm's advanced interface returns
 * `result<list<u8>, string>` (string error), so the raise helper takes
 * a string slice from the WIT side rather than an error-code enum.
 */

#define STRING zstd_cap_import_string_t

static PyObject *raise_string_err(const char *prefix, STRING *err)
{
    PyErr_Format(PyExc_RuntimeError, "%s: %.*s",
                 prefix, (int) err->len, (const char *) err->ptr);
    zstd_cap_import_string_free(err);
    return NULL;
}


static PyObject *zstd_dict_id_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"bytes", NULL};
    PyObject *bytes;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:zstd_dict_id", kwl, &bytes))
        return NULL;

    LIST_U8 dict_bytes;
    if (bytes_to_list_u8(bytes, &dict_bytes) < 0) return NULL;

    /* Construct a temporary ZstdDict resource, query id, drop. */
    zstd_compression_advanced_own_zstd_dict_t own =
        zstd_compression_advanced_constructor_zstd_dict(&dict_bytes);
    zstd_compression_advanced_borrow_zstd_dict_t borrow =
        zstd_compression_advanced_borrow_zstd_dict(own);
    uint32_t id = zstd_compression_advanced_method_zstd_dict_id(borrow);
    zstd_compression_advanced_zstd_dict_drop_own(own);
    /* dict_bytes.ptr is owned by us — free it after the WIT call so the
     * cap-side gets a copy. */
    PyMem_Free(dict_bytes.ptr);

    return PyLong_FromUnsignedLong((unsigned long) id);
}


static PyObject *zstd_compress_with_dict_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "dict_bytes", "level", NULL};
    PyObject *data, *dict_bytes_obj;
    int level = 3;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|i:zstd_compress_with_dict",
                                     kwl, &data, &dict_bytes_obj, &level)) return NULL;

    LIST_U8 input, dict_bytes;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;
    if (bytes_to_list_u8(dict_bytes_obj, &dict_bytes) < 0) {
        PyMem_Free(input.ptr); return NULL;
    }

    zstd_compression_advanced_own_zstd_dict_t own_dict =
        zstd_compression_advanced_constructor_zstd_dict(&dict_bytes);
    zstd_compression_advanced_borrow_zstd_dict_t borrow_dict =
        zstd_compression_advanced_borrow_zstd_dict(own_dict);

    LIST_U8 output;
    STRING err;
    bool ok = zstd_compression_advanced_compress_with_dict(
        &input, borrow_dict, (int32_t) level, &output, &err);

    zstd_compression_advanced_zstd_dict_drop_own(own_dict);
    PyMem_Free(input.ptr);
    PyMem_Free(dict_bytes.ptr);

    if (!ok) return raise_string_err("zstd_compress_with_dict", &err);
    return list_u8_to_bytes(&output);
}


static PyObject *zstd_decompress_with_dict_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "dict_bytes", NULL};
    PyObject *data, *dict_bytes_obj;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO:zstd_decompress_with_dict",
                                     kwl, &data, &dict_bytes_obj)) return NULL;

    LIST_U8 input, dict_bytes;
    if (bytes_to_list_u8(data, &input) < 0) return NULL;
    if (bytes_to_list_u8(dict_bytes_obj, &dict_bytes) < 0) {
        PyMem_Free(input.ptr); return NULL;
    }

    zstd_compression_advanced_own_zstd_dict_t own_dict =
        zstd_compression_advanced_constructor_zstd_dict(&dict_bytes);
    zstd_compression_advanced_borrow_zstd_dict_t borrow_dict =
        zstd_compression_advanced_borrow_zstd_dict(own_dict);

    LIST_U8 output;
    STRING err;
    bool ok = zstd_compression_advanced_decompress_with_dict(
        &input, borrow_dict, &output, &err);

    zstd_compression_advanced_zstd_dict_drop_own(own_dict);
    PyMem_Free(input.ptr);
    PyMem_Free(dict_bytes.ptr);

    if (!ok) return raise_string_err("zstd_decompress_with_dict", &err);
    return list_u8_to_bytes(&output);
}


static PyObject *zstd_get_frame_size_py(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"frame", NULL};
    PyObject *frame;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O:zstd_get_frame_size",
                                     kwl, &frame)) return NULL;

    LIST_U8 input;
    if (bytes_to_list_u8(frame, &input) < 0) return NULL;

    uint64_t size;
    STRING err;
    bool ok = zstd_compression_advanced_get_frame_size(&input, &size, &err);
    PyMem_Free(input.ptr);

    if (!ok) return raise_string_err("zstd_get_frame_size", &err);
    return PyLong_FromUnsignedLongLong((unsigned long long) size);
}


/* train-dict / finalize-dict / advanced-with-params return clear errors
 * for callers; not yet implemented in zstd-wasm. */

static PyObject *zstd_train_dict_py(PyObject *self, PyObject *args, PyObject *kw)
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "zstd_train_dict not implemented in zstd-wasm yet "
                    "(requires zstd-safe direct binding)");
    return NULL;
}

static PyObject *zstd_finalize_dict_py(PyObject *self, PyObject *args, PyObject *kw)
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "zstd_finalize_dict not implemented");
    return NULL;
}

static PyObject *zstd_compress_advanced_py(PyObject *self, PyObject *args, PyObject *kw)
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "zstd_compress_advanced (raw zstd-param API) not implemented");
    return NULL;
}

static PyObject *zstd_decompress_advanced_py(PyObject *self, PyObject *args, PyObject *kw)
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "zstd_decompress_advanced (raw zstd-param API) not implemented");
    return NULL;
}

static PyObject *zstd_compress_advanced_with_dict_py(PyObject *self, PyObject *args, PyObject *kw)
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "zstd_compress_advanced_with_dict not implemented");
    return NULL;
}

static PyObject *zstd_decompress_advanced_with_dict_py(PyObject *self, PyObject *args, PyObject *kw)
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "zstd_decompress_advanced_with_dict not implemented");
    return NULL;
}

#define M(name, fn, doc) {name, (PyCFunction) (fn), METH_VARARGS | METH_KEYWORDS, doc}

static PyMethodDef module_methods[] = {
    M("zstd_compress",   zstd_compress_py,
      "zstd_compress(data: bytes, level: int = 3) -> bytes\n"
      "One-shot zstd compression via zstd-wasm."),
    M("zstd_decompress", zstd_decompress_py,
      "zstd_decompress(data: bytes) -> bytes\n"
      "One-shot zstd decompression."),
    M("zstd_dict_id", zstd_dict_id_py,
      "zstd_dict_id(bytes) -> int\nDictionary ID; 0 for raw-content dicts."),
    M("zstd_compress_with_dict", zstd_compress_with_dict_py,
      "zstd_compress_with_dict(data: bytes, dict_bytes: bytes, level: int = 3) -> bytes"),
    M("zstd_decompress_with_dict", zstd_decompress_with_dict_py,
      "zstd_decompress_with_dict(data: bytes, dict_bytes: bytes) -> bytes"),
    M("zstd_get_frame_size", zstd_get_frame_size_py,
      "zstd_get_frame_size(frame: bytes) -> int"),
    /* Stubs raising NotImplementedError — cap-side not implemented yet. */
    M("zstd_train_dict",                     zstd_train_dict_py,                     "Deferred"),
    M("zstd_finalize_dict",                  zstd_finalize_dict_py,                  "Deferred"),
    M("zstd_compress_advanced",              zstd_compress_advanced_py,              "Deferred"),
    M("zstd_decompress_advanced",            zstd_decompress_advanced_py,            "Deferred"),
    M("zstd_compress_advanced_with_dict",    zstd_compress_advanced_with_dict_py,    "Deferred"),
    M("zstd_decompress_advanced_with_dict",  zstd_decompress_advanced_with_dict_py,  "Deferred"),
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

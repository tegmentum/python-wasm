/* _xxhash — Python C extension for the (non-cryptographic) hashing-multiplexer
 * capability: xxHash, CRC, MurmurHash, BLAKE3.
 *
 * Python's stdlib has no analogue for these. The module exposes:
 *
 *   _xxhash.digest(name: str, data: bytes, seed: int = 0) -> bytes
 *   _xxhash.algorithms() -> tuple[str, ...]
 *
 * Algorithm names: xxh32, xxh64, xxh3, xxh128, crc32, crc32c, murmur3,
 * murmur128, blake3.
 *
 * No streaming wrapper yet — the multiplexer's hasher resource provides one
 * but most use cases are one-shot. (Lib/xxhash.py shim, if added, would
 * surface a hashlib-like API.)
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "gen/xxhash_import.h"

typedef tegmentum_hashing_multiplexer_hashing_dispatcher_algorithm_t algo_t;

typedef struct {
    const char *name;
    algo_t      algo;
} algo_name_t;

static const algo_name_t ALGO_TABLE[] = {
    {"xxh32",     TEGMENTUM_HASHING_MULTIPLEXER_HASHING_DISPATCHER_ALGORITHM_XXH32},
    {"xxh64",     TEGMENTUM_HASHING_MULTIPLEXER_HASHING_DISPATCHER_ALGORITHM_XXH64},
    {"xxh3",      TEGMENTUM_HASHING_MULTIPLEXER_HASHING_DISPATCHER_ALGORITHM_XXH3},
    {"xxh128",    TEGMENTUM_HASHING_MULTIPLEXER_HASHING_DISPATCHER_ALGORITHM_XXH128},
    {"crc32",     TEGMENTUM_HASHING_MULTIPLEXER_HASHING_DISPATCHER_ALGORITHM_CRC32},
    {"crc32c",    TEGMENTUM_HASHING_MULTIPLEXER_HASHING_DISPATCHER_ALGORITHM_CRC32C},
    {"murmur3",   TEGMENTUM_HASHING_MULTIPLEXER_HASHING_DISPATCHER_ALGORITHM_MURMUR3},
    {"murmur128", TEGMENTUM_HASHING_MULTIPLEXER_HASHING_DISPATCHER_ALGORITHM_MURMUR128},
    {"blake3",    TEGMENTUM_HASHING_MULTIPLEXER_HASHING_DISPATCHER_ALGORITHM_BLAKE3},
};

static int name_to_algo(const char *name, algo_t *out)
{
    for (size_t i = 0; i < sizeof(ALGO_TABLE) / sizeof(ALGO_TABLE[0]); i++) {
        if (strcmp(name, ALGO_TABLE[i].name) == 0) {
            *out = ALGO_TABLE[i].algo;
            return 0;
        }
    }
    PyErr_Format(PyExc_ValueError, "unsupported hash algorithm: %s", name);
    return -1;
}

static PyObject *mod_digest(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwlist[] = {"name", "data", "seed", NULL};
    const char *name;
    PyObject *data;
    unsigned long long seed = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "sO|K:digest", kwlist,
                                     &name, &data, &seed)) {
        return NULL;
    }
    algo_t a;
    if (name_to_algo(name, &a) < 0) return NULL;

    /* Marshal input as list<u8> the canonical-ABI owns. */
    Py_buffer view;
    if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE | PyBUF_C_CONTIGUOUS) < 0) {
        return NULL;
    }
    xxhash_import_list_u8_t input;
    if (view.len > 0) {
        input.ptr = (uint8_t *) malloc((size_t) view.len);
        if (!input.ptr) { PyBuffer_Release(&view); PyErr_NoMemory(); return NULL; }
        memcpy(input.ptr, view.buf, (size_t) view.len);
    } else {
        input.ptr = NULL;
    }
    input.len = (size_t) view.len;
    PyBuffer_Release(&view);

    xxhash_import_list_u8_t out;
    tegmentum_hashing_multiplexer_hashing_dispatcher_digest(a, &input, (uint64_t) seed, &out);
    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr,
                                            (Py_ssize_t) out.len);
    xxhash_import_list_u8_free(&out);
    return r;
}

/* Helper: copy a Python buffer-protocol object into a wit list<u8>. */
static int bytes_into_list(PyObject *src, xxhash_import_list_u8_t *out)
{
    Py_buffer view;
    if (PyObject_GetBuffer(src, &view, PyBUF_SIMPLE) < 0) return -1;
    if (view.len > 0) {
        out->ptr = (uint8_t *) malloc((size_t) view.len);
        if (!out->ptr) { PyBuffer_Release(&view); PyErr_NoMemory(); return -1; }
        memcpy(out->ptr, view.buf, (size_t) view.len);
    } else {
        out->ptr = NULL;
    }
    out->len = (size_t) view.len;
    PyBuffer_Release(&view);
    return 0;
}

static PyObject *mod_blake3_keyed(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"key", "data", "output_length", NULL};
    PyObject *key; PyObject *data; unsigned int output_length = 32;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|I:blake3_keyed",
                                     kwl, &key, &data, &output_length)) return NULL;
    xxhash_import_list_u8_t key_l, data_l, out;
    xxhash_import_string_t err;
    if (bytes_into_list(key, &key_l) < 0) return NULL;
    if (bytes_into_list(data, &data_l) < 0) { if (key_l.ptr) free(key_l.ptr); return NULL; }
    bool ok = tegmentum_hashing_multiplexer_blake3_extras_keyed_digest(
        &key_l, &data_l, output_length, &out, &err);
    if (key_l.ptr) free(key_l.ptr);
    if (data_l.ptr) free(data_l.ptr);
    if (!ok) {
        PyErr_Format(PyExc_RuntimeError, "blake3_keyed: %.*s",
                     (int) err.len, (const char *) err.ptr);
        xxhash_import_string_free(&err);
        return NULL;
    }
    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr, (Py_ssize_t) out.len);
    xxhash_import_list_u8_free(&out);
    return r;
}

static PyObject *mod_blake3_derive_key(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"context", "key_material", "output_length", NULL};
    const char *context; PyObject *key_material; unsigned int output_length = 32;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "sO|I:blake3_derive_key",
                                     kwl, &context, &key_material, &output_length)) return NULL;
    xxhash_import_string_t ctx = {(uint8_t *) context, strlen(context)};
    xxhash_import_list_u8_t km_l, out;
    xxhash_import_string_t err;
    if (bytes_into_list(key_material, &km_l) < 0) return NULL;
    bool ok = tegmentum_hashing_multiplexer_blake3_extras_derive_key(
        &ctx, &km_l, output_length, &out, &err);
    if (km_l.ptr) free(km_l.ptr);
    if (!ok) {
        PyErr_Format(PyExc_RuntimeError, "blake3_derive_key: %.*s",
                     (int) err.len, (const char *) err.ptr);
        xxhash_import_string_free(&err);
        return NULL;
    }
    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr, (Py_ssize_t) out.len);
    xxhash_import_list_u8_free(&out);
    return r;
}

static PyObject *mod_blake3_xof(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "output_length", NULL};
    PyObject *data; unsigned int output_length;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OI:blake3_xof",
                                     kwl, &data, &output_length)) return NULL;
    xxhash_import_list_u8_t data_l, out;
    xxhash_import_string_t err;
    if (bytes_into_list(data, &data_l) < 0) return NULL;
    bool ok = tegmentum_hashing_multiplexer_blake3_extras_digest_xof(
        &data_l, output_length, &out, &err);
    if (data_l.ptr) free(data_l.ptr);
    if (!ok) {
        PyErr_Format(PyExc_RuntimeError, "blake3_xof: %.*s",
                     (int) err.len, (const char *) err.ptr);
        xxhash_import_string_free(&err);
        return NULL;
    }
    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr, (Py_ssize_t) out.len);
    xxhash_import_list_u8_free(&out);
    return r;
}

static PyObject *mod_xxh3_with_secret(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"data", "secret", "bits", NULL};
    PyObject *data; PyObject *secret; int bits = 64;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|i:xxh3_with_secret",
                                     kwl, &data, &secret, &bits)) return NULL;
    xxhash_import_list_u8_t data_l, secret_l, out;
    xxhash_import_string_t err;
    if (bytes_into_list(data, &data_l) < 0) return NULL;
    if (bytes_into_list(secret, &secret_l) < 0) { if (data_l.ptr) free(data_l.ptr); return NULL; }
    bool ok;
    if (bits == 64) {
        ok = tegmentum_hashing_multiplexer_xxh3_extras_digest_u64(
            &data_l, &secret_l, &out, &err);
    } else if (bits == 128) {
        ok = tegmentum_hashing_multiplexer_xxh3_extras_digest_u128(
            &data_l, &secret_l, &out, &err);
    } else {
        if (data_l.ptr) free(data_l.ptr);
        if (secret_l.ptr) free(secret_l.ptr);
        PyErr_SetString(PyExc_ValueError, "bits must be 64 or 128");
        return NULL;
    }
    if (data_l.ptr) free(data_l.ptr);
    if (secret_l.ptr) free(secret_l.ptr);
    if (!ok) {
        PyErr_Format(PyExc_RuntimeError, "xxh3_with_secret: %.*s",
                     (int) err.len, (const char *) err.ptr);
        xxhash_import_string_free(&err);
        return NULL;
    }
    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr, (Py_ssize_t) out.len);
    xxhash_import_list_u8_free(&out);
    return r;
}

static PyObject *mod_algorithms(PyObject *self, PyObject *Py_UNUSED(args))
{
    Py_ssize_t n = (Py_ssize_t) (sizeof(ALGO_TABLE) / sizeof(ALGO_TABLE[0]));
    PyObject *t = PyTuple_New(n);
    if (!t) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *s = PyUnicode_FromString(ALGO_TABLE[i].name);
        if (!s) { Py_DECREF(t); return NULL; }
        PyTuple_SET_ITEM(t, i, s);
    }
    return t;
}

static PyMethodDef module_methods[] = {
    {"digest",            (PyCFunction) mod_digest, METH_VARARGS | METH_KEYWORDS,
     "digest(name, data, seed=0) -> bytes\n\nOne-shot non-crypto hash via the hashing-multiplexer."},
    {"blake3_keyed",      (PyCFunction) mod_blake3_keyed, METH_VARARGS | METH_KEYWORDS,
     "blake3_keyed(key: bytes[32], data: bytes, output_length: int = 32) -> bytes"},
    {"blake3_derive_key", (PyCFunction) mod_blake3_derive_key, METH_VARARGS | METH_KEYWORDS,
     "blake3_derive_key(context: str, key_material: bytes, output_length: int = 32) -> bytes"},
    {"blake3_xof",        (PyCFunction) mod_blake3_xof, METH_VARARGS | METH_KEYWORDS,
     "blake3_xof(data: bytes, output_length: int) -> bytes"},
    {"xxh3_with_secret",  (PyCFunction) mod_xxh3_with_secret, METH_VARARGS | METH_KEYWORDS,
     "xxh3_with_secret(data: bytes, secret: bytes, bits: int = 64) -> bytes\n"
     "bits must be 64 or 128. secret must be >= 136 bytes."},
    {"algorithms",        mod_algorithms, METH_NOARGS,
     "algorithms() -> tuple[str, ...]\n\nNames of supported algorithms."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef xxhash_module = {
    PyModuleDef_HEAD_INIT,
    "_xxhash",
    "Non-cryptographic hashing (xxhash, crc, murmur, blake3) via the\n"
    "tegmentum:hashing-multiplexer capability.",
    -1,
    module_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__xxhash(void)
{
    return PyModule_Create(&xxhash_module);
}

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
    {"digest",     (PyCFunction) mod_digest, METH_VARARGS | METH_KEYWORDS,
     "digest(name, data, seed=0) -> bytes\n\nOne-shot non-crypto hash via the hashing-multiplexer."},
    {"algorithms", mod_algorithms, METH_NOARGS,
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

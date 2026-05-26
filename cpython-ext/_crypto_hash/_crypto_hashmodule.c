/* _crypto_hash — Python C extension routing to the crypto-hash-multiplexer
 * capability over the Component Model.
 *
 * Statically linked into wasi-sdk CPython (Modules/Setup.local). The imported
 * WIT functions appear as wasm imports on python-component.wasm, satisfied at
 * compose time by wac/composectl plugging crypto_hash_multiplexer.wasm.
 *
 * Python surface:
 *   _crypto_hash.digest(name: str, data: bytes) -> bytes        # one-shot
 *   _crypto_hash.new(name: str) -> hasher                       # streaming
 *       hasher.update(data: bytes) -> None
 *       hasher.digest() -> bytes        (idempotent, hashlib semantics)
 *       hasher.hexdigest() -> str
 *       hasher.copy() -> hasher         (independent, same accumulated state)
 *       hasher.name -> str
 *       hasher.digest_size -> int
 *
 * Algorithm names match Python's hashlib: md5, sha1, sha256, sha384, sha512,
 * sha3_256, sha3_512, blake2b, blake2s.
 *
 * Design note — why the hasher buffers input rather than using the WIT
 * resource's streaming hasher.update/finish: the capability's `finish` consumes
 * the hasher (no `clone`), so making `digest()` idempotent (a hard hashlib
 * contract: `h.digest() == h.digest()`) requires either capability-side
 * cloning we don't have, or replaying input. We buffer + call the capability's
 * one-shot `digest(algo, bytes)` on every materialization. This costs memory
 * proportional to the input but is correct; it can be replaced with the
 * resource path once the WIT gains a clone primitive.
 *
 * Lib/_hashlib.py (shim, Phase 2) re-exports this as the OpenSSL-free hashlib
 * backend so `hashlib.sha256(b'abc').hexdigest()` keeps working unchanged.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "gen/crypto_hash_import.h"

typedef tegmentum_crypto_hash_multiplexer_hash_dispatcher_algorithm_t algo_t;

/* Name <-> algorithm mapping --------------------------------------------- */

typedef struct {
    const char *name;
    algo_t      algo;
} algo_name_t;

static const algo_name_t ALGO_TABLE[] = {
    {"md5",      TEGMENTUM_CRYPTO_HASH_MULTIPLEXER_HASH_DISPATCHER_ALGORITHM_MD5},
    {"sha1",     TEGMENTUM_CRYPTO_HASH_MULTIPLEXER_HASH_DISPATCHER_ALGORITHM_SHA1},
    {"sha256",   TEGMENTUM_CRYPTO_HASH_MULTIPLEXER_HASH_DISPATCHER_ALGORITHM_SHA256},
    {"sha384",   TEGMENTUM_CRYPTO_HASH_MULTIPLEXER_HASH_DISPATCHER_ALGORITHM_SHA384},
    {"sha512",   TEGMENTUM_CRYPTO_HASH_MULTIPLEXER_HASH_DISPATCHER_ALGORITHM_SHA512},
    {"sha3_256", TEGMENTUM_CRYPTO_HASH_MULTIPLEXER_HASH_DISPATCHER_ALGORITHM_SHA3256},
    {"sha3_512", TEGMENTUM_CRYPTO_HASH_MULTIPLEXER_HASH_DISPATCHER_ALGORITHM_SHA3512},
    {"blake2b",  TEGMENTUM_CRYPTO_HASH_MULTIPLEXER_HASH_DISPATCHER_ALGORITHM_BLAKE2B},
    {"blake2s",  TEGMENTUM_CRYPTO_HASH_MULTIPLEXER_HASH_DISPATCHER_ALGORITHM_BLAKE2S},
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

static const char *algo_to_name(algo_t a)
{
    for (size_t i = 0; i < sizeof(ALGO_TABLE) / sizeof(ALGO_TABLE[0]); i++) {
        if (ALGO_TABLE[i].algo == a) return ALGO_TABLE[i].name;
    }
    return "?";
}

/* Bytes <-> wit-bindgen list<u8> ----------------------------------------- */

static int bytes_to_list(PyObject *src, crypto_hash_import_list_u8_t *out)
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

static PyObject *list_to_bytes(crypto_hash_import_list_u8_t *src)
{
    PyObject *r = PyBytes_FromStringAndSize((const char *) src->ptr,
                                            (Py_ssize_t) src->len);
    crypto_hash_import_list_u8_free(src);
    return r;
}

/* One-shot helper: digest into a list_u8 (caller frees via list_to_bytes
 * or crypto_hash_import_list_u8_free). */
static int digest_capability(algo_t a, const uint8_t *data, size_t len,
                              crypto_hash_import_list_u8_t *out)
{
    crypto_hash_import_list_u8_t input;
    if (len > 0) {
        input.ptr = (uint8_t *) malloc(len);
        if (!input.ptr) { PyErr_NoMemory(); return -1; }
        memcpy(input.ptr, data, len);
    } else {
        input.ptr = NULL;
    }
    input.len = len;
    tegmentum_crypto_hash_multiplexer_hash_dispatcher_digest(a, &input, out);
    return 0;
}

/* digest(name, data) — one-shot ------------------------------------------ */

static PyObject *mod_digest(PyObject *self, PyObject *args)
{
    const char *name;
    PyObject *data;
    if (!PyArg_ParseTuple(args, "sO:digest", &name, &data)) return NULL;
    algo_t a;
    if (name_to_algo(name, &a) < 0) return NULL;

    crypto_hash_import_list_u8_t input;
    if (bytes_to_list(data, &input) < 0) return NULL;
    crypto_hash_import_list_u8_t output;
    tegmentum_crypto_hash_multiplexer_hash_dispatcher_digest(a, &input, &output);
    return list_to_bytes(&output);
}

/* Hasher streaming type -------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    algo_t      algo;
    PyObject   *buffer;   /* PyByteArrayObject — accumulated input */
    Py_ssize_t  digest_size;  /* lazy-discovered */
} HasherObject;

static PyTypeObject HasherType;

static PyObject *Hasher_new_with_state(algo_t a, PyObject *seed_buffer)
{
    HasherObject *self = PyObject_New(HasherObject, &HasherType);
    if (!self) return NULL;
    self->algo = a;
    self->digest_size = -1;
    if (seed_buffer) {
        Py_INCREF(seed_buffer);
        self->buffer = seed_buffer;
    } else {
        self->buffer = PyByteArray_FromStringAndSize(NULL, 0);
        if (!self->buffer) { PyObject_Free(self); return NULL; }
    }
    return (PyObject *) self;
}

static void Hasher_dealloc(HasherObject *self)
{
    Py_XDECREF(self->buffer);
    PyObject_Free(self);
}

static PyObject *Hasher_update(HasherObject *self, PyObject *args)
{
    PyObject *data;
    if (!PyArg_ParseTuple(args, "O:update", &data)) return NULL;

    Py_buffer view;
    if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE | PyBUF_C_CONTIGUOUS) < 0) {
        return NULL;
    }
    /* Append to the accumulated input buffer. */
    PyObject *appended = PyByteArray_FromStringAndSize((const char *) view.buf,
                                                       (Py_ssize_t) view.len);
    PyBuffer_Release(&view);
    if (!appended) return NULL;
    PyObject *cat = PyByteArray_Concat(self->buffer, appended);
    Py_DECREF(appended);
    if (!cat) return NULL;
    Py_DECREF(self->buffer);
    self->buffer = cat;
    Py_RETURN_NONE;
}

/* Materialize: one-shot digest of the entire accumulated input. Idempotent. */
static PyObject *Hasher_materialize(HasherObject *self)
{
    crypto_hash_import_list_u8_t out;
    if (digest_capability(self->algo,
                          (const uint8_t *) PyByteArray_AS_STRING(self->buffer),
                          (size_t) PyByteArray_GET_SIZE(self->buffer),
                          &out) < 0) {
        return NULL;
    }
    self->digest_size = (Py_ssize_t) out.len;
    return list_to_bytes(&out);
}

static PyObject *Hasher_digest(HasherObject *self, PyObject *Py_UNUSED(args))
{
    return Hasher_materialize(self);
}

static PyObject *Hasher_hexdigest(HasherObject *self, PyObject *Py_UNUSED(args))
{
    PyObject *b = Hasher_materialize(self);
    if (!b) return NULL;
    PyObject *hex = PyObject_CallMethod(b, "hex", NULL);
    Py_DECREF(b);
    return hex;
}

/* copy() returns an independent hasher with the same algorithm + same
 * accumulated input. Matches hashlib semantics. */
static PyObject *Hasher_copy(HasherObject *self, PyObject *Py_UNUSED(args))
{
    PyObject *buf_copy = PyByteArray_FromStringAndSize(
        PyByteArray_AS_STRING(self->buffer),
        PyByteArray_GET_SIZE(self->buffer));
    if (!buf_copy) return NULL;
    PyObject *clone = Hasher_new_with_state(self->algo, buf_copy);
    Py_DECREF(buf_copy);
    return clone;
}

static PyObject *Hasher_get_name(HasherObject *self, void *Py_UNUSED(closure))
{
    return PyUnicode_FromString(algo_to_name(self->algo));
}

static PyObject *Hasher_get_digest_size(HasherObject *self, void *Py_UNUSED(closure))
{
    if (self->digest_size < 0) {
        /* Discover via empty digest, then forget the result. */
        crypto_hash_import_list_u8_t out;
        if (digest_capability(self->algo, NULL, 0, &out) < 0) return NULL;
        self->digest_size = (Py_ssize_t) out.len;
        crypto_hash_import_list_u8_free(&out);
    }
    return PyLong_FromSsize_t(self->digest_size);
}

static PyMethodDef Hasher_methods[] = {
    {"update",    (PyCFunction) Hasher_update,    METH_VARARGS, "Add data to the hash."},
    {"digest",    (PyCFunction) Hasher_digest,    METH_NOARGS,  "Return the digest bytes (idempotent)."},
    {"hexdigest", (PyCFunction) Hasher_hexdigest, METH_NOARGS,  "Return the digest as a hex string."},
    {"copy",      (PyCFunction) Hasher_copy,      METH_NOARGS,  "Return an independent copy (same accumulated state)."},
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef Hasher_getset[] = {
    {"name",        (getter) Hasher_get_name,        NULL, "Algorithm name.", NULL},
    {"digest_size", (getter) Hasher_get_digest_size, NULL, "Digest length in bytes.", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyTypeObject HasherType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "_crypto_hash.hasher",
    .tp_basicsize = sizeof(HasherObject),
    .tp_dealloc   = (destructor) Hasher_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "Streaming hasher backed by the crypto-hash-multiplexer capability.",
    .tp_methods   = Hasher_methods,
    .tp_getset    = Hasher_getset,
};

/* new(name) -> hasher ---------------------------------------------------- */

static PyObject *mod_new(PyObject *self, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s:new", &name)) return NULL;
    algo_t a;
    if (name_to_algo(name, &a) < 0) return NULL;
    return Hasher_new_with_state(a, NULL);
}

/* algorithms() ----------------------------------------------------------- */

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

/* Module plumbing -------------------------------------------------------- */

static PyMethodDef module_methods[] = {
    {"digest",     mod_digest,     METH_VARARGS,
     "digest(name, data) -> bytes\n\nOne-shot digest via the crypto-hash-multiplexer."},
    {"new",        mod_new,        METH_VARARGS,
     "new(name) -> hasher\n\nReturn a streaming hasher for algorithm `name`."},
    {"algorithms", mod_algorithms, METH_NOARGS,
     "algorithms() -> tuple[str, ...]\n\nNames of supported algorithms."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef crypto_hash_module = {
    PyModuleDef_HEAD_INIT,
    "_crypto_hash",
    "CPython binding for tegmentum:crypto-hash-multiplexer.\n"
    "Provides hashlib-style digests via the WIT capability — composed in at\n"
    "component-build time, no OpenSSL required.",
    -1,
    module_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__crypto_hash(void)
{
    if (PyType_Ready(&HasherType) < 0) return NULL;
    PyObject *m = PyModule_Create(&crypto_hash_module);
    if (!m) return NULL;
    Py_INCREF(&HasherType);
    if (PyModule_AddObject(m, "hasher", (PyObject *) &HasherType) < 0) {
        Py_DECREF(&HasherType);
        Py_DECREF(m);
        return NULL;
    }
    return m;
}

/* _kdf_cap — Python C extension routing scrypt + pbkdf2 + argon2id
 * through the tegmentum:password-hash-multiplexer/password-dispatcher
 * capability.
 *
 * Stdlib hashlib.scrypt and hashlib.pbkdf2_hmac normally come from
 * _hashlib (OpenSSL). The default python-wasm build doesn't ship
 * static OpenSSL, so those KDFs were unavailable. Phase 5 KDF cap
 * integration ships scrypt/argon2id via this extension (pbkdf2 stays
 * pure-Python in Lib/_hashlib.py because the cap's pbkdf2 is sha256-only
 * while stdlib's signature takes any hash name).
 *
 * Surface:
 *   _kdf_cap.derive(algorithm, password, salt, length) -> bytes
 *   _kdf_cap.algorithms() -> tuple[str, ...]
 *
 * The cap uses RECOMMENDED parameters per algorithm — it doesn't take
 * N/r/p for scrypt or iterations for pbkdf2. Sufficient for the common
 * "derive a key from a password" use case. Stdlib hashlib's scrypt
 * signature takes tuning params; Lib/_hashlib.py's scrypt wrapper
 * routes through this cap only when params are unspecified or match
 * recommended defaults.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "gen/kdf_cap_import.h"

#define ALGO_T      tegmentum_password_hash_multiplexer_password_dispatcher_password_algorithm_t
#define ALGO_ARGON2 TEGMENTUM_PASSWORD_HASH_MULTIPLEXER_PASSWORD_DISPATCHER_PASSWORD_ALGORITHM_ARGON2ID
#define ALGO_SCRYPT TEGMENTUM_PASSWORD_HASH_MULTIPLEXER_PASSWORD_DISPATCHER_PASSWORD_ALGORITHM_SCRYPT
#define ALGO_PBKDF2 TEGMENTUM_PASSWORD_HASH_MULTIPLEXER_PASSWORD_DISPATCHER_PASSWORD_ALGORITHM_PBKDF2

typedef struct {
    const char *name;
    ALGO_T      algo;
} algo_name_t;

static const algo_name_t ALGO_TABLE[] = {
    {"argon2id", ALGO_ARGON2},
    {"scrypt",   ALGO_SCRYPT},
    {"pbkdf2",   ALGO_PBKDF2},
};

static int name_to_algo(const char *name, ALGO_T *out)
{
    for (size_t i = 0; i < sizeof(ALGO_TABLE) / sizeof(ALGO_TABLE[0]); i++) {
        if (strcmp(name, ALGO_TABLE[i].name) == 0) {
            *out = ALGO_TABLE[i].algo;
            return 0;
        }
    }
    PyErr_Format(PyExc_ValueError, "unsupported KDF algorithm: %s", name);
    return -1;
}


/* derive(algo, password, salt, length) -> bytes */
static PyObject *
mod_derive(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"algorithm", "password", "salt", "length", NULL};
    const char *algo_name;
    Py_buffer password_buf;
    Py_buffer salt_buf;
    unsigned int length;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sy*y*I:derive", kwlist,
                                      &algo_name, &password_buf, &salt_buf,
                                      &length)) {
        return NULL;
    }

    ALGO_T algo;
    if (name_to_algo(algo_name, &algo) < 0) {
        PyBuffer_Release(&password_buf);
        PyBuffer_Release(&salt_buf);
        return NULL;
    }

    /* The cap takes password as `string`; we have bytes from the caller.
     * For scrypt/pbkdf2/argon2 the password is treated as opaque bytes
     * — pass through with WIT-string framing. */
    kdf_cap_import_string_t password = {
        .ptr = (uint8_t *) password_buf.buf,
        .len = (size_t) password_buf.len,
    };
    kdf_cap_import_list_u8_t salt = {
        .ptr = (uint8_t *) salt_buf.buf,
        .len = (size_t) salt_buf.len,
    };
    kdf_cap_import_list_u8_t out;
    kdf_cap_import_string_t err;

    bool ok = tegmentum_password_hash_multiplexer_password_dispatcher_derive(
        algo, &password, &salt, (uint32_t) length, &out, &err);

    PyBuffer_Release(&password_buf);
    PyBuffer_Release(&salt_buf);

    if (!ok) {
        PyErr_Format(PyExc_RuntimeError, "_kdf_cap.derive failed: %.*s",
                     (int) err.len, (const char *) err.ptr);
        kdf_cap_import_string_free(&err);
        return NULL;
    }

    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr,
                                              (Py_ssize_t) out.len);
    kdf_cap_import_list_u8_free(&out);
    return r;
}


/* algorithms() -> tuple[str, ...] */
static PyObject *
mod_algorithms(PyObject *self, PyObject *Py_UNUSED(args))
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
    {"derive",     (PyCFunction) mod_derive, METH_VARARGS | METH_KEYWORDS,
     "derive(algorithm, password, salt, length) -> bytes\n\n"
     "Derive `length` bytes from `password` and `salt` using the named\n"
     "algorithm at its recommended cost. Routes through the\n"
     "password-hash-multiplexer capability."},
    {"algorithms", mod_algorithms, METH_NOARGS,
     "algorithms() -> tuple[str, ...]\n\nSupported algorithm names."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "_kdf_cap",
    "KDF dispatcher (argon2id, scrypt, pbkdf2) over the "
    "tegmentum:password-hash-multiplexer capability.",
    -1,
    module_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__kdf_cap(void)
{
    return PyModule_Create(&module_def);
}

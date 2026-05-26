/* _ssl_capability — Python C extension routing to the openssl-component
 * TLS capability over the Component Model.
 *
 * Statically linked into wasi-sdk CPython (Modules/Setup.local). The imported
 * WIT functions appear as wasm imports on python.wasm; satisfied at compose
 * time by wac/composectl plugging openssl-component.wasm.
 *
 * Current Python surface:
 *   - MemoryBIO()                       — Phase 3b.2 (this commit)
 *       .write(b: bytes) -> int          (bytes written)
 *       .read([size]) -> bytes           (size==-1 -> drain)
 *       .write_eof() -> None             (one-shot)
 *       .pending -> int                  (bytes buffered)
 *       .eof -> bool                     (write_eof() called AND drained)
 *
 *   - probe_imports() -> None           — keep openssl-component import alive
 *                                          until 3b.3 wires the real consumer
 *
 * Phase 3b.3+ adds _SSLContext / _SSLSocket / RAND_bytes / OPENSSL_VERSION /
 * error classes / ssl.py re-routing. Naming is `_ssl_capability` (not `_ssl`)
 * so the existing static OpenSSL `_ssl` module keeps working unchanged during
 * the bring-up; Phase 5 retires the static path.
 *
 * Design note (MemoryBIO):
 *   CPython's MemoryBIO wraps OpenSSL BIO_s_mem(); we don't have direct BIO
 *   access (the openssl-component owns its OpenSSL state), so we implement
 *   the BIO as a pure in-memory growable byte buffer with CPython's exact
 *   public semantics. Phase 3b.3's _SSLContext.wrap_bio() reads/writes these
 *   to pump bytes between user code and openssl-component's tls.client (which
 *   manages its own internal BIOs). Semantically equivalent to the static
 *   _ssl.MemoryBIO; the bytes that flow through it are identical.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "gen/ssl_import.h"

/* -------------------------------------------------------------------------
 * MemoryBIO type — Phase 3b.2
 *
 * A FIFO byte buffer with a "head" cursor: read() advances the head;
 * write() appends at the tail; pending = tail - head. Pure in-memory, no
 * openssl-component dependency.
 * ------------------------------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    char       *buf;        /* heap allocation */
    Py_ssize_t  cap;        /* allocated capacity */
    Py_ssize_t  head;       /* read cursor */
    Py_ssize_t  tail;       /* write cursor (== bytes available + head) */
    int         eof_written;
} MemoryBIOObject;

static PyTypeObject MemoryBIO_Type;

#define MBIO_INITIAL_CAP 256

static int memorybio_grow(MemoryBIOObject *self, Py_ssize_t need)
{
    Py_ssize_t want = self->tail + need;
    if (want <= self->cap) {
        return 0;
    }
    /* Compact: shift unread bytes to the start before growing, so a
     * read/write pattern with many small chunks doesn't grow unboundedly. */
    if (self->head > 0) {
        Py_ssize_t avail = self->tail - self->head;
        if (avail > 0) {
            memmove(self->buf, self->buf + self->head, (size_t) avail);
        }
        self->tail -= self->head;
        self->head = 0;
        if (self->tail + need <= self->cap) {
            return 0;
        }
    }
    Py_ssize_t new_cap = self->cap == 0 ? MBIO_INITIAL_CAP : self->cap;
    while (new_cap < self->tail + need) {
        if (new_cap > PY_SSIZE_T_MAX / 2) {
            PyErr_NoMemory();
            return -1;
        }
        new_cap *= 2;
    }
    char *nb = (char *) PyMem_Realloc(self->buf, (size_t) new_cap);
    if (!nb) {
        PyErr_NoMemory();
        return -1;
    }
    self->buf = nb;
    self->cap = new_cap;
    return 0;
}

static PyObject *MemoryBIO_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    if (args && PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError, "MemoryBIO() takes no arguments");
        return NULL;
    }
    if (kwds && PyDict_GET_SIZE(kwds) != 0) {
        PyErr_SetString(PyExc_TypeError, "MemoryBIO() takes no keyword arguments");
        return NULL;
    }
    MemoryBIOObject *self = (MemoryBIOObject *) type->tp_alloc(type, 0);
    if (!self) return NULL;
    self->buf = NULL;
    self->cap = 0;
    self->head = 0;
    self->tail = 0;
    self->eof_written = 0;
    return (PyObject *) self;
}

static void MemoryBIO_dealloc(MemoryBIOObject *self)
{
    PyMem_Free(self->buf);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyObject *MemoryBIO_get_pending(MemoryBIOObject *self, void *Py_UNUSED(closure))
{
    return PyLong_FromSsize_t(self->tail - self->head);
}

static PyObject *MemoryBIO_get_eof(MemoryBIOObject *self, void *Py_UNUSED(closure))
{
    int empty = (self->tail == self->head);
    return PyBool_FromLong(empty && self->eof_written);
}

static PyObject *MemoryBIO_read(MemoryBIOObject *self, PyObject *args)
{
    Py_ssize_t want = -1;
    if (!PyArg_ParseTuple(args, "|n:read", &want)) return NULL;

    Py_ssize_t avail = self->tail - self->head;
    Py_ssize_t take = (want < 0 || want > avail) ? avail : want;

    PyObject *out = PyBytes_FromStringAndSize(self->buf + self->head, take);
    if (!out) return NULL;
    self->head += take;
    /* Reset cursors when fully drained — avoids growing unboundedly on a
     * pump-style usage pattern (write N, read N, write N, ...). */
    if (self->head == self->tail) {
        self->head = self->tail = 0;
    }
    return out;
}

static PyObject *MemoryBIO_write(MemoryBIOObject *self, PyObject *args)
{
    Py_buffer view;
    if (!PyArg_ParseTuple(args, "y*:write", &view)) return NULL;

    if (self->eof_written) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_OSError, "cannot write() after write_eof()");
        return NULL;
    }
    if (view.len > 0) {
        if (memorybio_grow(self, view.len) < 0) {
            PyBuffer_Release(&view);
            return NULL;
        }
        memcpy(self->buf + self->tail, view.buf, (size_t) view.len);
        self->tail += view.len;
    }
    Py_ssize_t n = view.len;
    PyBuffer_Release(&view);
    return PyLong_FromSsize_t(n);
}

static PyObject *MemoryBIO_write_eof(MemoryBIOObject *self, PyObject *Py_UNUSED(args))
{
    self->eof_written = 1;
    Py_RETURN_NONE;
}

static PyMethodDef MemoryBIO_methods[] = {
    {"read",      (PyCFunction) MemoryBIO_read,      METH_VARARGS,
     "read([size=-1]) -> bytes\n\nRead up to `size` bytes (default: drain).\n"
     "Empty bytes means either EOF (see .eof) or no data available."},
    {"write",     (PyCFunction) MemoryBIO_write,     METH_VARARGS,
     "write(b: bytes) -> int\n\nAppend bytes; returns the count written.\n"
     "Raises OSError if write_eof() has already been called."},
    {"write_eof", (PyCFunction) MemoryBIO_write_eof, METH_NOARGS,
     "write_eof() -> None\n\nMark the buffer as no-more-writes. The .eof\n"
     "property becomes True once all written bytes have been read."},
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef MemoryBIO_getset[] = {
    {"pending", (getter) MemoryBIO_get_pending, NULL,
     "Number of bytes available to read.", NULL},
    {"eof",     (getter) MemoryBIO_get_eof,     NULL,
     "True when write_eof() has been called AND all data has been read.", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyTypeObject MemoryBIO_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "_ssl_capability.MemoryBIO",
    .tp_basicsize = sizeof(MemoryBIOObject),
    .tp_dealloc   = (destructor) MemoryBIO_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "In-memory BIO buffer (FIFO byte queue) matching\n"
                    "ssl.MemoryBIO semantics. Used by _SSLContext.wrap_bio()\n"
                    "to pump bytes between user code and the TLS capability.",
    .tp_methods   = MemoryBIO_methods,
    .tp_getset    = MemoryBIO_getset,
    .tp_new       = MemoryBIO_new,
};

/* -------------------------------------------------------------------------
 * Module plumbing
 * ------------------------------------------------------------------------- */

/* probe_imports — touches the openssl:component/x509 import so the linker
 * keeps the import alive until Phase 3b.3 introduces real openssl-component
 * calls (SSL context construction, etc.). Will be removed at 3b.3. */
static PyObject *probe_imports(PyObject *self, PyObject *Py_UNUSED(args))
{
    openssl_component_x509_own_store_t store =
        openssl_component_x509_constructor_store();
    openssl_component_x509_store_drop_own(store);
    Py_RETURN_NONE;
}

static PyMethodDef module_methods[] = {
    {"probe_imports", probe_imports, METH_NOARGS,
     "Scaffold: touch openssl-component/x509 to keep the import alive.\n"
     "Removed in Phase 3b.3 when real consumers (_SSLContext) are added."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef ssl_capability_module = {
    PyModuleDef_HEAD_INIT,
    "_ssl_capability",
    "CPython binding for openssl:component/tls.\n"
    "Composed in at component-build time via 'wac plug openssl-component.wasm'.\n"
    "Phase 3b.2 surface: MemoryBIO (in-memory BIO buffer). Full _SSLContext /\n"
    "_SSLSocket land in Phase 3b.3+. See docs/phase-3-tls.md.",
    -1,
    module_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__ssl_capability(void)
{
    if (PyType_Ready(&MemoryBIO_Type) < 0) return NULL;
    PyObject *m = PyModule_Create(&ssl_capability_module);
    if (!m) return NULL;
    Py_INCREF(&MemoryBIO_Type);
    if (PyModule_AddObject(m, "MemoryBIO", (PyObject *) &MemoryBIO_Type) < 0) {
        Py_DECREF(&MemoryBIO_Type);
        Py_DECREF(m);
        return NULL;
    }
    return m;
}

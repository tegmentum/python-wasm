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
#include "ca-bundle/ca_bundle.h"

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
 * SSLContext type — Phase 3b.3
 *
 * Holds the per-connection config that ssl.SSLContext exposes (verify mode,
 * CA roots, ALPN, etc.). On wrap_socket() we materialize an openssl-component
 * client by calling tls.client.connect(host, port, config).
 *
 * Limitations vs CPython's _SSLContext (documented in phase-3-tls.md
 * §"gap inventory"):
 *   - check_hostname is bundled with verify (we can't disable hostname
 *     check independently of cert validation)
 *   - load_cert_chain takes paths; we currently only accept inline PEM via
 *     set_client_cert (a future helper)
 *   - load_verify_locations same: inline PEM via set_ca_certs
 *   - No session resumption (v1.1)
 * ------------------------------------------------------------------------- */

/* SSLError exception — populated at module-init time. */
static PyObject *SSLError = NULL;

typedef struct {
    PyObject_HEAD
    /* Config knobs that map onto openssl_component_tls_client_config_t fields. */
    int       verify_mode;    /* 0=none, 1=optional, 2=required (CPython enum) */
    int       min_protocol;   /* tls12=0, tls13=1; -1 = default */
    int       max_protocol;
    /* PEM blobs the caller supplied via load_*; refcounted; NULL = unused. */
    PyObject *ca_pem_bytes;       /* bytes — concatenated CA roots */
    PyObject *client_cert_bytes;  /* bytes — client cert PEM */
    PyObject *client_key_bytes;   /* bytes — client key PEM */
    /* ALPN list as Python list-of-bytes (each item is ASCII proto name). */
    PyObject *alpn_protocols;     /* list[bytes] */
    /* Lazy-built trust store from ca_pem_bytes; owned by this context.
     * has_trust_store == 0 means none built yet (or no ca_pem_bytes set). */
    openssl_component_x509_own_store_t trust_store;
    int       has_trust_store;
} SSLContextObject;

static PyTypeObject SSLContext_Type;

static int SSLContext_init(SSLContextObject *self, PyObject *args, PyObject *kwds)
{
    /* Optional `protocol` arg — accepts the ssl.PROTOCOL_* int; we map TLS_CLIENT
     * to "max version = TLS 1.3, min = TLS 1.2", which is the standard default. */
    int protocol = -1;
    static char *kwlist[] = {"protocol", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|i:_SSLContext", kwlist, &protocol)) {
        return -1;
    }
    self->verify_mode = 2;        /* CERT_REQUIRED default */
    self->min_protocol = 0;       /* TLSv1.2 */
    self->max_protocol = 1;       /* TLSv1.3 */
    self->ca_pem_bytes = NULL;
    self->client_cert_bytes = NULL;
    self->client_key_bytes = NULL;
    self->alpn_protocols = NULL;
    self->has_trust_store = 0;
    return 0;
}

static void SSLContext_drop_trust(SSLContextObject *self)
{
    if (self->has_trust_store) {
        openssl_component_x509_store_drop_own(self->trust_store);
        self->has_trust_store = 0;
    }
}

static void SSLContext_dealloc(SSLContextObject *self)
{
    SSLContext_drop_trust(self);
    Py_XDECREF(self->ca_pem_bytes);
    Py_XDECREF(self->client_cert_bytes);
    Py_XDECREF(self->client_key_bytes);
    Py_XDECREF(self->alpn_protocols);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

/* Build (or rebuild) the trust store from self->ca_pem_bytes.
 * Returns 0 on success, -1 on error (with PyErr set). */
static int SSLContext_build_trust_store(SSLContextObject *self)
{
    SSLContext_drop_trust(self);
    if (!self->ca_pem_bytes) {
        return 0;  /* no trust store wanted; wrap_socket leaves trust=None */
    }
    Py_ssize_t pem_len = PyBytes_GET_SIZE(self->ca_pem_bytes);
    if (pem_len == 0) return 0;

    /* Parse the PEM bundle into a list of certificates (owned by us). */
    ssl_import_list_u8_t pem_in;
    pem_in.ptr = (uint8_t *) malloc((size_t) pem_len);
    if (!pem_in.ptr) { PyErr_NoMemory(); return -1; }
    memcpy(pem_in.ptr, PyBytes_AS_STRING(self->ca_pem_bytes), (size_t) pem_len);
    pem_in.len = (size_t) pem_len;

    openssl_component_x509_list_own_certificate_t certs;
    openssl_component_x509_x509_error_t parse_err;
    bool ok = openssl_component_x509_static_certificate_parse_chain(
        &pem_in, &certs, &parse_err);
    if (!ok) {
        openssl_component_x509_x509_error_free(&parse_err);
        PyErr_SetString(SSLError, "failed to parse CA bundle PEM");
        return -1;
    }

    openssl_component_x509_own_store_t store =
        openssl_component_x509_constructor_store();

    /* Add each parsed cert to the store as a trust anchor. add_trusted takes
     * a borrowed store + borrowed cert; both are converted from `own` handles. */
    openssl_component_x509_borrow_store_t bstore =
        openssl_component_x509_borrow_store(store);
    for (size_t i = 0; i < certs.len; i++) {
        openssl_component_x509_borrow_certificate_t bcert =
            openssl_component_x509_borrow_certificate(certs.ptr[i]);
        openssl_component_x509_x509_error_t add_err;
        bool added = openssl_component_x509_method_store_add_trusted(
            bstore, bcert, &add_err);
        if (!added) {
            /* Skip this cert; don't fail the whole bundle for one bad anchor. */
            openssl_component_x509_x509_error_free(&add_err);
        }
    }
    /* Drop the owned certs — the store now holds its own references. */
    for (size_t i = 0; i < certs.len; i++) {
        openssl_component_x509_certificate_drop_own(certs.ptr[i]);
    }
    if (certs.ptr) free(certs.ptr);

    self->trust_store = store;
    self->has_trust_store = 1;
    return 0;
}

/* set_ca_certs(cadata: bytes) — inline alternative to load_verify_locations.
 * Invalidates any previously-built trust store; it'll be rebuilt lazily on
 * the next wrap_socket(). */
static PyObject *SSLContext_set_ca_certs(SSLContextObject *self, PyObject *args)
{
    PyObject *pem;
    if (!PyArg_ParseTuple(args, "S:set_ca_certs", &pem)) return NULL;
    Py_INCREF(pem);
    Py_XSETREF(self->ca_pem_bytes, pem);
    SSLContext_drop_trust(self);
    Py_RETURN_NONE;
}

/* load_default_certs() — installs the embedded Mozilla CA bundle as the
 * trust store. After this, CERT_REQUIRED can validate normal HTTPS servers.
 * The PEM is committed to the repo at cpython-ext/_ssl/ca-bundle/cacert.pem
 * (SHA256-pinned via SSL_CAPABILITY_CA_BUNDLE_SHA256). */
static PyObject *SSLContext_load_default_certs(SSLContextObject *self, PyObject *Py_UNUSED(args))
{
    PyObject *pem = PyBytes_FromStringAndSize(
        (const char *) SSL_CAPABILITY_CA_BUNDLE_PEM,
        (Py_ssize_t) SSL_CAPABILITY_CA_BUNDLE_PEM_LEN);
    if (!pem) return NULL;
    Py_XSETREF(self->ca_pem_bytes, pem);
    SSLContext_drop_trust(self);
    Py_RETURN_NONE;
}

/* set_client_cert(cert_pem: bytes, key_pem: bytes) — inline mTLS material. */
static PyObject *SSLContext_set_client_cert(SSLContextObject *self, PyObject *args)
{
    PyObject *cert, *key;
    if (!PyArg_ParseTuple(args, "SS:set_client_cert", &cert, &key)) return NULL;
    Py_INCREF(cert);
    Py_INCREF(key);
    Py_XSETREF(self->client_cert_bytes, cert);
    Py_XSETREF(self->client_key_bytes, key);
    Py_RETURN_NONE;
}

/* set_alpn_protocols(protos: list[str|bytes]) */
static PyObject *SSLContext_set_alpn_protocols(SSLContextObject *self, PyObject *args)
{
    PyObject *list;
    if (!PyArg_ParseTuple(args, "O:set_alpn_protocols", &list)) return NULL;
    if (!PyList_Check(list) && !PyTuple_Check(list)) {
        PyErr_SetString(PyExc_TypeError, "alpn_protocols must be a list or tuple");
        return NULL;
    }
    Py_ssize_t n = PySequence_Size(list);
    PyObject *out = PyList_New(n);
    if (!out) return NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PySequence_GetItem(list, i);
        if (!item) { Py_DECREF(out); return NULL; }
        PyObject *as_bytes = NULL;
        if (PyBytes_Check(item)) {
            as_bytes = item; Py_INCREF(as_bytes);
        } else if (PyUnicode_Check(item)) {
            as_bytes = PyUnicode_AsASCIIString(item);
        } else {
            PyErr_SetString(PyExc_TypeError, "alpn proto must be str or bytes");
            Py_DECREF(item); Py_DECREF(out); return NULL;
        }
        Py_DECREF(item);
        if (!as_bytes) { Py_DECREF(out); return NULL; }
        PyList_SET_ITEM(out, i, as_bytes);
    }
    Py_XSETREF(self->alpn_protocols, out);
    Py_RETURN_NONE;
}

/* verify_mode getter/setter */
static PyObject *SSLContext_get_verify_mode(SSLContextObject *self, void *Py_UNUSED(c))
{
    return PyLong_FromLong(self->verify_mode);
}
static int SSLContext_set_verify_mode(SSLContextObject *self, PyObject *value, void *Py_UNUSED(c))
{
    long v = PyLong_AsLong(value);
    if (v == -1 && PyErr_Occurred()) return -1;
    if (v < 0 || v > 2) {
        PyErr_SetString(PyExc_ValueError, "verify_mode must be 0, 1, or 2");
        return -1;
    }
    self->verify_mode = (int) v;
    return 0;
}

/* Forward to SSLSocket constructor — defined below. */
static PyObject *SSLSocket_new_for_context(SSLContextObject *ctx,
                                           const char *host, uint16_t port,
                                           const char *server_hostname);

/* wrap_socket(host: str, port: int, server_hostname: str | None) -> _SSLSocket
 *
 * Departure from CPython: CPython's wrap_socket takes a `socket.socket`
 * argument and consumes its fd; openssl-component owns its TCP internally so
 * we take host/port directly. ssl.py-side compatibility shim translates the
 * old signature into ours. */
static PyObject *SSLContext_wrap_socket(SSLContextObject *self, PyObject *args, PyObject *kwds)
{
    const char *host;
    int port;
    const char *server_hostname = NULL;
    static char *kwlist[] = {"host", "port", "server_hostname", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "si|z:wrap_socket", kwlist,
                                     &host, &port, &server_hostname)) {
        return NULL;
    }
    if (port < 0 || port > 65535) {
        PyErr_SetString(PyExc_ValueError, "port out of range");
        return NULL;
    }
    return SSLSocket_new_for_context(self, host, (uint16_t) port,
                                     server_hostname ? server_hostname : host);
}

static PyMethodDef SSLContext_methods[] = {
    {"set_ca_certs",        (PyCFunction) SSLContext_set_ca_certs,        METH_VARARGS,
     "set_ca_certs(pem: bytes)\n\nInline trust roots (alternative to load_verify_locations)."},
    {"load_default_certs",  (PyCFunction) SSLContext_load_default_certs,  METH_NOARGS,
     "load_default_certs() -> None\n\nLoad the bundled Mozilla WebPKI roots\n"
     "(see _ssl_capability.CA_BUNDLE_CERT_COUNT for the cert count).\n"
     "After this, CERT_REQUIRED can validate normal HTTPS servers."},
    {"set_client_cert",     (PyCFunction) SSLContext_set_client_cert,     METH_VARARGS,
     "set_client_cert(cert_pem: bytes, key_pem: bytes)\n\nClient certificate + key for mTLS."},
    {"set_alpn_protocols",  (PyCFunction) SSLContext_set_alpn_protocols,  METH_VARARGS,
     "set_alpn_protocols(protos: list[str|bytes])\n\nALPN preference list."},
    {"wrap_socket",         (PyCFunction) SSLContext_wrap_socket,         METH_VARARGS | METH_KEYWORDS,
     "wrap_socket(host: str, port: int, server_hostname: str=None) -> _SSLSocket"},
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef SSLContext_getset[] = {
    {"verify_mode",
     (getter) SSLContext_get_verify_mode,
     (setter) SSLContext_set_verify_mode,
     "0=none, 1=optional, 2=required (mirrors ssl.CERT_*).", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyTypeObject SSLContext_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "_ssl_capability._SSLContext",
    .tp_basicsize = sizeof(SSLContextObject),
    .tp_dealloc   = (destructor) SSLContext_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_doc       = "TLS context: configuration for client connections.\n"
                    "Materializes an openssl:component/tls.client on wrap_socket().",
    .tp_methods   = SSLContext_methods,
    .tp_getset    = SSLContext_getset,
    .tp_init      = (initproc) SSLContext_init,
    .tp_new       = PyType_GenericNew,
};

/* -------------------------------------------------------------------------
 * SSLSocket type — Phase 3b.3
 *
 * Wraps an openssl:component/tls.client resource. Construction implies
 * connect+handshake (the openssl-component connect() is synchronous and
 * handles its own TCP). read/write proxy directly to tls.client.{read,write}.
 * Connection lifecycle: a non-None handle == still open; None == closed/
 * deallocated.
 * ------------------------------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    openssl_component_tls_own_client_t handle;
    int                                 has_handle;    /* 0 after unwrap/close */
    PyObject                           *server_hostname; /* str, kept for getter */
    PyObject                           *cached_peer_info;
    int                                 cached_peer_done;
} SSLSocketObject;

static PyTypeObject SSLSocket_Type;

/* Raise an SSLError with a formatted message including the openssl error code,
 * then free the WIT error blob. Returns NULL for convenience in `return raise...`. */
static PyObject *raise_tls_error(const char *prefix,
                                 openssl_component_tls_tls_error_t *err)
{
    /* The WIT tls-error variant is opaque to us at the binding layer in v1;
     * future work can introspect the tag. For now, free it and surface a
     * human-readable prefix. */
    openssl_component_tls_tls_error_free(err);
    PyErr_Format(SSLError, "%s", prefix);
    return NULL;
}

/* Build a list_string for ALPN from a Python list of bytes objects. */
static int alpn_list_from_pylist(PyObject *list, ssl_import_list_string_t *out)
{
    if (!list || list == Py_None || PyList_Size(list) == 0) {
        out->ptr = NULL; out->len = 0;
        return 0;
    }
    Py_ssize_t n = PyList_Size(list);
    out->ptr = (ssl_import_string_t *) malloc(sizeof(ssl_import_string_t) * n);
    if (!out->ptr) { PyErr_NoMemory(); return -1; }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PyList_GET_ITEM(list, i);
        Py_ssize_t blen = PyBytes_GET_SIZE(item);
        uint8_t *dup = (uint8_t *) malloc(blen);
        if (!dup) {
            for (Py_ssize_t j = 0; j < i; j++) free(out->ptr[j].ptr);
            free(out->ptr); PyErr_NoMemory(); return -1;
        }
        memcpy(dup, PyBytes_AS_STRING(item), (size_t) blen);
        out->ptr[i].ptr = dup;
        out->ptr[i].len = (size_t) blen;
    }
    out->len = (size_t) n;
    return 0;
}

/* Build a list_u8 by COPYING the given bytes. Caller (or canonical-ABI) frees. */
static int list_u8_dup(const uint8_t *src, size_t len, ssl_import_list_u8_t *out)
{
    if (len == 0) { out->ptr = NULL; out->len = 0; return 0; }
    out->ptr = (uint8_t *) malloc(len);
    if (!out->ptr) { PyErr_NoMemory(); return -1; }
    memcpy(out->ptr, src, len);
    out->len = len;
    return 0;
}

static PyObject *SSLSocket_new_for_context(SSLContextObject *ctx,
                                           const char *host, uint16_t port,
                                           const char *server_hostname)
{
    SSLSocketObject *self = PyObject_New(SSLSocketObject, &SSLSocket_Type);
    if (!self) return NULL;
    self->has_handle = 0;
    self->server_hostname = PyUnicode_FromString(server_hostname ? server_hostname : host);
    self->cached_peer_info = NULL;
    self->cached_peer_done = 0;
    if (!self->server_hostname) { Py_DECREF(self); return NULL; }

    /* Build the openssl-component config. Optionals: trust=None (use system
     * default until set_ca_certs supplies a PEM blob — TODO Phase 3b.4 will
     * build a store from ca_pem_bytes); verify-options=None; client cert
     * mTLS only when both PEMs are present; ALPN from list_string. */
    /* Lazily build the trust store from ca_pem_bytes (set via set_ca_certs
     * or load_default_certs). Failure here propagates up as SSLError. */
    if (!ctx->has_trust_store && ctx->ca_pem_bytes != NULL) {
        if (SSLContext_build_trust_store(ctx) < 0) {
            Py_DECREF(self);
            return NULL;
        }
    }

    openssl_component_tls_client_config_t config = {0};
    config.protocols.min = (uint8_t) ctx->min_protocol;
    config.protocols.max = (uint8_t) ctx->max_protocol;
    config.verify = (uint8_t) ctx->verify_mode;
    config.trust.is_some = false;
    if (ctx->has_trust_store) {
        /* openssl-component takes ownership of the store handle we hand it
         * (a one-shot consume — matches how `own` resources flow in WIT).
         * Move it: clear our cached handle to avoid a double-free; the next
         * wrap_socket() will rebuild from ca_pem_bytes. */
        config.trust.is_some = true;
        config.trust.val = ctx->trust_store;
        ctx->has_trust_store = 0;
    }
    config.verify_options.is_some = false;
    config.server_name.is_some = true;
    {
        size_t n = strlen(server_hostname);
        config.server_name.val.ptr = (uint8_t *) malloc(n);
        if (!config.server_name.val.ptr) { Py_DECREF(self); PyErr_NoMemory(); return NULL; }
        memcpy(config.server_name.val.ptr, server_hostname, n);
        config.server_name.val.len = n;
    }
    config.client_cert.is_some = false;
    config.client_key.is_some = false;
    /* TODO 3b.4: ctx->client_cert_bytes / ->client_key_bytes -> x509 cert + pkey resources */

    config.alpn.is_some = (ctx->alpn_protocols != NULL && PyList_Size(ctx->alpn_protocols) > 0);
    if (config.alpn.is_some) {
        if (alpn_list_from_pylist(ctx->alpn_protocols, &config.alpn.val.protocols) < 0) {
            free(config.server_name.val.ptr);
            Py_DECREF(self);
            return NULL;
        }
    }
    config.ciphers.is_some = false;
    config.groups.is_some = false;
    config.enable_early_data = false;
    /* resume_session, keylog: all zero/none from {0} init */

    ssl_import_string_t host_str;
    if (list_u8_dup((const uint8_t *) host, strlen(host),
                    (ssl_import_list_u8_t *) &host_str) < 0) {
        /* clean up config blobs */
        if (config.server_name.is_some) free(config.server_name.val.ptr);
        Py_DECREF(self);
        return NULL;
    }

    openssl_component_tls_own_client_t client;
    openssl_component_tls_tls_error_t err;
    bool ok = openssl_component_tls_static_client_connect(
        &host_str, port, &config, &client, &err);

    if (!ok) {
        Py_DECREF(self);
        return raise_tls_error("TLS handshake failed", &err);
    }

    self->handle = client;
    self->has_handle = 1;
    return (PyObject *) self;
}

static void SSLSocket_dealloc(SSLSocketObject *self)
{
    if (self->has_handle) {
        /* openssl-component's tls.client has a static close(c) that takes
         * ownership; that's the standard tear-down. */
        openssl_component_tls_static_client_close(self->handle);
        self->has_handle = 0;
    }
    Py_XDECREF(self->server_hostname);
    Py_XDECREF(self->cached_peer_info);
    PyObject_Free(self);
}

static int ensure_open(SSLSocketObject *self)
{
    if (!self->has_handle) {
        PyErr_SetString(SSLError, "SSL connection is closed");
        return -1;
    }
    return 0;
}

static PyObject *SSLSocket_read(SSLSocketObject *self, PyObject *args)
{
    if (ensure_open(self) < 0) return NULL;
    int max = 8192;
    if (!PyArg_ParseTuple(args, "|i:read", &max)) return NULL;
    if (max <= 0) return PyBytes_FromStringAndSize(NULL, 0);

    ssl_import_list_u8_t out;
    openssl_component_tls_tls_error_t err;
    openssl_component_tls_borrow_client_t b =
        openssl_component_tls_borrow_client(self->handle);
    bool ok = openssl_component_tls_method_client_read(
        b, (uint32_t) max, &out, &err);
    if (!ok) return raise_tls_error("TLS read failed", &err);

    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr,
                                            (Py_ssize_t) out.len);
    ssl_import_list_u8_free(&out);
    return r;
}

static PyObject *SSLSocket_write(SSLSocketObject *self, PyObject *args)
{
    if (ensure_open(self) < 0) return NULL;
    Py_buffer view;
    if (!PyArg_ParseTuple(args, "y*:write", &view)) return NULL;

    ssl_import_list_u8_t input;
    if (list_u8_dup((const uint8_t *) view.buf, (size_t) view.len, &input) < 0) {
        PyBuffer_Release(&view);
        return NULL;
    }
    PyBuffer_Release(&view);

    uint32_t written;
    openssl_component_tls_tls_error_t err;
    openssl_component_tls_borrow_client_t b =
        openssl_component_tls_borrow_client(self->handle);
    bool ok = openssl_component_tls_method_client_write(b, &input, &written, &err);
    if (!ok) return raise_tls_error("TLS write failed", &err);
    return PyLong_FromUnsignedLong((unsigned long) written);
}

static PyObject *SSLSocket_pending(SSLSocketObject *self, PyObject *Py_UNUSED(args))
{
    /* openssl:component/tls doesn't expose SSL_pending; gap inventory item #1.
     * Return 0 — the only consequence is that SSLObject.pending() is uninformative;
     * read() still works (it just blocks at the TLS layer until a record arrives). */
    return PyLong_FromLong(0);
}

static PyObject *SSLSocket_shutdown(SSLSocketObject *self, PyObject *Py_UNUSED(args))
{
    if (!self->has_handle) Py_RETURN_NONE;  /* idempotent */
    openssl_component_tls_static_client_close(self->handle);
    self->has_handle = 0;
    Py_RETURN_NONE;
}

/* Get the peer_info, caching for repeat calls. */
static int load_peer_info(SSLSocketObject *self, openssl_component_tls_peer_info_t *out)
{
    if (ensure_open(self) < 0) return -1;
    openssl_component_tls_borrow_client_t b =
        openssl_component_tls_borrow_client(self->handle);
    openssl_component_tls_method_client_peer(b, out);
    return 0;
}

static PyObject *SSLSocket_version(SSLSocketObject *self, PyObject *Py_UNUSED(args))
{
    openssl_component_tls_peer_info_t info;
    if (load_peer_info(self, &info) < 0) return NULL;
    const char *name;
    switch (info.protocol) {
        case OPENSSL_COMPONENT_TLS_PROTOCOL_TLS12: name = "TLSv1.2"; break;
        case OPENSSL_COMPONENT_TLS_PROTOCOL_TLS13: name = "TLSv1.3"; break;
        default: name = "unknown"; break;
    }
    openssl_component_tls_peer_info_free(&info);
    return PyUnicode_FromString(name);
}

static PyObject *SSLSocket_cipher(SSLSocketObject *self, PyObject *Py_UNUSED(args))
{
    openssl_component_tls_peer_info_t info;
    if (load_peer_info(self, &info) < 0) return NULL;
    const char *version;
    switch (info.protocol) {
        case OPENSSL_COMPONENT_TLS_PROTOCOL_TLS12: version = "TLSv1.2"; break;
        case OPENSSL_COMPONENT_TLS_PROTOCOL_TLS13: version = "TLSv1.3"; break;
        default: version = "unknown"; break;
    }
    /* (name, version, secret_bits) — secret_bits is 0 per gap-inventory item #5. */
    PyObject *t = Py_BuildValue("(s#si)",
                                (const char *) info.cipher_suite.ptr,
                                (Py_ssize_t) info.cipher_suite.len,
                                version, 0);
    openssl_component_tls_peer_info_free(&info);
    return t;
}

/* peer_cert_der() — return the peer's certificate as DER bytes (or None
 * if the peer didn't send one). Powers ssl.get_server_certificate +
 * stdlib SSLSocket.getpeercert(binary_form=True). */
static PyObject *SSLSocket_peer_cert_der(SSLSocketObject *self, PyObject *Py_UNUSED(args))
{
    openssl_component_tls_peer_info_t info;
    if (load_peer_info(self, &info) < 0) return NULL;

    if (info.peer_chain.len == 0) {
        openssl_component_tls_peer_info_free(&info);
        Py_RETURN_NONE;
    }

    /* The peer's own certificate is the first in peer_chain. encode() it
     * to DER bytes — that's the binary form stdlib getpeercert returns. */
    openssl_component_x509_borrow_certificate_t cert =
        openssl_component_x509_borrow_certificate(info.peer_chain.ptr[0]);
    ssl_import_list_u8_t out;
    openssl_component_x509_x509_error_t err;
    bool is_err = openssl_component_x509_method_certificate_encode(
        cert, OPENSSL_COMPONENT_PKEY_ENCODING_DER, &out, &err);

    if (is_err) {
        PyErr_SetString(PyExc_RuntimeError,
                        "_ssl_capability.peer_cert_der: x509 encode failed");
        /* Don't bother decoding err here — the openssl_component x509_error
         * is a complex variant; simple message is fine for now. */
        openssl_component_tls_peer_info_free(&info);
        return NULL;
    }

    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr,
                                              (Py_ssize_t) out.len);
    ssl_import_list_u8_free(&out);
    openssl_component_tls_peer_info_free(&info);
    return r;
}


static PyObject *SSLSocket_selected_alpn_protocol(SSLSocketObject *self, PyObject *Py_UNUSED(args))
{
    openssl_component_tls_peer_info_t info;
    if (load_peer_info(self, &info) < 0) return NULL;
    PyObject *r;
    if (info.alpn.is_some) {
        r = PyUnicode_FromStringAndSize((const char *) info.alpn.val.ptr,
                                         (Py_ssize_t) info.alpn.val.len);
    } else {
        Py_INCREF(Py_None);
        r = Py_None;
    }
    openssl_component_tls_peer_info_free(&info);
    return r;
}

static PyObject *SSLSocket_get_server_hostname(SSLSocketObject *self, void *Py_UNUSED(c))
{
    if (self->server_hostname) {
        Py_INCREF(self->server_hostname);
        return self->server_hostname;
    }
    Py_RETURN_NONE;
}

static PyMethodDef SSLSocket_methods[] = {
    {"read",                    (PyCFunction) SSLSocket_read,                    METH_VARARGS,
     "read([max=8192]) -> bytes — decrypted application data."},
    {"write",                   (PyCFunction) SSLSocket_write,                   METH_VARARGS,
     "write(data: bytes) -> int — bytes encrypted+sent."},
    {"pending",                 (PyCFunction) SSLSocket_pending,                 METH_NOARGS,
     "pending() -> int — always 0 in v1 (openssl-component doesn't expose SSL_pending)."},
    {"shutdown",                (PyCFunction) SSLSocket_shutdown,                METH_NOARGS,
     "shutdown() -> None — close the TLS connection (sends close_notify, closes TCP)."},
    {"version",                 (PyCFunction) SSLSocket_version,                 METH_NOARGS,
     "version() -> str — \"TLSv1.2\" or \"TLSv1.3\"."},
    {"cipher",                  (PyCFunction) SSLSocket_cipher,                  METH_NOARGS,
     "cipher() -> (name, version, secret_bits)"},
    {"peer_cert_der",           (PyCFunction) SSLSocket_peer_cert_der,           METH_NOARGS,
     "peer_cert_der() -> bytes | None — the peer's certificate in DER, or "
     "None if the peer didn't send one."},
    {"selected_alpn_protocol",  (PyCFunction) SSLSocket_selected_alpn_protocol,  METH_NOARGS,
     "selected_alpn_protocol() -> str | None"},
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef SSLSocket_getset[] = {
    {"server_hostname",
     (getter) SSLSocket_get_server_hostname,
     NULL, "SNI server name", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyTypeObject SSLSocket_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "_ssl_capability._SSLSocket",
    .tp_basicsize = sizeof(SSLSocketObject),
    .tp_dealloc   = (destructor) SSLSocket_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "An open TLS connection. Not user-constructible; obtain via\n"
                    "_SSLContext.wrap_socket().",
    .tp_methods   = SSLSocket_methods,
    .tp_getset    = SSLSocket_getset,
};

/* -------------------------------------------------------------------------
 * Module plumbing
 * ------------------------------------------------------------------------- */

/* RAND_bytes(n: int) -> bytes — public random bytes from openssl-component's
 * DRBG. Mirrors ssl.RAND_bytes (and hashlib.RAND_bytes); for high-entropy
 * needs (key material) prefer RAND_priv_bytes which uses the private DRBG. */
static PyObject *mod_RAND_bytes(PyObject *self, PyObject *args)
{
    int n;
    if (!PyArg_ParseTuple(args, "i:RAND_bytes", &n)) return NULL;
    if (n < 0) {
        PyErr_SetString(PyExc_ValueError, "RAND_bytes: n must be >= 0");
        return NULL;
    }
    ssl_import_list_u8_t out;
    openssl_component_random_random_error_t err;
    bool ok = openssl_component_random_bytes((uint32_t) n, &out, &err);
    if (!ok) {
        openssl_component_random_random_error_free(&err);
        PyErr_SetString(SSLError, "RAND_bytes failed (DRBG error)");
        return NULL;
    }
    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr,
                                            (Py_ssize_t) out.len);
    ssl_import_list_u8_free(&out);
    return r;
}

/* RAND_priv_bytes(n) — same but draws from the private DRBG (for key material). */
static PyObject *mod_RAND_priv_bytes(PyObject *self, PyObject *args)
{
    int n;
    if (!PyArg_ParseTuple(args, "i:RAND_priv_bytes", &n)) return NULL;
    if (n < 0) {
        PyErr_SetString(PyExc_ValueError, "RAND_priv_bytes: n must be >= 0");
        return NULL;
    }
    ssl_import_list_u8_t out;
    openssl_component_random_random_error_t err;
    bool ok = openssl_component_random_private_bytes((uint32_t) n, &out, &err);
    if (!ok) {
        openssl_component_random_random_error_free(&err);
        PyErr_SetString(SSLError, "RAND_priv_bytes failed (DRBG error)");
        return NULL;
    }
    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr,
                                            (Py_ssize_t) out.len);
    ssl_import_list_u8_free(&out);
    return r;
}

static PyMethodDef module_methods[] = {
    {"RAND_bytes",      mod_RAND_bytes,      METH_VARARGS,
     "RAND_bytes(n) -> bytes  — public cryptographic random bytes."},
    {"RAND_priv_bytes", mod_RAND_priv_bytes, METH_VARARGS,
     "RAND_priv_bytes(n) -> bytes  — private-DRBG random bytes (key material)."},
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
    if (PyType_Ready(&SSLContext_Type) < 0) return NULL;
    if (PyType_Ready(&SSLSocket_Type) < 0) return NULL;

    PyObject *m = PyModule_Create(&ssl_capability_module);
    if (!m) return NULL;

    SSLError = PyErr_NewException("_ssl_capability.SSLError", PyExc_OSError, NULL);
    if (!SSLError) { Py_DECREF(m); return NULL; }
    Py_INCREF(SSLError);
    if (PyModule_AddObject(m, "SSLError", SSLError) < 0) goto fail;

    Py_INCREF(&MemoryBIO_Type);
    if (PyModule_AddObject(m, "MemoryBIO", (PyObject *) &MemoryBIO_Type) < 0) goto fail;
    Py_INCREF(&SSLContext_Type);
    if (PyModule_AddObject(m, "_SSLContext", (PyObject *) &SSLContext_Type) < 0) goto fail;
    Py_INCREF(&SSLSocket_Type);
    if (PyModule_AddObject(m, "_SSLSocket", (PyObject *) &SSLSocket_Type) < 0) goto fail;

    /* Mirror CPython ssl.CERT_NONE / CERT_OPTIONAL / CERT_REQUIRED. */
    PyModule_AddIntConstant(m, "CERT_NONE", 0);
    PyModule_AddIntConstant(m, "CERT_OPTIONAL", 1);
    PyModule_AddIntConstant(m, "CERT_REQUIRED", 2);
    /* ssl.PROTOCOL_* values we accept in SSLContext(protocol=...). */
    PyModule_AddIntConstant(m, "PROTOCOL_TLS_CLIENT", 16);  /* matches stdlib */

    /* OpenSSL identity strings. openssl-component doesn't expose the live
     * OpenSSL version in its WIT, so we report what its README pins
     * ("OpenSSL 3.x via openssl-wasm wasi-sdk 33") — exact bump bumps here
     * if/when openssl-wasm publishes a query interface. */
    PyModule_AddStringConstant(m, "OPENSSL_VERSION",
        "OpenSSL 3.x (via openssl-wasm component)");
    PyModule_AddIntConstant(m, "OPENSSL_VERSION_NUMBER", 0x30000000);
    PyModule_AddStringConstant(m, "OPENSSL_VERSION_INFO",
        "(3, 0, 0, 0, 0)  # openssl-component-bridged");

    /* WebPKI bundle metadata — for callers that want to assert provenance. */
    PyModule_AddStringConstant(m, "CA_BUNDLE_SHA256", SSL_CAPABILITY_CA_BUNDLE_SHA256);
    PyModule_AddStringConstant(m, "CA_BUNDLE_DATE",   SSL_CAPABILITY_CA_BUNDLE_DATE);
    PyModule_AddIntConstant(m,    "CA_BUNDLE_CERT_COUNT", SSL_CAPABILITY_CA_BUNDLE_CERT_COUNT);

    return m;

fail:
    Py_DECREF(m);
    return NULL;
}

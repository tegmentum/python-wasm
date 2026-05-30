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
                                           const char *server_hostname,
                                           const uint8_t *session_bytes,
                                           Py_ssize_t session_len);

/* wrap_socket(host: str, port: int, server_hostname: str | None,
 *             session: bytes | None) -> _SSLSocket
 *
 * Departure from CPython: CPython's wrap_socket takes a `socket.socket`
 * argument and consumes its fd; openssl-component owns its TCP internally so
 * we take host/port directly. ssl.py-side compatibility shim translates the
 * old signature into ours.
 *
 * `session` is an opaque ticket previously returned by an SSLSocket's
 * .session_ticket() — passed straight to client-config.resume-session for
 * TLS 1.3 session resumption (skips the full handshake when the server
 * accepts the ticket). */
static PyObject *SSLContext_wrap_socket(SSLContextObject *self, PyObject *args, PyObject *kwds)
{
    const char *host;
    int port;
    const char *server_hostname = NULL;
    Py_buffer session_buf = {0};
    int has_session = 0;
    static char *kwlist[] = {"host", "port", "server_hostname", "session", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "si|zy*:wrap_socket", kwlist,
                                     &host, &port, &server_hostname,
                                     &session_buf)) {
        return NULL;
    }
    has_session = (session_buf.buf != NULL);
    if (port < 0 || port > 65535) {
        if (has_session) PyBuffer_Release(&session_buf);
        PyErr_SetString(PyExc_ValueError, "port out of range");
        return NULL;
    }
    PyObject *r = SSLSocket_new_for_context(
        self, host, (uint16_t) port,
        server_hostname ? server_hostname : host,
        has_session ? (const uint8_t *) session_buf.buf : NULL,
        has_session ? session_buf.len : 0);
    if (has_session) PyBuffer_Release(&session_buf);
    return r;
}

/* Forward declaration so SSLContext_methods can take the address — the
 * full definition is below the MemBioSSLClient type, which can't move
 * up because SSLContext_wrap_bio needs MemBioSSLClient_Type. */
static PyObject *SSLContext_wrap_bio(SSLContextObject *ctx, PyObject *args, PyObject *kwds);

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
    {"wrap_bio",            (PyCFunction) SSLContext_wrap_bio,            METH_VARARGS | METH_KEYWORDS,
     "wrap_bio(bio_in, bio_out, server_side=False, server_hostname=None)\n"
     "  -> _MemBioSSLClient\n\n"
     "Memory-BIO TLS client for async transports (anyio, httpx). The\n"
     "returned object owns no socket — pump ciphertext through bio_write\n"
     "/ bio_read in step with your event loop."},
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
                                           const char *server_hostname,
                                           const uint8_t *session_bytes,
                                           Py_ssize_t session_len)
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
    /* Optional TLS 1.3 session ticket for resumption. Cap consumes the
     * bytes verbatim — they're the same blob client.session-ticket()
     * returned earlier. */
    config.resume_session.is_some = (session_bytes != NULL && session_len > 0);
    if (config.resume_session.is_some) {
        config.resume_session.val.ptr = (uint8_t *) malloc((size_t) session_len);
        if (!config.resume_session.val.ptr) {
            free(config.server_name.val.ptr);
            Py_DECREF(self); PyErr_NoMemory(); return NULL;
        }
        memcpy(config.resume_session.val.ptr, session_bytes, (size_t) session_len);
        config.resume_session.val.len = (size_t) session_len;
    }
    /* keylog: zero/none from {0} init */

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
    /* openssl:component/tls@0.2.x adds has-pending (true if OpenSSL has
     * data buffered — decrypted plaintext OR unprocessed ciphertext in
     * its BIO — such that the next read() won't block on the network).
     * Stdlib SSLSocket.pending() returns a byte count but our callers
     * (the SSLSocket.read drain) only care about the boolean. Return 1
     * when has-pending, 0 otherwise. */
    if (!self->has_handle) return PyLong_FromLong(0);
    openssl_component_tls_borrow_client_t b =
        openssl_component_tls_borrow_client(self->handle);
    bool any = openssl_component_tls_method_client_has_pending(b);
    return PyLong_FromLong(any ? 1 : 0);
}

static PyObject *SSLSocket_socket_readable(SSLSocketObject *self, PyObject *Py_UNUSED(args))
{
    /* openssl-component@0.2.x: non-blocking POSIX poll on SSL_get_fd.
     * Picks up the case where a TLS record sits in the kernel TCP
     * buffer but OpenSSL hasn't pulled it into the BIO yet — pending()
     * misses those because SSL_has_pending only sees OpenSSL's BIO.
     * Used by SSLSocket.read's drain loop together with pending(). */
    if (!self->has_handle) Py_RETURN_FALSE;
    openssl_component_tls_borrow_client_t b =
        openssl_component_tls_borrow_client(self->handle);
    bool r = openssl_component_tls_method_client_socket_readable(b);
    if (r) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

static PyObject *SSLSocket_fileno(SSLSocketObject *self, PyObject *Py_UNUSED(args))
{
    /* openssl-component@0.2.x: real fd of the underlying TCP socket
     * (SSL_get_fd). Callers can pass this to select.poll / select.select
     * for application-level readiness — httpx/httpcore poll the socket
     * as part of their async event loop. Returns -1 if the handle is
     * detached. */
    if (!self->has_handle) return PyLong_FromLong(-1);
    openssl_component_tls_borrow_client_t b =
        openssl_component_tls_borrow_client(self->handle);
    int32_t fd = openssl_component_tls_method_client_socket_fd(b);
    return PyLong_FromLong((long)fd);
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

/* ------ x509 certificate-info marshaling (shared by SSLSocket + MemBio) ------
 *
 * Pull `certificate-info` off a borrowed cert handle and project it into a
 * raw dict the Python wrapper massages into the stdlib getpeercert(False)
 * shape. We keep the C side as a thin pass-through (raw OIDs, raw ISO 8601
 * timestamps, raw general-name tags) so the OID→long-name table and date
 * formatting live in Python where they're easy to extend. */

static const char *gn_tag_str(uint8_t tag)
{
    switch (tag) {
        case OPENSSL_COMPONENT_X509_GENERAL_NAME_DNS:   return "DNS";
        case OPENSSL_COMPONENT_X509_GENERAL_NAME_EMAIL: return "email";
        case OPENSSL_COMPONENT_X509_GENERAL_NAME_URI:   return "URI";
        case OPENSSL_COMPONENT_X509_GENERAL_NAME_IP:    return "IP";
        default:                                          return "other";
    }
}

static PyObject *build_name_list(const openssl_component_x509_name_t *n)
{
    PyObject *out = PyList_New((Py_ssize_t) n->len);
    if (!out) return NULL;
    for (size_t i = 0; i < n->len; i++) {
        openssl_component_x509_name_entry_t *e = &n->ptr[i];
        PyObject *t = Py_BuildValue("(s#s#)",
            (const char *) e->oid.ptr,   (Py_ssize_t) e->oid.len,
            (const char *) e->value.ptr, (Py_ssize_t) e->value.len);
        if (!t) { Py_DECREF(out); return NULL; }
        PyList_SET_ITEM(out, (Py_ssize_t) i, t);
    }
    return out;
}

static PyObject *build_san_list(const openssl_component_x509_list_general_name_t *l)
{
    PyObject *out = PyList_New((Py_ssize_t) l->len);
    if (!out) return NULL;
    for (size_t i = 0; i < l->len; i++) {
        openssl_component_x509_general_name_t *g = &l->ptr[i];
        const char *kind = gn_tag_str(g->tag);
        PyObject *t = NULL;
        switch (g->tag) {
            case OPENSSL_COMPONENT_X509_GENERAL_NAME_DNS:
                t = Py_BuildValue("(ss#)", kind,
                    (const char *) g->val.dns.ptr,   (Py_ssize_t) g->val.dns.len);
                break;
            case OPENSSL_COMPONENT_X509_GENERAL_NAME_EMAIL:
                t = Py_BuildValue("(ss#)", kind,
                    (const char *) g->val.email.ptr, (Py_ssize_t) g->val.email.len);
                break;
            case OPENSSL_COMPONENT_X509_GENERAL_NAME_URI:
                t = Py_BuildValue("(ss#)", kind,
                    (const char *) g->val.uri.ptr,   (Py_ssize_t) g->val.uri.len);
                break;
            case OPENSSL_COMPONENT_X509_GENERAL_NAME_IP:
                t = Py_BuildValue("(sy#)", kind,
                    (const char *) g->val.ip.ptr,    (Py_ssize_t) g->val.ip.len);
                break;
            default:
                t = Py_BuildValue("(sy#)", kind,
                    (const char *) g->val.other.ptr, (Py_ssize_t) g->val.other.len);
                break;
        }
        if (!t) { Py_DECREF(out); return NULL; }
        PyList_SET_ITEM(out, (Py_ssize_t) i, t);
    }
    return out;
}

static PyObject *build_cert_info_dict(openssl_component_x509_borrow_certificate_t cert)
{
    openssl_component_x509_certificate_info_t info;
    openssl_component_x509_method_certificate_info(cert, &info);

    PyObject *d = PyDict_New();
    if (!d) { openssl_component_x509_certificate_info_free(&info); return NULL; }

#define SET_OR_FAIL(key, expr) do { \
    PyObject *_v = (expr); \
    if (!_v) goto err; \
    if (PyDict_SetItemString(d, (key), _v) < 0) { Py_DECREF(_v); goto err; } \
    Py_DECREF(_v); \
} while (0)

    SET_OR_FAIL("version", PyLong_FromUnsignedLong(info.version));
    SET_OR_FAIL("serial_hex", PyUnicode_FromStringAndSize(
        (const char *) info.serial_hex.ptr, (Py_ssize_t) info.serial_hex.len));
    SET_OR_FAIL("subject", build_name_list(&info.subject));
    SET_OR_FAIL("issuer",  build_name_list(&info.issuer));
    SET_OR_FAIL("not_before", PyUnicode_FromStringAndSize(
        (const char *) info.validity.not_before.ptr,
        (Py_ssize_t)   info.validity.not_before.len));
    SET_OR_FAIL("not_after", PyUnicode_FromStringAndSize(
        (const char *) info.validity.not_after.ptr,
        (Py_ssize_t)   info.validity.not_after.len));
    SET_OR_FAIL("subject_alt_names", build_san_list(&info.subject_alt_names));
    SET_OR_FAIL("issuer_alt_names",  build_san_list(&info.issuer_alt_names));
    SET_OR_FAIL("signature_algorithm", PyUnicode_FromStringAndSize(
        (const char *) info.signature_algorithm.ptr,
        (Py_ssize_t)   info.signature_algorithm.len));

#undef SET_OR_FAIL

    openssl_component_x509_certificate_info_free(&info);
    return d;

err:
    Py_DECREF(d);
    openssl_component_x509_certificate_info_free(&info);
    return NULL;
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
    bool ok = openssl_component_x509_method_certificate_encode(
        cert, OPENSSL_COMPONENT_PKEY_ENCODING_DER, &out, &err);

    if (!ok) {
        PyErr_SetString(PyExc_RuntimeError,
                        "_ssl_capability.peer_cert_der: x509 encode failed");
        openssl_component_x509_x509_error_free(&err);
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

/* session_ticket() — return the cap's session ticket as bytes (None if
 * the server didn't issue one). Pass back to wrap_socket(..., session=)
 * to resume the session and skip the full handshake on TLS 1.3. */
static PyObject *SSLSocket_session_ticket(SSLSocketObject *self, PyObject *Py_UNUSED(args))
{
    if (!self->has_handle) Py_RETURN_NONE;
    ssl_import_list_u8_t out;
    openssl_component_tls_borrow_client_t b =
        openssl_component_tls_borrow_client(self->handle);
    bool ok = openssl_component_tls_method_client_session_ticket(b, &out);
    if (!ok) Py_RETURN_NONE;
    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr,
                                            (Py_ssize_t) out.len);
    ssl_import_list_u8_free(&out);
    return r;
}

/* peer_chain_der() — return the full peer cert chain as a list of bytes
 * (leaf first). Empty list if the handshake hasn't completed or the peer
 * sent no certificate. Cap-side iterates STACK_OF(X509) + i2d_X509. */
static PyObject *SSLSocket_peer_chain_der(SSLSocketObject *self, PyObject *Py_UNUSED(args))
{
    if (!self->has_handle) return PyList_New(0);
    ssl_import_list_list_u8_t chain;
    openssl_component_tls_borrow_client_t b =
        openssl_component_tls_borrow_client(self->handle);
    openssl_component_tls_method_client_peer_chain_der(b, &chain);
    PyObject *out = PyList_New((Py_ssize_t) chain.len);
    if (!out) { ssl_import_list_list_u8_free(&chain); return NULL; }
    for (size_t i = 0; i < chain.len; i++) {
        PyObject *b = PyBytes_FromStringAndSize(
            (const char *) chain.ptr[i].ptr, (Py_ssize_t) chain.ptr[i].len);
        if (!b) { Py_DECREF(out); ssl_import_list_list_u8_free(&chain); return NULL; }
        PyList_SET_ITEM(out, (Py_ssize_t) i, b);
    }
    ssl_import_list_list_u8_free(&chain);
    return out;
}

/* peer_cert_info() — return a raw dict pulled from
 * x509.certificate.info() on the peer's leaf cert. The Python wrapper
 * (SSLSocket.getpeercert) projects it into the stdlib's nested-tuple
 * shape with long OID names + GMT-formatted dates. Returns None if no
 * peer cert was sent. */
static PyObject *SSLSocket_peer_cert_info(SSLSocketObject *self, PyObject *Py_UNUSED(a))
{
    openssl_component_tls_peer_info_t info;
    if (load_peer_info(self, &info) < 0) return NULL;
    if (info.peer_chain.len == 0) {
        openssl_component_tls_peer_info_free(&info);
        Py_RETURN_NONE;
    }
    openssl_component_x509_borrow_certificate_t bc =
        openssl_component_x509_borrow_certificate(info.peer_chain.ptr[0]);
    PyObject *d = build_cert_info_dict(bc);
    openssl_component_tls_peer_info_free(&info);
    return d;
}

static PyMethodDef SSLSocket_methods[] = {
    {"read",                    (PyCFunction) SSLSocket_read,                    METH_VARARGS,
     "read([max=8192]) -> bytes — decrypted application data."},
    {"write",                   (PyCFunction) SSLSocket_write,                   METH_VARARGS,
     "write(data: bytes) -> int — bytes encrypted+sent."},
    {"pending",                 (PyCFunction) SSLSocket_pending,                 METH_NOARGS,
     "pending() -> int — 1 if openssl-component has buffered data the next read() can return without blocking; 0 otherwise. v0.2.x wires SSL_has_pending; v1 returned a hardcoded 0."},
    {"socket_readable",         (PyCFunction) SSLSocket_socket_readable,         METH_NOARGS,
     "socket_readable() -> bool — True if the underlying TCP socket has bytes available (poll(0)). Used by the read drain loop to catch trailing TLS records that haven't reached the BIO yet."},
    {"fileno",                  (PyCFunction) SSLSocket_fileno,                  METH_NOARGS,
     "fileno() -> int — file descriptor of the underlying TCP socket (SSL_get_fd). Pollable via select.poll / select.select."},
    {"shutdown",                (PyCFunction) SSLSocket_shutdown,                METH_NOARGS,
     "shutdown() -> None — close the TLS connection (sends close_notify, closes TCP)."},
    {"version",                 (PyCFunction) SSLSocket_version,                 METH_NOARGS,
     "version() -> str — \"TLSv1.2\" or \"TLSv1.3\"."},
    {"cipher",                  (PyCFunction) SSLSocket_cipher,                  METH_NOARGS,
     "cipher() -> (name, version, secret_bits)"},
    {"peer_cert_der",           (PyCFunction) SSLSocket_peer_cert_der,           METH_NOARGS,
     "peer_cert_der() -> bytes | None — the peer's certificate in DER, or "
     "None if the peer didn't send one."},
    {"peer_chain_der",          (PyCFunction) SSLSocket_peer_chain_der,          METH_NOARGS,
     "peer_chain_der() -> list[bytes] — full peer cert chain (leaf first), DER."},
    {"peer_cert_info",          (PyCFunction) SSLSocket_peer_cert_info,          METH_NOARGS,
     "peer_cert_info() -> dict | None — raw x509 certificate-info for the\n"
     "peer's leaf cert; Python wrapper massages into stdlib getpeercert shape."},
    {"session_ticket",          (PyCFunction) SSLSocket_session_ticket,          METH_NOARGS,
     "session_ticket() -> bytes | None — opaque TLS 1.3 ticket the server\n"
     "issued for resumption; pass to wrap_socket(..., session=...) on the\n"
     "next connect to skip the full handshake."},
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
 * MemBioSSLClient — memory-BIO mode TLS client. Returned by
 * _SSLContext.wrap_bio(). Wraps openssl:component/tls.mem-bio-client,
 * which owns the SSL state machine but no socket — the caller pumps
 * ciphertext through bio_write/bio_read in their own event loop. This
 * is the surface async TLS libraries need (anyio TLSStream, httpx async
 * transport, ...).
 * ------------------------------------------------------------------------- */

typedef struct {
    PyObject_HEAD
    openssl_component_tls_own_mem_bio_client_t handle;
    int       has_handle;
    PyObject *server_hostname;        /* str — for getter only */
} MemBioSSLClientObject;

static PyTypeObject MemBioSSLClient_Type;

static int mb_ensure_open(MemBioSSLClientObject *self)
{
    if (!self->has_handle) {
        PyErr_SetString(SSLError, "SSL connection is closed");
        return -1;
    }
    return 0;
}

static PyObject *mb_do_handshake(MemBioSSLClientObject *self, PyObject *Py_UNUSED(a))
{
    if (mb_ensure_open(self) < 0) return NULL;
    openssl_component_tls_tls_error_t err;
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    bool ok = openssl_component_tls_method_mem_bio_client_do_handshake(b, &err);
    if (ok) Py_RETURN_NONE;
    /* The would-block tag means "needs more I/O" — surface as
     * SSL_ERROR_WANT_READ-ish by raising SSLError with a distinguishable
     * message so the Python shim can map it to SSLWantReadError. */
    int is_would_block = (err.tag == OPENSSL_COMPONENT_TLS_TLS_ERROR_WOULD_BLOCK);
    openssl_component_tls_tls_error_free(&err);
    if (is_would_block) {
        PyErr_SetString(SSLError, "SSL_ERROR_WANT_READ");
        return NULL;
    }
    PyErr_SetString(SSLError, "TLS handshake failed");
    return NULL;
}

static PyObject *mb_handshake_done(MemBioSSLClientObject *self, PyObject *Py_UNUSED(a))
{
    if (!self->has_handle) Py_RETURN_FALSE;
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    bool r = openssl_component_tls_method_mem_bio_client_handshake_done(b);
    return PyBool_FromLong(r ? 1 : 0);
}

static PyObject *mb_bio_write(MemBioSSLClientObject *self, PyObject *args)
{
    if (mb_ensure_open(self) < 0) return NULL;
    Py_buffer view;
    if (!PyArg_ParseTuple(args, "y*:bio_write", &view)) return NULL;
    ssl_import_list_u8_t input;
    if (list_u8_dup((const uint8_t *) view.buf, (size_t) view.len, &input) < 0) {
        PyBuffer_Release(&view);
        return NULL;
    }
    PyBuffer_Release(&view);
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    uint32_t n = openssl_component_tls_method_mem_bio_client_bio_write(b, &input);
    return PyLong_FromUnsignedLong((unsigned long) n);
}

static PyObject *mb_bio_read(MemBioSSLClientObject *self, PyObject *args)
{
    if (mb_ensure_open(self) < 0) return NULL;
    int max = 16384;
    if (!PyArg_ParseTuple(args, "|i:bio_read", &max)) return NULL;
    if (max <= 0) return PyBytes_FromStringAndSize(NULL, 0);
    ssl_import_list_u8_t out;
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    openssl_component_tls_method_mem_bio_client_bio_read(b, (uint32_t) max, &out);
    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr,
                                            (Py_ssize_t) out.len);
    ssl_import_list_u8_free(&out);
    return r;
}

static PyObject *mb_bio_pending(MemBioSSLClientObject *self, PyObject *Py_UNUSED(a))
{
    if (!self->has_handle) return PyLong_FromLong(0);
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    uint32_t n = openssl_component_tls_method_mem_bio_client_bio_pending(b);
    return PyLong_FromUnsignedLong((unsigned long) n);
}

static PyObject *mb_read(MemBioSSLClientObject *self, PyObject *args)
{
    if (mb_ensure_open(self) < 0) return NULL;
    int max = 8192;
    if (!PyArg_ParseTuple(args, "|i:read", &max)) return NULL;
    if (max <= 0) return PyBytes_FromStringAndSize(NULL, 0);
    ssl_import_list_u8_t out;
    openssl_component_tls_tls_error_t err;
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    bool ok = openssl_component_tls_method_mem_bio_client_read(
        b, (uint32_t) max, &out, &err);
    if (!ok) {
        int is_would_block = (err.tag == OPENSSL_COMPONENT_TLS_TLS_ERROR_WOULD_BLOCK);
        int is_closed = (err.tag == OPENSSL_COMPONENT_TLS_TLS_ERROR_IO_CLOSED);
        openssl_component_tls_tls_error_free(&err);
        if (is_would_block) {
            PyErr_SetString(SSLError, "SSL_ERROR_WANT_READ");
            return NULL;
        }
        if (is_closed) {
            return PyBytes_FromStringAndSize(NULL, 0);
        }
        PyErr_SetString(SSLError, "TLS read failed");
        return NULL;
    }
    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr,
                                            (Py_ssize_t) out.len);
    ssl_import_list_u8_free(&out);
    return r;
}

static PyObject *mb_write(MemBioSSLClientObject *self, PyObject *args)
{
    if (mb_ensure_open(self) < 0) return NULL;
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
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    bool ok = openssl_component_tls_method_mem_bio_client_write(b, &input, &written, &err);
    if (!ok) {
        int is_would_block = (err.tag == OPENSSL_COMPONENT_TLS_TLS_ERROR_WOULD_BLOCK);
        openssl_component_tls_tls_error_free(&err);
        if (is_would_block) {
            PyErr_SetString(SSLError, "SSL_ERROR_WANT_WRITE");
            return NULL;
        }
        PyErr_SetString(SSLError, "TLS write failed");
        return NULL;
    }
    return PyLong_FromUnsignedLong((unsigned long) written);
}

static PyObject *mb_shutdown(MemBioSSLClientObject *self, PyObject *Py_UNUSED(a))
{
    if (!self->has_handle) Py_RETURN_NONE;
    openssl_component_tls_tls_error_t err;
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    bool ok = openssl_component_tls_method_mem_bio_client_shutdown(b, &err);
    if (!ok) {
        int is_would_block = (err.tag == OPENSSL_COMPONENT_TLS_TLS_ERROR_WOULD_BLOCK);
        openssl_component_tls_tls_error_free(&err);
        if (is_would_block) {
            PyErr_SetString(SSLError, "SSL_ERROR_WANT_READ");
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

static PyObject *mb_version(MemBioSSLClientObject *self, PyObject *Py_UNUSED(a))
{
    if (!self->has_handle) Py_RETURN_NONE;
    ssl_import_string_t out;
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    openssl_component_tls_method_mem_bio_client_version(b, &out);
    PyObject *r = PyUnicode_FromStringAndSize((const char *) out.ptr,
                                              (Py_ssize_t) out.len);
    ssl_import_string_free(&out);
    return r;
}

static PyObject *mb_peer_cert_der(MemBioSSLClientObject *self, PyObject *Py_UNUSED(a))
{
    if (!self->has_handle) Py_RETURN_NONE;
    ssl_import_list_u8_t out;
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    bool ok = openssl_component_tls_method_mem_bio_client_peer_cert_der(b, &out);
    if (!ok) Py_RETURN_NONE;
    PyObject *r = PyBytes_FromStringAndSize((const char *) out.ptr,
                                            (Py_ssize_t) out.len);
    ssl_import_list_u8_free(&out);
    return r;
}

static PyObject *mb_peer_chain_der(MemBioSSLClientObject *self, PyObject *Py_UNUSED(args))
{
    if (!self->has_handle) return PyList_New(0);
    ssl_import_list_list_u8_t chain;
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    openssl_component_tls_method_mem_bio_client_peer_chain_der(b, &chain);
    PyObject *out = PyList_New((Py_ssize_t) chain.len);
    if (!out) { ssl_import_list_list_u8_free(&chain); return NULL; }
    for (size_t i = 0; i < chain.len; i++) {
        PyObject *bb = PyBytes_FromStringAndSize(
            (const char *) chain.ptr[i].ptr, (Py_ssize_t) chain.ptr[i].len);
        if (!bb) { Py_DECREF(out); ssl_import_list_list_u8_free(&chain); return NULL; }
        PyList_SET_ITEM(out, (Py_ssize_t) i, bb);
    }
    ssl_import_list_list_u8_free(&chain);
    return out;
}

/* mb_peer_cert_info() — counterpart to SSLSocket_peer_cert_info for the
 * memory-BIO client. The mem-bio cap doesn't surface owned cert handles
 * on `peer()` (it returns the leaf as DER), so we have to parse the DER
 * back into an x509.certificate to call info() on it, then drop. */
static PyObject *mb_peer_cert_info(MemBioSSLClientObject *self, PyObject *Py_UNUSED(a))
{
    if (!self->has_handle) Py_RETURN_NONE;
    ssl_import_list_u8_t der;
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    if (!openssl_component_tls_method_mem_bio_client_peer_cert_der(b, &der))
        Py_RETURN_NONE;

    openssl_component_x509_own_certificate_t cert;
    openssl_component_x509_x509_error_t err;
    bool parsed = openssl_component_x509_static_certificate_parse(
        &der, OPENSSL_COMPONENT_PKEY_ENCODING_DER, &cert, &err);
    if (!parsed) {
        openssl_component_x509_x509_error_free(&err);
        Py_RETURN_NONE;
    }
    openssl_component_x509_borrow_certificate_t bc =
        openssl_component_x509_borrow_certificate(cert);
    PyObject *d = build_cert_info_dict(bc);
    openssl_component_x509_certificate_drop_own(cert);
    return d;
}

static PyObject *mb_selected_alpn_protocol(MemBioSSLClientObject *self, PyObject *Py_UNUSED(a))
{
    if (!self->has_handle) Py_RETURN_NONE;
    ssl_import_string_t out;
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    bool ok = openssl_component_tls_method_mem_bio_client_selected_alpn_protocol(b, &out);
    if (!ok) Py_RETURN_NONE;
    PyObject *r = PyUnicode_FromStringAndSize((const char *) out.ptr,
                                              (Py_ssize_t) out.len);
    ssl_import_string_free(&out);
    return r;
}

/* cipher() — stdlib SSLObject.cipher() returns
 *   (cipher_name: str, protocol_version: str, secret_bits: int)
 * Built from the cap's peer_info.cipher_suite + protocol enum;
 * secret_bits is the conventional 256 for AES-256-GCM / 128 for
 * AES-128-GCM. We surface 0 when the cap doesn't expose secret bits. */
static PyObject *mb_cipher(MemBioSSLClientObject *self, PyObject *Py_UNUSED(a))
{
    if (!self->has_handle) Py_RETURN_NONE;
    openssl_component_tls_peer_info_t pi;
    openssl_component_tls_borrow_mem_bio_client_t b =
        openssl_component_tls_borrow_mem_bio_client(self->handle);
    openssl_component_tls_method_mem_bio_client_peer(b, &pi);
    /* protocol enum: tls12=0, tls13=1, dtls12=2. Map to stdlib string. */
    const char *proto = (pi.protocol == 1) ? "TLSv1.3"
                      : (pi.protocol == 2) ? "DTLSv1.2"
                      : "TLSv1.2";
    PyObject *name = PyUnicode_FromStringAndSize(
        (const char *) pi.cipher_suite.ptr, (Py_ssize_t) pi.cipher_suite.len);
    PyObject *r = name
        ? Py_BuildValue("(OsI)", name, proto, 0u)
        : NULL;
    Py_XDECREF(name);
    openssl_component_tls_peer_info_free(&pi);
    return r;
}

static PyObject *mb_get_server_hostname(MemBioSSLClientObject *self, void *Py_UNUSED(c))
{
    if (self->server_hostname) {
        Py_INCREF(self->server_hostname);
        return self->server_hostname;
    }
    Py_RETURN_NONE;
}

static void MemBioSSLClient_dealloc(MemBioSSLClientObject *self)
{
    if (self->has_handle) {
        openssl_component_tls_static_mem_bio_client_close(self->handle);
        self->has_handle = 0;
    }
    Py_XDECREF(self->server_hostname);
    PyObject_Free(self);
}

static PyMethodDef MemBioSSLClient_methods[] = {
    {"do_handshake",            (PyCFunction) mb_do_handshake,            METH_NOARGS,
     "Drive the handshake; raises SSLError(SSL_ERROR_WANT_READ) until enough\n"
     "ciphertext has been bio_write()-ed."},
    {"handshake_done",          (PyCFunction) mb_handshake_done,          METH_NOARGS,
     "True once the handshake has succeeded."},
    {"bio_write",               (PyCFunction) mb_bio_write,               METH_VARARGS,
     "bio_write(b: bytes) -> int — inject ciphertext from the network."},
    {"bio_read",                (PyCFunction) mb_bio_read,                METH_VARARGS,
     "bio_read([n=16384]) -> bytes — drain ciphertext for the network."},
    {"bio_pending",             (PyCFunction) mb_bio_pending,             METH_NOARGS,
     "Bytes the TLS layer wants to send."},
    {"read",                    (PyCFunction) mb_read,                    METH_VARARGS,
     "read([n=8192]) -> bytes — decrypted plaintext."},
    {"write",                   (PyCFunction) mb_write,                   METH_VARARGS,
     "write(b: bytes) -> int — encrypt and queue for sending."},
    {"shutdown",                (PyCFunction) mb_shutdown,                METH_NOARGS,
     "Send close_notify."},
    {"version",                 (PyCFunction) mb_version,                 METH_NOARGS,
     "Negotiated protocol version."},
    {"peer_cert_der",           (PyCFunction) mb_peer_cert_der,           METH_NOARGS,
     "Peer certificate in DER, or None before handshake."},
    {"peer_chain_der",          (PyCFunction) mb_peer_chain_der,          METH_NOARGS,
     "Full peer cert chain (leaf first), DER bytes per entry."},
    {"peer_cert_info",          (PyCFunction) mb_peer_cert_info,          METH_NOARGS,
     "peer_cert_info() -> dict | None — raw x509 certificate-info; the\n"
     "Python wrapper formats it for stdlib getpeercert."},
    {"selected_alpn_protocol",  (PyCFunction) mb_selected_alpn_protocol,  METH_NOARGS,
     "Negotiated ALPN protocol, or None."},
    {"cipher",                  (PyCFunction) mb_cipher,                  METH_NOARGS,
     "cipher() -> (name, protocol_version, secret_bits)\n"
     "Stdlib SSLObject.cipher shape. Returns None before handshake."},
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef MemBioSSLClient_getset[] = {
    {"server_hostname",  (getter) mb_get_server_hostname, NULL,
     "SNI / hostname recorded at wrap_bio time.", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyTypeObject MemBioSSLClient_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "_ssl_capability._MemBioSSLClient",
    .tp_basicsize = sizeof(MemBioSSLClientObject),
    .tp_dealloc   = (destructor) MemBioSSLClient_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "Memory-BIO TLS client. Not user-constructible; obtain\n"
                    "via _SSLContext.wrap_bio().",
    .tp_methods   = MemBioSSLClient_methods,
    .tp_getset    = MemBioSSLClient_getset,
};

/* SSLContext.wrap_bio(bio_in, bio_out, server_side=False,
 *                     server_hostname=None) -> _MemBioSSLClient
 *
 * Departs from CPython: we don't actually retain the user's MemoryBIO
 * instances — the cap owns its own internal BIOs, and the Python shim
 * (Lib/ssl_capability.py) pumps bytes between the user's bios and
 * ours. The signature matches CPython's so the shim can pass through
 * the MemoryBIO objects to its SSLObject wrapper. */
static PyObject *SSLContext_wrap_bio(SSLContextObject *ctx, PyObject *args, PyObject *kwds)
{
    PyObject *bio_in;
    PyObject *bio_out;
    int server_side = 0;
    const char *server_hostname = NULL;
    static char *kwlist[] = {"bio_in", "bio_out", "server_side", "server_hostname", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|iz:wrap_bio", kwlist,
                                     &bio_in, &bio_out, &server_side, &server_hostname)) {
        return NULL;
    }
    if (server_side) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "wrap_bio: server_side not implemented yet");
        return NULL;
    }
    /* Lazy trust store, identical to wrap_socket. */
    if (!ctx->has_trust_store && ctx->ca_pem_bytes != NULL) {
        if (SSLContext_build_trust_store(ctx) < 0) return NULL;
    }

    openssl_component_tls_client_config_t config = {0};
    config.protocols.min = (uint8_t) ctx->min_protocol;
    config.protocols.max = (uint8_t) ctx->max_protocol;
    config.verify = (uint8_t) ctx->verify_mode;
    config.trust.is_some = false;
    if (ctx->has_trust_store) {
        config.trust.is_some = true;
        config.trust.val = ctx->trust_store;
        ctx->has_trust_store = 0;
    }
    config.verify_options.is_some = false;
    config.server_name.is_some = (server_hostname != NULL);
    if (config.server_name.is_some) {
        size_t n = strlen(server_hostname);
        config.server_name.val.ptr = (uint8_t *) malloc(n);
        if (!config.server_name.val.ptr) { PyErr_NoMemory(); return NULL; }
        memcpy(config.server_name.val.ptr, server_hostname, n);
        config.server_name.val.len = n;
    }
    config.client_cert.is_some = false;
    config.client_key.is_some = false;
    config.alpn.is_some = (ctx->alpn_protocols != NULL
                           && PyList_Size(ctx->alpn_protocols) > 0);
    if (config.alpn.is_some) {
        if (alpn_list_from_pylist(ctx->alpn_protocols, &config.alpn.val.protocols) < 0) {
            if (config.server_name.is_some) free(config.server_name.val.ptr);
            return NULL;
        }
    }
    config.ciphers.is_some = false;
    config.groups.is_some = false;
    config.enable_early_data = false;

    openssl_component_tls_own_mem_bio_client_t cap_client;
    openssl_component_tls_tls_error_t err;
    bool ok = openssl_component_tls_static_mem_bio_client_new(
        &config, &cap_client, &err);
    if (!ok) {
        return raise_tls_error("wrap_bio failed", &err);
    }

    MemBioSSLClientObject *self = PyObject_New(MemBioSSLClientObject,
                                                &MemBioSSLClient_Type);
    if (!self) {
        openssl_component_tls_static_mem_bio_client_close(cap_client);
        return NULL;
    }
    self->handle = cap_client;
    self->has_handle = 1;
    self->server_hostname = (server_hostname
                             ? PyUnicode_FromString(server_hostname)
                             : NULL);
    /* Note: bio_in / bio_out are accepted to match CPython's signature
     * but not used here — the shim layer keeps references and pumps
     * bytes through them. Suppress unused-arg warnings. */
    (void) bio_in; (void) bio_out;
    return (PyObject *) self;
}

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
    if (PyType_Ready(&MemBioSSLClient_Type) < 0) return NULL;

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
    Py_INCREF(&MemBioSSLClient_Type);
    if (PyModule_AddObject(m, "_MemBioSSLClient", (PyObject *) &MemBioSSLClient_Type) < 0) goto fail;

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

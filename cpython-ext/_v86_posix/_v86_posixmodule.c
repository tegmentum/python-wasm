/* _v86_posix — Python C extension routing to the v86 POSIX-extension
 * capability over the Component Model.
 *
 * Statically linked into wasi-sdk CPython (registered alongside the other
 * cpython-ext capability extensions). The imported WIT functions appear as
 * wasm imports on python-component.wasm, satisfied at compose time by
 * wac/composectl plugging v86-component.wasm into the import.
 *
 * Python surface (intentionally minimal — the higher-level subprocess.Popen
 * shim lives separately):
 *
 *   _v86_posix.STDIO_INHERIT = 0
 *   _v86_posix.STDIO_PIPED   = 1
 *   _v86_posix.STDIO_NULL    = 2
 *
 *   _v86_posix.spawn(
 *       program: str,
 *       args: Sequence[str] = (),
 *       env:  Sequence[Tuple[str, str]] | None = None,   # None = inherit guest env
 *       cwd:  str | None = None,
 *       stdin:  int = STDIO_INHERIT,
 *       stdout: int = STDIO_INHERIT,
 *       stderr: int = STDIO_INHERIT,
 *   ) -> Process
 *
 *   class Process:
 *       def pid(self) -> int                              # PID inside the guest
 *       def signal(self, signum: int) -> None             # POSIX signal num
 *       def wait(self) -> tuple[str, int]                 # ("exited", code) | ("signaled", sig)
 *       def try_wait(self) -> tuple[str, int] | None      # None = still running
 *       def take_stdin(self) -> object                    # NotImplementedError, see below
 *       def take_stdout(self) -> object                   # NotImplementedError, see below
 *       def take_stderr(self) -> object                   # NotImplementedError, see below
 *
 * Stdio accessors. The WIT returns wasi:io/streams handles; wrapping those
 * as Python file-like objects is a cross-extension concern (the SSL, HTTP,
 * etc. paths all face the same need) and intentionally lives outside this
 * module. Until that lands, take-stdin/out/err raise NotImplementedError.
 *
 * Errors. spawn maps each WIT spawn-error variant to a specific exception
 * type so callers can branch on classification rather than parsing strings.
 * signal maps signal-error similarly. See SpawnError / GuestNotReadyError
 * etc. below.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "gen/v86_posix_import.h"

/* ===================================================================
 * Exceptions
 * =================================================================== */

static PyObject *SpawnError_base = NULL;      /* base for all spawn failures */
static PyObject *ProgramNotFoundError = NULL;
static PyObject *ExecFailedError = NULL;
static PyObject *TooManyProcessesError = NULL;
static PyObject *InvalidArgumentError = NULL;
static PyObject *GuestNotReadyError = NULL;
static PyObject *SignalError_base = NULL;     /* base for all signal failures */
static PyObject *NoSuchProcessError = NULL;
static PyObject *InvalidSignalError = NULL;

/* ===================================================================
 * String helpers
 * =================================================================== */

/* Copy a Python str into a wit-bindgen string (caller-owned heap copy).
 * Returns -1 on failure (Python exception set). */
static int pystr_to_wit(PyObject *src, v86_posix_import_string_t *out)
{
    Py_ssize_t len;
    const char *utf8 = PyUnicode_AsUTF8AndSize(src, &len);
    if (utf8 == NULL) {
        return -1;
    }
    if (len > 0) {
        out->ptr = (uint8_t *) malloc((size_t) len);
        if (out->ptr == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        memcpy(out->ptr, utf8, (size_t) len);
    } else {
        out->ptr = NULL;
    }
    out->len = (size_t) len;
    return 0;
}

static PyObject *wit_to_pystr(v86_posix_import_string_t *src)
{
    return PyUnicode_FromStringAndSize((const char *) src->ptr,
                                       (Py_ssize_t) src->len);
}

/* Free a wit string we filled with pystr_to_wit (the callee owns it after
 * passing it into a WIT call; this is for our own cleanup on the err path). */
static void wit_str_free_local(v86_posix_import_string_t *s)
{
    if (s->ptr) {
        free(s->ptr);
        s->ptr = NULL;
        s->len = 0;
    }
}

/* ===================================================================
 * Error mapping
 * =================================================================== */

/* Raise a SpawnError-tree exception from a wit-bindgen spawn-error variant.
 * Always consumes ownership of `err`. */
static void raise_spawn_error(v86_posix_process_spawn_error_t *err)
{
    PyObject *exc = NULL;
    PyObject *msg = NULL;
    switch (err->tag) {
    case V86_POSIX_PROCESS_SPAWN_ERROR_PROGRAM_NOT_FOUND:
        exc = ProgramNotFoundError;
        msg = wit_to_pystr(&err->val.program_not_found);
        break;
    case V86_POSIX_PROCESS_SPAWN_ERROR_EXEC_FAILED:
        exc = ExecFailedError;
        msg = wit_to_pystr(&err->val.exec_failed);
        break;
    case V86_POSIX_PROCESS_SPAWN_ERROR_TOO_MANY_PROCESSES:
        exc = TooManyProcessesError;
        msg = PyUnicode_FromString("guest process-table limit reached");
        break;
    case V86_POSIX_PROCESS_SPAWN_ERROR_INVALID_ARGUMENT:
        exc = InvalidArgumentError;
        msg = wit_to_pystr(&err->val.invalid_argument);
        break;
    case V86_POSIX_PROCESS_SPAWN_ERROR_GUEST_NOT_READY:
        exc = GuestNotReadyError;
        msg = PyUnicode_FromString("v86 guest has not yet reached userspace");
        break;
    default:
        exc = SpawnError_base;
        msg = PyUnicode_FromFormat("unknown spawn-error variant: %u",
                                   (unsigned) err->tag);
        break;
    }
    if (exc && msg) {
        PyErr_SetObject(exc, msg);
    } else if (exc) {
        PyErr_SetString(exc, "spawn failed");
    }
    Py_XDECREF(msg);
    v86_posix_process_spawn_error_free(err);
}

static void raise_signal_error(v86_posix_process_signal_error_t *err)
{
    PyObject *exc = SignalError_base;
    const char *msg = "signal failed";
    switch (err->tag) {
    case V86_POSIX_PROCESS_SIGNAL_ERROR_NO_SUCH_PROCESS:
        exc = NoSuchProcessError;
        msg = "process has already exited";
        break;
    case V86_POSIX_PROCESS_SIGNAL_ERROR_INVALID_SIGNAL:
        exc = InvalidSignalError;
        msg = "guest rejected the signal (out of range)";
        break;
    }
    PyErr_SetString(exc, msg);
    v86_posix_process_signal_error_free(err);
}

/* ===================================================================
 * Process type
 * =================================================================== */

typedef struct {
    PyObject_HEAD
    v86_posix_process_own_process_t handle;
    bool owned;          /* false after drop_own (post-final-wait or on dealloc) */
} ProcessObject;

static PyTypeObject ProcessType;   /* fwd */

static PyObject *process_new(v86_posix_process_own_process_t handle)
{
    ProcessObject *p = PyObject_New(ProcessObject, &ProcessType);
    if (p == NULL) {
        /* Have to drop the handle ourselves; nothing else owns it. */
        v86_posix_process_process_drop_own(handle);
        return NULL;
    }
    p->handle = handle;
    p->owned = true;
    return (PyObject *) p;
}

static void process_dealloc(ProcessObject *self)
{
    if (self->owned) {
        v86_posix_process_process_drop_own(self->handle);
        self->owned = false;
    }
    PyObject_Del(self);
}

static PyObject *process_pid(ProcessObject *self, PyObject *Py_UNUSED(args))
{
    if (!self->owned) {
        PyErr_SetString(PyExc_RuntimeError, "process handle has been dropped");
        return NULL;
    }
    v86_posix_process_borrow_process_t b =
        v86_posix_process_borrow_process(self->handle);
    uint32_t pid = v86_posix_process_method_process_pid(b);
    return PyLong_FromUnsignedLong((unsigned long) pid);
}

static PyObject *process_signal(ProcessObject *self, PyObject *args)
{
    int signum;
    if (!PyArg_ParseTuple(args, "i:signal", &signum)) {
        return NULL;
    }
    if (signum < 0 || signum > (int) UINT32_MAX) {
        PyErr_SetString(PyExc_OverflowError, "signum out of range");
        return NULL;
    }
    if (!self->owned) {
        PyErr_SetString(PyExc_RuntimeError, "process handle has been dropped");
        return NULL;
    }
    v86_posix_process_borrow_process_t b =
        v86_posix_process_borrow_process(self->handle);
    v86_posix_process_signal_error_t err;
    bool ok = v86_posix_process_method_process_signal(b, (uint32_t) signum, &err);
    if (!ok) {
        raise_signal_error(&err);
        return NULL;
    }
    Py_RETURN_NONE;
}

/* Translate an exit-status into a 2-tuple ("exited"|"signaled", int). */
static PyObject *exit_status_to_tuple(const v86_posix_process_exit_status_t *st)
{
    switch (st->tag) {
    case V86_POSIX_PROCESS_EXIT_STATUS_EXITED:
        return Py_BuildValue("(si)", "exited", (int) st->val.exited);
    case V86_POSIX_PROCESS_EXIT_STATUS_SIGNALED:
        return Py_BuildValue("(sk)", "signaled", (unsigned long) st->val.signaled);
    default:
        PyErr_Format(PyExc_RuntimeError, "unknown exit-status variant: %u",
                     (unsigned) st->tag);
        return NULL;
    }
}

static PyObject *process_wait(ProcessObject *self, PyObject *Py_UNUSED(args))
{
    if (!self->owned) {
        PyErr_SetString(PyExc_RuntimeError, "process handle has been dropped");
        return NULL;
    }
    v86_posix_process_borrow_process_t b =
        v86_posix_process_borrow_process(self->handle);
    v86_posix_process_exit_status_t st;
    /* wait may block — release the GIL. */
    Py_BEGIN_ALLOW_THREADS
    v86_posix_process_method_process_wait(b, &st);
    Py_END_ALLOW_THREADS
    return exit_status_to_tuple(&st);
}

static PyObject *process_try_wait(ProcessObject *self, PyObject *Py_UNUSED(args))
{
    if (!self->owned) {
        PyErr_SetString(PyExc_RuntimeError, "process handle has been dropped");
        return NULL;
    }
    v86_posix_process_borrow_process_t b =
        v86_posix_process_borrow_process(self->handle);
    v86_posix_process_exit_status_t st;
    bool present = v86_posix_process_method_process_try_wait(b, &st);
    if (!present) {
        Py_RETURN_NONE;
    }
    return exit_status_to_tuple(&st);
}

/* ===================================================================
 * InputStream / OutputStream — Python file-like wrappers around the
 * wasi:io/streams handles `take_std{in,out,err}` returns.
 *
 * Forward decls so process_take_* can construct them; full type/methods
 * appear below.
 * =================================================================== */

typedef struct {
    PyObject_HEAD
    wasi_io_streams_own_input_stream_t handle;
    bool owned;
} InputStreamObject;

typedef struct {
    PyObject_HEAD
    wasi_io_streams_own_output_stream_t handle;
    bool owned;
} OutputStreamObject;

static PyTypeObject InputStreamType;
static PyTypeObject OutputStreamType;

/* Construct an InputStream wrapping an owned handle. Steals ownership. */
static PyObject *input_stream_new(wasi_io_streams_own_input_stream_t handle)
{
    InputStreamObject *s = PyObject_New(InputStreamObject, &InputStreamType);
    if (s == NULL) {
        wasi_io_streams_input_stream_drop_own(handle);
        return NULL;
    }
    s->handle = handle;
    s->owned = true;
    return (PyObject *) s;
}

static PyObject *output_stream_new(wasi_io_streams_own_output_stream_t handle)
{
    OutputStreamObject *s = PyObject_New(OutputStreamObject, &OutputStreamType);
    if (s == NULL) {
        wasi_io_streams_output_stream_drop_own(handle);
        return NULL;
    }
    s->handle = handle;
    s->owned = true;
    return (PyObject *) s;
}

/* Raise an OSError carrying the wasi:io/error debug string. Always consumes
 * ownership of `err`. */
static void raise_stream_error(wasi_io_streams_stream_error_t *err,
                               const char *op)
{
    if (err->tag == WASI_IO_STREAMS_STREAM_ERROR_CLOSED) {
        /* Caller decides whether this is EOF (input) or BrokenPipeError
         * (output); the caller should branch before calling this helper.
         * Defensive default: generic OSError. */
        PyErr_Format(PyExc_OSError, "%s on a closed stream", op);
    } else {
        v86_posix_import_string_t debug;
        wasi_io_error_borrow_error_t e =
            wasi_io_error_borrow_error(err->val.last_operation_failed);
        wasi_io_error_method_error_to_debug_string(e, &debug);
        PyErr_Format(PyExc_OSError, "%s failed: %.*s",
                     op, (int) debug.len, (const char *) debug.ptr);
        v86_posix_import_string_free(&debug);
    }
    wasi_io_streams_stream_error_free(err);
}

/* --- Process.take_std{in,out,err} — now actually constructs streams --- */

static PyObject *process_take_stdin(ProcessObject *self, PyObject *Py_UNUSED(args))
{
    if (!self->owned) {
        PyErr_SetString(PyExc_RuntimeError, "process handle has been dropped");
        return NULL;
    }
    v86_posix_process_borrow_process_t b =
        v86_posix_process_borrow_process(self->handle);
    v86_posix_process_own_output_stream_t out;
    bool present = v86_posix_process_method_process_take_stdin(b, &out);
    if (!present) {
        Py_RETURN_NONE;
    }
    return output_stream_new(out);
}

static PyObject *process_take_stdout(ProcessObject *self, PyObject *Py_UNUSED(args))
{
    if (!self->owned) {
        PyErr_SetString(PyExc_RuntimeError, "process handle has been dropped");
        return NULL;
    }
    v86_posix_process_borrow_process_t b =
        v86_posix_process_borrow_process(self->handle);
    v86_posix_process_own_input_stream_t out;
    bool present = v86_posix_process_method_process_take_stdout(b, &out);
    if (!present) {
        Py_RETURN_NONE;
    }
    return input_stream_new(out);
}

static PyObject *process_take_stderr(ProcessObject *self, PyObject *Py_UNUSED(args))
{
    if (!self->owned) {
        PyErr_SetString(PyExc_RuntimeError, "process handle has been dropped");
        return NULL;
    }
    v86_posix_process_borrow_process_t b =
        v86_posix_process_borrow_process(self->handle);
    v86_posix_process_own_input_stream_t out;
    bool present = v86_posix_process_method_process_take_stderr(b, &out);
    if (!present) {
        Py_RETURN_NONE;
    }
    return input_stream_new(out);
}

/* --- InputStream method bodies --- */

static void input_stream_dealloc(InputStreamObject *self)
{
    if (self->owned) {
        wasi_io_streams_input_stream_drop_own(self->handle);
        self->owned = false;
    }
    PyObject_Del(self);
}

static PyObject *input_stream_close(InputStreamObject *self, PyObject *Py_UNUSED(a))
{
    if (self->owned) {
        wasi_io_streams_input_stream_drop_own(self->handle);
        self->owned = false;
    }
    Py_RETURN_NONE;
}

static PyObject *input_stream_get_closed(InputStreamObject *self, void *Py_UNUSED(c))
{
    return PyBool_FromLong(!self->owned);
}

static PyObject *input_stream_readable(InputStreamObject *Py_UNUSED(self),
                                       PyObject *Py_UNUSED(a))
{ Py_RETURN_TRUE; }

static PyObject *input_stream_writable(InputStreamObject *Py_UNUSED(self),
                                       PyObject *Py_UNUSED(a))
{ Py_RETURN_FALSE; }

static PyObject *input_stream_seekable(InputStreamObject *Py_UNUSED(self),
                                       PyObject *Py_UNUSED(a))
{ Py_RETURN_FALSE; }

static PyObject *input_stream_fileno(InputStreamObject *Py_UNUSED(self),
                                     PyObject *Py_UNUSED(a))
{
    PyErr_SetString(PyExc_OSError, "stream has no host file descriptor");
    return NULL;
}

/* Chunk size for read(-1)'s loop. Sized for typical pipe latency vs
 * round-trip cost; the upstream stdlib io.BufferedReader uses 8 KiB. */
#define V86_INPUT_READ_CHUNK 8192

static PyObject *input_stream_read(InputStreamObject *self, PyObject *args)
{
    Py_ssize_t size = -1;
    if (!PyArg_ParseTuple(args, "|n:read", &size)) {
        return NULL;
    }
    if (!self->owned) {
        PyErr_SetString(PyExc_ValueError, "I/O operation on closed stream");
        return NULL;
    }
    wasi_io_streams_borrow_input_stream_t borrow =
        wasi_io_streams_borrow_input_stream(self->handle);

    if (size >= 0) {
        v86_posix_import_list_u8_t out;
        wasi_io_streams_stream_error_t err;
        bool ok;
        Py_BEGIN_ALLOW_THREADS
        ok = wasi_io_streams_method_input_stream_blocking_read(
            borrow, (uint64_t) size, &out, &err);
        Py_END_ALLOW_THREADS
        if (!ok) {
            if (err.tag == WASI_IO_STREAMS_STREAM_ERROR_CLOSED) {
                wasi_io_streams_stream_error_free(&err);
                return PyBytes_FromStringAndSize(NULL, 0);
            }
            raise_stream_error(&err, "read");
            return NULL;
        }
        PyObject *bytes = PyBytes_FromStringAndSize(
            (const char *) out.ptr, (Py_ssize_t) out.len);
        v86_posix_import_list_u8_free(&out);
        return bytes;
    }

    /* size < 0 — read to EOF, accumulating chunks. */
    PyObject *buf = PyBytes_FromStringAndSize(NULL, 0);
    if (buf == NULL) return NULL;
    while (1) {
        v86_posix_import_list_u8_t out;
        wasi_io_streams_stream_error_t err;
        bool ok;
        Py_BEGIN_ALLOW_THREADS
        ok = wasi_io_streams_method_input_stream_blocking_read(
            borrow, (uint64_t) V86_INPUT_READ_CHUNK, &out, &err);
        Py_END_ALLOW_THREADS
        if (!ok) {
            if (err.tag == WASI_IO_STREAMS_STREAM_ERROR_CLOSED) {
                wasi_io_streams_stream_error_free(&err);
                return buf;
            }
            raise_stream_error(&err, "read");
            Py_DECREF(buf);
            return NULL;
        }
        if (out.len > 0) {
            if (_PyBytes_Resize(&buf, PyBytes_GET_SIZE(buf) + (Py_ssize_t) out.len) < 0) {
                v86_posix_import_list_u8_free(&out);
                return NULL;
            }
            memcpy(PyBytes_AS_STRING(buf) + PyBytes_GET_SIZE(buf) - out.len,
                   out.ptr, out.len);
        }
        v86_posix_import_list_u8_free(&out);
        /* blocking_read either returns >=1 byte or the closed-error path;
         * a 0-byte success would loop forever. Guard defensively. */
        if (out.len == 0) {
            return buf;
        }
    }
}

static PyObject *input_stream_readinto(InputStreamObject *self, PyObject *args)
{
    Py_buffer view;
    if (!PyArg_ParseTuple(args, "w*:readinto", &view)) {
        return NULL;
    }
    if (!self->owned) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "I/O operation on closed stream");
        return NULL;
    }
    wasi_io_streams_borrow_input_stream_t borrow =
        wasi_io_streams_borrow_input_stream(self->handle);
    v86_posix_import_list_u8_t out;
    wasi_io_streams_stream_error_t err;
    bool ok;
    Py_BEGIN_ALLOW_THREADS
    ok = wasi_io_streams_method_input_stream_blocking_read(
        borrow, (uint64_t) view.len, &out, &err);
    Py_END_ALLOW_THREADS
    if (!ok) {
        if (err.tag == WASI_IO_STREAMS_STREAM_ERROR_CLOSED) {
            wasi_io_streams_stream_error_free(&err);
            PyBuffer_Release(&view);
            return PyLong_FromLong(0);
        }
        raise_stream_error(&err, "readinto");
        PyBuffer_Release(&view);
        return NULL;
    }
    Py_ssize_t n = (Py_ssize_t) out.len;
    if (n > view.len) n = view.len;
    if (n > 0) {
        memcpy(view.buf, out.ptr, (size_t) n);
    }
    v86_posix_import_list_u8_free(&out);
    PyBuffer_Release(&view);
    return PyLong_FromSsize_t(n);
}

static PyObject *input_stream_enter(InputStreamObject *self, PyObject *Py_UNUSED(a))
{
    Py_INCREF(self);
    return (PyObject *) self;
}

static PyObject *input_stream_exit(InputStreamObject *self, PyObject *Py_UNUSED(a))
{
    if (self->owned) {
        wasi_io_streams_input_stream_drop_own(self->handle);
        self->owned = false;
    }
    Py_RETURN_NONE;
}

static PyMethodDef input_stream_methods[] = {
    {"read",     (PyCFunction) input_stream_read,     METH_VARARGS,
     "read(size=-1) -> bytes — blocking read; size<0 reads to EOF."},
    {"readinto", (PyCFunction) input_stream_readinto, METH_VARARGS,
     "readinto(buf) -> int — blocking read into a writable buffer."},
    {"close",    (PyCFunction) input_stream_close,    METH_NOARGS,
     "Release the underlying wasi:io stream handle."},
    {"readable", (PyCFunction) input_stream_readable, METH_NOARGS, "True."},
    {"writable", (PyCFunction) input_stream_writable, METH_NOARGS, "False."},
    {"seekable", (PyCFunction) input_stream_seekable, METH_NOARGS, "False."},
    {"fileno",   (PyCFunction) input_stream_fileno,   METH_NOARGS,
     "Always raises — wasi:io streams are not fd-backed."},
    {"__enter__", (PyCFunction) input_stream_enter,   METH_NOARGS, NULL},
    {"__exit__",  (PyCFunction) input_stream_exit,    METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef input_stream_getset[] = {
    {"closed", (getter) input_stream_get_closed, NULL,
     "True once close() has been called.", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyTypeObject InputStreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_v86_posix.InputStream",
    .tp_basicsize = sizeof(InputStreamObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor) input_stream_dealloc,
    .tp_methods = input_stream_methods,
    .tp_getset = input_stream_getset,
    .tp_doc = "Python file-like wrapper for a wasi:io/streams input-stream.\n\n"
              "Supports read / readinto / context-manager / close. Not seekable.\n"
              "Returned from Process.take_stdout() / take_stderr() when the\n"
              "corresponding stdio was spawned with PIPE.",
};

/* --- OutputStream method bodies --- */

static void output_stream_dealloc(OutputStreamObject *self)
{
    if (self->owned) {
        wasi_io_streams_output_stream_drop_own(self->handle);
        self->owned = false;
    }
    PyObject_Del(self);
}

static PyObject *output_stream_close(OutputStreamObject *self, PyObject *Py_UNUSED(a))
{
    if (self->owned) {
        wasi_io_streams_output_stream_drop_own(self->handle);
        self->owned = false;
    }
    Py_RETURN_NONE;
}

static PyObject *output_stream_get_closed(OutputStreamObject *self, void *Py_UNUSED(c))
{
    return PyBool_FromLong(!self->owned);
}

static PyObject *output_stream_readable(OutputStreamObject *Py_UNUSED(self),
                                        PyObject *Py_UNUSED(a))
{ Py_RETURN_FALSE; }

static PyObject *output_stream_writable(OutputStreamObject *Py_UNUSED(self),
                                        PyObject *Py_UNUSED(a))
{ Py_RETURN_TRUE; }

static PyObject *output_stream_seekable(OutputStreamObject *Py_UNUSED(self),
                                        PyObject *Py_UNUSED(a))
{ Py_RETURN_FALSE; }

static PyObject *output_stream_fileno(OutputStreamObject *Py_UNUSED(self),
                                      PyObject *Py_UNUSED(a))
{
    PyErr_SetString(PyExc_OSError, "stream has no host file descriptor");
    return NULL;
}

static PyObject *output_stream_write(OutputStreamObject *self, PyObject *args)
{
    Py_buffer view;
    if (!PyArg_ParseTuple(args, "y*:write", &view)) {
        return NULL;
    }
    if (!self->owned) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "I/O operation on closed stream");
        return NULL;
    }
    /* The WIT consumes its input list. Duplicate into a heap allocation we
     * hand off; mirrors bytes_to_list_u8 in deflate_raw above. */
    v86_posix_import_list_u8_t contents;
    contents.ptr = NULL;
    contents.len = (size_t) view.len;
    if (view.len > 0) {
        contents.ptr = (uint8_t *) malloc((size_t) view.len);
        if (contents.ptr == NULL) {
            PyBuffer_Release(&view);
            PyErr_NoMemory();
            return NULL;
        }
        memcpy(contents.ptr, view.buf, (size_t) view.len);
    }
    Py_ssize_t n = view.len;
    PyBuffer_Release(&view);

    wasi_io_streams_borrow_output_stream_t borrow =
        wasi_io_streams_borrow_output_stream(self->handle);
    wasi_io_streams_stream_error_t err;
    bool ok;
    Py_BEGIN_ALLOW_THREADS
    ok = wasi_io_streams_method_output_stream_blocking_write_and_flush(
        borrow, &contents, &err);
    Py_END_ALLOW_THREADS
    if (!ok) {
        if (err.tag == WASI_IO_STREAMS_STREAM_ERROR_CLOSED) {
            wasi_io_streams_stream_error_free(&err);
            PyErr_SetString(PyExc_BrokenPipeError, "write to closed stream");
            return NULL;
        }
        raise_stream_error(&err, "write");
        return NULL;
    }
    /* blocking_write_and_flush guarantees the full input was written. */
    return PyLong_FromSsize_t(n);
}

static PyObject *output_stream_flush(OutputStreamObject *self, PyObject *Py_UNUSED(a))
{
    if (!self->owned) {
        PyErr_SetString(PyExc_ValueError, "I/O operation on closed stream");
        return NULL;
    }
    wasi_io_streams_borrow_output_stream_t borrow =
        wasi_io_streams_borrow_output_stream(self->handle);
    wasi_io_streams_stream_error_t err;
    bool ok;
    Py_BEGIN_ALLOW_THREADS
    ok = wasi_io_streams_method_output_stream_blocking_flush(borrow, &err);
    Py_END_ALLOW_THREADS
    if (!ok) {
        if (err.tag == WASI_IO_STREAMS_STREAM_ERROR_CLOSED) {
            wasi_io_streams_stream_error_free(&err);
            Py_RETURN_NONE;  /* flush on closed is a no-op */
        }
        raise_stream_error(&err, "flush");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *output_stream_enter(OutputStreamObject *self, PyObject *Py_UNUSED(a))
{
    Py_INCREF(self);
    return (PyObject *) self;
}

static PyObject *output_stream_exit(OutputStreamObject *self, PyObject *Py_UNUSED(a))
{
    if (self->owned) {
        wasi_io_streams_output_stream_drop_own(self->handle);
        self->owned = false;
    }
    Py_RETURN_NONE;
}

static PyMethodDef output_stream_methods[] = {
    {"write",    (PyCFunction) output_stream_write,    METH_VARARGS,
     "write(bytes-like) -> int — blocking write+flush; returns bytes written."},
    {"flush",    (PyCFunction) output_stream_flush,    METH_NOARGS,
     "Force-flush any buffered data."},
    {"close",    (PyCFunction) output_stream_close,    METH_NOARGS,
     "Release the underlying wasi:io stream handle."},
    {"readable", (PyCFunction) output_stream_readable, METH_NOARGS, "False."},
    {"writable", (PyCFunction) output_stream_writable, METH_NOARGS, "True."},
    {"seekable", (PyCFunction) output_stream_seekable, METH_NOARGS, "False."},
    {"fileno",   (PyCFunction) output_stream_fileno,   METH_NOARGS,
     "Always raises — wasi:io streams are not fd-backed."},
    {"__enter__", (PyCFunction) output_stream_enter,   METH_NOARGS, NULL},
    {"__exit__",  (PyCFunction) output_stream_exit,    METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL}
};

static PyGetSetDef output_stream_getset[] = {
    {"closed", (getter) output_stream_get_closed, NULL,
     "True once close() has been called.", NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

static PyTypeObject OutputStreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_v86_posix.OutputStream",
    .tp_basicsize = sizeof(OutputStreamObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor) output_stream_dealloc,
    .tp_methods = output_stream_methods,
    .tp_getset = output_stream_getset,
    .tp_doc = "Python file-like wrapper for a wasi:io/streams output-stream.\n\n"
              "Supports write / flush / context-manager / close. Not seekable.\n"
              "Returned from Process.take_stdin() when stdio was spawned\n"
              "with PIPE. write() uses blocking-write-and-flush, so the call\n"
              "returns once the bytes are flushed to the underlying sink.",
};

static PyMethodDef process_methods[] = {
    {"pid",         (PyCFunction) process_pid,         METH_NOARGS,
     "Return the PID of this process inside the guest."},
    {"signal",      (PyCFunction) process_signal,      METH_VARARGS,
     "signal(signum: int) -> None\n\n"
     "Send a POSIX signal (1=HUP, 9=KILL, 15=TERM, …) to the process.\n"
     "Raises NoSuchProcessError if the process has already exited, or\n"
     "InvalidSignalError if the guest rejected the signal."},
    {"wait",        (PyCFunction) process_wait,        METH_NOARGS,
     "wait() -> tuple[str, int]\n\n"
     "Block until the process exits, returning either\n"
     "  (\"exited\",   code)  — process called exit() / returned from main\n"
     "  (\"signaled\", sig)   — process was terminated by signal `sig`\n"
     "Idempotent: subsequent calls return the same status without re-blocking."},
    {"try_wait",    (PyCFunction) process_try_wait,    METH_NOARGS,
     "try_wait() -> tuple[str, int] | None\n\n"
     "Non-blocking variant of `wait`. Returns None if still running."},
    {"take_stdin",  (PyCFunction) process_take_stdin,  METH_NOARGS,
     "take_stdin() -> OutputStream | None\n\n"
     "Take ownership of the child's stdin write-end. Returns None if\n"
     "spawn-options.stdin was not PIPED, or if a prior call already\n"
     "took ownership of the stream."},
    {"take_stdout", (PyCFunction) process_take_stdout, METH_NOARGS,
     "take_stdout() -> InputStream | None\n\n"
     "Take ownership of the child's stdout read-end. None if not PIPED."},
    {"take_stderr", (PyCFunction) process_take_stderr, METH_NOARGS,
     "take_stderr() -> InputStream | None\n\n"
     "Take ownership of the child's stderr read-end. None if not PIPED."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject ProcessType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_v86_posix.Process",
    .tp_basicsize = sizeof(ProcessObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor) process_dealloc,
    .tp_methods = process_methods,
    .tp_doc = "Handle to a process spawned inside the v86 guest.\n\n"
              "Drop the handle to detach (does NOT terminate the process —\n"
              "use signal(15) + wait() or signal(9) for an orderly shutdown).",
};

/* ===================================================================
 * Module-level: spawn()
 * =================================================================== */

/* Translate a Python int into a stdio-spec, validating range. */
static int parse_stdio(int v, v86_posix_process_stdio_spec_t *out, const char *which)
{
    if (v != V86_POSIX_PROCESS_STDIO_SPEC_INHERIT &&
        v != V86_POSIX_PROCESS_STDIO_SPEC_PIPED &&
        v != V86_POSIX_PROCESS_STDIO_SPEC_NULL) {
        PyErr_Format(PyExc_ValueError,
                     "%s must be STDIO_INHERIT, STDIO_PIPED, or STDIO_NULL",
                     which);
        return -1;
    }
    out->tag = (uint8_t) v;
    return 0;
}

/* Convert a Python sequence-of-str into a wit list<string>. Caller-owned;
 * free with v86_posix_import_list_string_t cleanup.
 *
 * A bare str is rejected up front: Python strings *are* sequences (each
 * char is a 1-char str), so without this check `spawn(prog, 'abc')` would
 * silently expand to args=['a', 'b', 'c'] — almost never what the caller
 * wants, and subprocess.Popen rejects the same shape with TypeError.
 * Mirror that contract here. */
static int seq_to_list_string(PyObject *seq, v86_posix_import_list_string_t *out)
{
    if (PyUnicode_Check(seq) || PyBytes_Check(seq)) {
        PyErr_SetString(PyExc_TypeError,
                        "args must be a sequence of strings, not a single string "
                        "(pass [\"arg1\", \"arg2\", …] not \"arg1 arg2 …\")");
        return -1;
    }
    PyObject *fast = PySequence_Fast(seq, "expected a sequence");
    if (fast == NULL) {
        return -1;
    }
    Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    out->ptr = NULL;
    out->len = 0;
    if (n > 0) {
        out->ptr = (v86_posix_import_string_t *)
            calloc((size_t) n, sizeof(v86_posix_import_string_t));
        if (out->ptr == NULL) {
            Py_DECREF(fast);
            PyErr_NoMemory();
            return -1;
        }
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(fast, i);  /* borrowed */
        if (!PyUnicode_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "args must be strings");
            goto fail;
        }
        if (pystr_to_wit(item, &out->ptr[i]) < 0) {
            goto fail;
        }
        out->len = (size_t) (i + 1);
    }
    Py_DECREF(fast);
    return 0;
fail:
    for (size_t i = 0; i < out->len; i++) {
        wit_str_free_local(&out->ptr[i]);
    }
    free(out->ptr);
    out->ptr = NULL;
    out->len = 0;
    Py_DECREF(fast);
    return -1;
}

/* Convert a Python sequence-of-(str, str) into a wit list<tuple<string,string>>. */
static int seq_to_list_env(PyObject *seq, v86_posix_import_list_tuple2_string_string_t *out)
{
    PyObject *fast = PySequence_Fast(seq, "env must be a sequence of (name, value)");
    if (fast == NULL) {
        return -1;
    }
    Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    out->ptr = NULL;
    out->len = 0;
    if (n > 0) {
        out->ptr = (v86_posix_import_tuple2_string_string_t *)
            calloc((size_t) n, sizeof(v86_posix_import_tuple2_string_string_t));
        if (out->ptr == NULL) {
            Py_DECREF(fast);
            PyErr_NoMemory();
            return -1;
        }
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(fast, i);
        PyObject *k = NULL, *v = NULL;
        if (!PyArg_ParseTuple(item, "OO", &k, &v)) {
            PyErr_SetString(PyExc_TypeError,
                            "env entries must be (name, value) pairs");
            goto fail;
        }
        if (!PyUnicode_Check(k) || !PyUnicode_Check(v)) {
            PyErr_SetString(PyExc_TypeError,
                            "env name and value must be strings");
            goto fail;
        }
        if (pystr_to_wit(k, &out->ptr[i].f0) < 0) goto fail;
        if (pystr_to_wit(v, &out->ptr[i].f1) < 0) {
            wit_str_free_local(&out->ptr[i].f0);
            goto fail;
        }
        out->len = (size_t) (i + 1);
    }
    Py_DECREF(fast);
    return 0;
fail:
    for (size_t i = 0; i < out->len; i++) {
        wit_str_free_local(&out->ptr[i].f0);
        wit_str_free_local(&out->ptr[i].f1);
    }
    free(out->ptr);
    out->ptr = NULL;
    out->len = 0;
    Py_DECREF(fast);
    return -1;
}

static PyObject *v86_posix_spawn(PyObject *Py_UNUSED(self),
                                 PyObject *args, PyObject *kw)
{
    static char *kwlist[] = {
        "program", "args", "env", "cwd",
        "stdin", "stdout", "stderr", NULL
    };
    PyObject *program = NULL;
    PyObject *argv = NULL;
    PyObject *env = Py_None;
    PyObject *cwd = Py_None;
    int stdin_kind  = V86_POSIX_PROCESS_STDIO_SPEC_INHERIT;
    int stdout_kind = V86_POSIX_PROCESS_STDIO_SPEC_INHERIT;
    int stderr_kind = V86_POSIX_PROCESS_STDIO_SPEC_INHERIT;

    if (!PyArg_ParseTupleAndKeywords(args, kw,
                                     "U|OOOiii:spawn", kwlist,
                                     &program, &argv, &env, &cwd,
                                     &stdin_kind, &stdout_kind, &stderr_kind)) {
        return NULL;
    }

    v86_posix_process_spawn_options_t opts;
    memset(&opts, 0, sizeof(opts));

    /* program */
    if (pystr_to_wit(program, &opts.program) < 0) return NULL;

    /* args (default: empty sequence) */
    if (argv == NULL) {
        opts.args.ptr = NULL;
        opts.args.len = 0;
    } else if (seq_to_list_string(argv, &opts.args) < 0) {
        goto fail;
    }

    /* env (None = inherit; sequence of (k, v)) */
    if (env == Py_None) {
        opts.env.ptr = NULL;
        opts.env.len = 0;
    } else if (seq_to_list_env(env, &opts.env) < 0) {
        goto fail;
    }

    /* cwd (None = inherit) */
    if (cwd == Py_None) {
        opts.cwd.is_some = false;
        memset(&opts.cwd.val, 0, sizeof(opts.cwd.val));
    } else if (PyUnicode_Check(cwd)) {
        opts.cwd.is_some = true;
        if (pystr_to_wit(cwd, &opts.cwd.val) < 0) goto fail;
    } else {
        PyErr_SetString(PyExc_TypeError, "cwd must be a string or None");
        goto fail;
    }

    /* stdio */
    if (parse_stdio(stdin_kind,  &opts.stdin_,  "stdin")  < 0) goto fail;
    if (parse_stdio(stdout_kind, &opts.stdout_, "stdout") < 0) goto fail;
    if (parse_stdio(stderr_kind, &opts.stderr_, "stderr") < 0) goto fail;

    /* Call. Ownership of `opts` and its sub-allocations is transferred to the
     * callee per canonical-ABI rules (we do NOT free what we passed). */
    v86_posix_process_own_process_t handle;
    v86_posix_process_spawn_error_t err;
    bool ok;
    Py_BEGIN_ALLOW_THREADS
    ok = v86_posix_process_spawn(&opts, &handle, &err);
    Py_END_ALLOW_THREADS

    if (!ok) {
        raise_spawn_error(&err);
        return NULL;
    }
    return process_new(handle);

fail:
    /* Release whatever sub-allocations we made before the WIT call. The
     * spawn-options free() helper walks all fields; safe even on partial
     * fill since calloc()'d above. */
    v86_posix_process_spawn_options_free(&opts);
    return NULL;
}

/* ===================================================================
 * Module plumbing
 * =================================================================== */

static PyMethodDef module_methods[] = {
    {"spawn", (PyCFunction) v86_posix_spawn, METH_VARARGS | METH_KEYWORDS,
     "spawn(program, args=(), env=None, cwd=None,\n"
     "      stdin=STDIO_INHERIT, stdout=STDIO_INHERIT, stderr=STDIO_INHERIT) -> Process\n\n"
     "Spawn a process inside the v86 guest. See the module docstring for the\n"
     "full contract + the error taxonomy (GuestNotReadyError, ProgramNotFoundError, …)."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef v86_posix_module = {
    PyModuleDef_HEAD_INIT,
    "_v86_posix",
    "CPython binding for the v86:posix/process WIT capability.\n\n"
    "Static extension into wasi-sdk CPython. The WIT imports appear as wasm\n"
    "imports on python-component.wasm; composectl/wac plug v86-component.wasm\n"
    "into them at compose time. Stdio stream wrapping is deferred (see\n"
    "Process.take_stdin's docstring).",
    -1,
    module_methods,
    NULL, NULL, NULL, NULL
};

/* Helper: create an exception subclass and add to module, save in `*out`. */
static int add_exc(PyObject *m, const char *name, PyObject *base, PyObject **out)
{
    PyObject *exc = PyErr_NewException(name, base, NULL);
    if (exc == NULL) {
        return -1;
    }
    /* PyModule_AddObject steals on success — INCREF first so we keep a ref. */
    Py_INCREF(exc);
    if (PyModule_AddObject(m, strrchr(name, '.') + 1, exc) < 0) {
        Py_DECREF(exc);
        Py_DECREF(exc);
        return -1;
    }
    *out = exc;
    return 0;
}

PyMODINIT_FUNC PyInit__v86_posix(void)
{
    if (PyType_Ready(&ProcessType) < 0) return NULL;
    if (PyType_Ready(&InputStreamType) < 0) return NULL;
    if (PyType_Ready(&OutputStreamType) < 0) return NULL;

    PyObject *m = PyModule_Create(&v86_posix_module);
    if (m == NULL) return NULL;

    /* Stdio constants */
    if (PyModule_AddIntConstant(m, "STDIO_INHERIT",
                                V86_POSIX_PROCESS_STDIO_SPEC_INHERIT) < 0) goto err;
    if (PyModule_AddIntConstant(m, "STDIO_PIPED",
                                V86_POSIX_PROCESS_STDIO_SPEC_PIPED) < 0) goto err;
    if (PyModule_AddIntConstant(m, "STDIO_NULL",
                                V86_POSIX_PROCESS_STDIO_SPEC_NULL) < 0) goto err;

    /* Process type */
    Py_INCREF(&ProcessType);
    if (PyModule_AddObject(m, "Process", (PyObject *) &ProcessType) < 0) {
        Py_DECREF(&ProcessType);
        goto err;
    }
    Py_INCREF(&InputStreamType);
    if (PyModule_AddObject(m, "InputStream", (PyObject *) &InputStreamType) < 0) {
        Py_DECREF(&InputStreamType);
        goto err;
    }
    Py_INCREF(&OutputStreamType);
    if (PyModule_AddObject(m, "OutputStream", (PyObject *) &OutputStreamType) < 0) {
        Py_DECREF(&OutputStreamType);
        goto err;
    }

    /* Exception hierarchy.
     *   SpawnError       (subclasses OSError, since semantics match Popen-side)
     *     ├─ ProgramNotFoundError
     *     ├─ ExecFailedError
     *     ├─ TooManyProcessesError
     *     ├─ InvalidArgumentError
     *     └─ GuestNotReadyError
     *   SignalError      (subclasses OSError)
     *     ├─ NoSuchProcessError
     *     └─ InvalidSignalError
     */
    if (add_exc(m, "_v86_posix.SpawnError",
                PyExc_OSError, &SpawnError_base) < 0) goto err;
    if (add_exc(m, "_v86_posix.ProgramNotFoundError",
                SpawnError_base, &ProgramNotFoundError) < 0) goto err;
    if (add_exc(m, "_v86_posix.ExecFailedError",
                SpawnError_base, &ExecFailedError) < 0) goto err;
    if (add_exc(m, "_v86_posix.TooManyProcessesError",
                SpawnError_base, &TooManyProcessesError) < 0) goto err;
    if (add_exc(m, "_v86_posix.InvalidArgumentError",
                SpawnError_base, &InvalidArgumentError) < 0) goto err;
    if (add_exc(m, "_v86_posix.GuestNotReadyError",
                SpawnError_base, &GuestNotReadyError) < 0) goto err;

    if (add_exc(m, "_v86_posix.SignalError",
                PyExc_OSError, &SignalError_base) < 0) goto err;
    if (add_exc(m, "_v86_posix.NoSuchProcessError",
                SignalError_base, &NoSuchProcessError) < 0) goto err;
    if (add_exc(m, "_v86_posix.InvalidSignalError",
                SignalError_base, &InvalidSignalError) < 0) goto err;

    return m;

err:
    Py_DECREF(m);
    return NULL;
}

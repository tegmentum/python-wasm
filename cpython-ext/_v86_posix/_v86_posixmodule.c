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

static PyObject *process_take_stdin(ProcessObject *Py_UNUSED(self),
                                    PyObject *Py_UNUSED(args))
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "stdin wrapping deferred — wasi:io/streams -> Python "
                    "file-like adapter is a cross-extension concern, see "
                    "docs/tier1-v86-integration.md");
    return NULL;
}

static PyObject *process_take_stdout(ProcessObject *Py_UNUSED(self),
                                     PyObject *Py_UNUSED(args))
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "stdout wrapping deferred — see take_stdin");
    return NULL;
}

static PyObject *process_take_stderr(ProcessObject *Py_UNUSED(self),
                                     PyObject *Py_UNUSED(args))
{
    PyErr_SetString(PyExc_NotImplementedError,
                    "stderr wrapping deferred — see take_stdin");
    return NULL;
}

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
     "DEFERRED. Raises NotImplementedError — wasi:io/streams Python\n"
     "wrapping is a separate concern; see module-level docstring."},
    {"take_stdout", (PyCFunction) process_take_stdout, METH_NOARGS,
     "DEFERRED. See take_stdin."},
    {"take_stderr", (PyCFunction) process_take_stderr, METH_NOARGS,
     "DEFERRED. See take_stdin."},
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

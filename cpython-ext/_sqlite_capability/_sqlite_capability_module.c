/* _sqlite_cap — Python C extension routing to the sqlite:wasm capability.
 *
 * Module name `_sqlite_cap` to avoid clash with stdlib's `_sqlite3` (which
 * isn't built on wasi-sdk anyway, but keeping the namespace separate makes
 * the boundary obvious). The Python-side shim at `Lib/sqlite3/__init__.py`
 * is what consumers actually import; this extension is its substrate.
 *
 * Statically linked into wasi-sdk CPython (see Modules/Setup.local). The
 * imported WIT functions appear as wasm imports on python-component.wasm,
 * satisfied at compose time by wac/composectl plugging sqlite-core.wasm.
 *
 * Design notes
 * ------------
 *
 * Resource handles (own_t / borrow_t over the canonical-ABI) are wrapped in
 * Python PyCapsules. The capsule destructor calls *_drop_own when Python
 * GCs the wrapper, so a leaked Connection still releases the cap-side
 * resource at interpreter shutdown. The handle int is stashed in the
 * capsule's pointer field via intptr_t round-trip — no malloc per handle.
 *
 * SELECT path uses `query-with-params` (eager: returns all rows at once)
 * rather than prepare/step iteration. This loses streaming for huge result
 * sets but keeps the C surface ~6 functions instead of ~15, and matches
 * what the typical fetchall() / iterator-over-cursor flow does anyway.
 * Streaming via statement-stepping is a follow-up.
 *
 * Python surface (consumed by Lib/sqlite3/__init__.py):
 *
 *   _sqlite_cap.connect(path: str, mode: int) -> capsule
 *     mode: 0=ro, 1=rw, 2=rwcreate, 3=memory
 *
 *   _sqlite_cap.execute(conn, sql: str, params: list) -> (changes, lastrowid)
 *   _sqlite_cap.query(conn, sql: str, params: list)   -> (col_names, rows)
 *     rows is list[list[Python value]]; Python values are int/float/str/bytes/None
 *
 *   _sqlite_cap.begin(conn), .commit(conn), .rollback(conn) -> None
 *   _sqlite_cap.in_autocommit(conn) -> bool
 *   _sqlite_cap.last_error(conn) -> None | (code, ext_code, msg)
 *
 *   _sqlite_cap.version()        -> str    (sqlite library version)
 *   _sqlite_cap.version_number() -> int
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "gen/sqlite_import.h"

typedef sqlite_wasm_high_level_own_connection_t conn_own_t;
typedef sqlite_wasm_high_level_borrow_connection_t conn_borrow_t;
typedef sqlite_wasm_high_level_value_t            value_t;
typedef sqlite_wasm_high_level_list_value_t       list_value_t;
typedef sqlite_wasm_high_level_database_error_t   db_err_t;
typedef sqlite_wasm_high_level_query_result_t     qry_t;
typedef sqlite_wasm_high_level_exec_result_t      exec_t;
typedef sqlite_wasm_high_level_row_t              row_t;

/* Custom Python exception type defined by the Python shim and registered
 * back into this module so we raise it for DB errors. Set by
 * _sqlite_cap._set_error_class(cls). */
static PyObject *DatabaseError = NULL;

/* String marshaling -------------------------------------------------------
 * sqlite_import_string_t carries (ptr, len) where the bytes are UTF-8 and
 * .ptr is malloc'd. The canonical ABI consumes the allocation on the
 * happy path; we duplicate from Python's PyUnicode_AsUTF8AndSize buffer. */

static int pystr_to_wit_string(PyObject *s, sqlite_import_string_t *out)
{
    Py_ssize_t n;
    const char *utf8 = PyUnicode_AsUTF8AndSize(s, &n);
    if (utf8 == NULL) return -1;
    out->ptr = (uint8_t *) malloc((size_t) n);
    if (out->ptr == NULL && n > 0) {
        PyErr_NoMemory();
        return -1;
    }
    if (n > 0) memcpy(out->ptr, utf8, (size_t) n);
    out->len = (size_t) n;
    return 0;
}

static PyObject *wit_string_to_pystr(const sqlite_import_string_t *s)
{
    return PyUnicode_FromStringAndSize((const char *) s->ptr, (Py_ssize_t) s->len);
}

/* Value variant marshaling ------------------------------------------------ */

/* Python object -> WIT value variant. Returns -1 on type error (with Python
 * error set) or non-marshalable input. */
static int py_to_value(PyObject *obj, value_t *out)
{
    if (obj == Py_None) {
        out->tag = SQLITE_WASM_HIGH_LEVEL_VALUE_NULL;
        return 0;
    }
    if (PyBool_Check(obj)) {
        /* sqlite stores booleans as 0/1 integer, matching stdlib behavior */
        out->tag = SQLITE_WASM_HIGH_LEVEL_VALUE_INTEGER;
        out->val.integer = (obj == Py_True) ? 1 : 0;
        return 0;
    }
    if (PyLong_Check(obj)) {
        long long v = PyLong_AsLongLong(obj);
        if (v == -1 && PyErr_Occurred()) return -1;
        out->tag = SQLITE_WASM_HIGH_LEVEL_VALUE_INTEGER;
        out->val.integer = (int64_t) v;
        return 0;
    }
    if (PyFloat_Check(obj)) {
        out->tag = SQLITE_WASM_HIGH_LEVEL_VALUE_REAL;
        out->val.real = PyFloat_AsDouble(obj);
        return 0;
    }
    if (PyUnicode_Check(obj)) {
        out->tag = SQLITE_WASM_HIGH_LEVEL_VALUE_TEXT;
        return pystr_to_wit_string(obj, &out->val.text);
    }
    if (PyBytes_Check(obj) || PyByteArray_Check(obj)) {
        Py_buffer view;
        if (PyObject_GetBuffer(obj, &view, PyBUF_SIMPLE | PyBUF_C_CONTIGUOUS) < 0) return -1;
        out->tag = SQLITE_WASM_HIGH_LEVEL_VALUE_BLOB;
        out->val.blob.ptr = (uint8_t *) malloc((size_t) view.len);
        if (out->val.blob.ptr == NULL && view.len > 0) {
            PyBuffer_Release(&view);
            PyErr_NoMemory();
            return -1;
        }
        if (view.len > 0) memcpy(out->val.blob.ptr, view.buf, (size_t) view.len);
        out->val.blob.len = (size_t) view.len;
        PyBuffer_Release(&view);
        return 0;
    }
    PyErr_Format(PyExc_TypeError,
                 "unsupported sqlite param type: %s",
                 Py_TYPE(obj)->tp_name);
    return -1;
}

/* WIT value variant -> Python object. The list_u8 / string buffers in the
 * source are freed by the caller (via the *_result_free helpers). */
static PyObject *value_to_py(const value_t *v)
{
    switch (v->tag) {
    case SQLITE_WASM_HIGH_LEVEL_VALUE_NULL:
        Py_RETURN_NONE;
    case SQLITE_WASM_HIGH_LEVEL_VALUE_INTEGER:
        return PyLong_FromLongLong((long long) v->val.integer);
    case SQLITE_WASM_HIGH_LEVEL_VALUE_REAL:
        return PyFloat_FromDouble(v->val.real);
    case SQLITE_WASM_HIGH_LEVEL_VALUE_TEXT:
        return PyUnicode_FromStringAndSize(
            (const char *) v->val.text.ptr, (Py_ssize_t) v->val.text.len);
    case SQLITE_WASM_HIGH_LEVEL_VALUE_BLOB:
        return PyBytes_FromStringAndSize(
            (const char *) v->val.blob.ptr, (Py_ssize_t) v->val.blob.len);
    default:
        PyErr_Format(PyExc_RuntimeError, "unknown value tag: %d", (int) v->tag);
        return NULL;
    }
}

/* Build a list<value> from a Python sequence of Python values. On error
 * frees any partially-built items and returns -1. */
static int pyseq_to_value_list(PyObject *seq, list_value_t *out)
{
    out->ptr = NULL;
    out->len = 0;
    if (seq == Py_None) return 0;
    PyObject *fast = PySequence_Fast(seq, "params must be a sequence");
    if (fast == NULL) return -1;
    Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    if (n == 0) {
        Py_DECREF(fast);
        return 0;
    }
    out->ptr = (value_t *) malloc((size_t) n * sizeof(value_t));
    if (out->ptr == NULL) {
        Py_DECREF(fast);
        PyErr_NoMemory();
        return -1;
    }
    out->len = (size_t) n;
    for (Py_ssize_t i = 0; i < n; i++) {
        if (py_to_value(PySequence_Fast_GET_ITEM(fast, i), &out->ptr[i]) < 0) {
            /* free already-allocated text/blob payloads */
            for (Py_ssize_t j = 0; j < i; j++) {
                if (out->ptr[j].tag == SQLITE_WASM_HIGH_LEVEL_VALUE_TEXT)
                    free(out->ptr[j].val.text.ptr);
                else if (out->ptr[j].tag == SQLITE_WASM_HIGH_LEVEL_VALUE_BLOB)
                    free(out->ptr[j].val.blob.ptr);
            }
            free(out->ptr);
            out->ptr = NULL;
            Py_DECREF(fast);
            return -1;
        }
    }
    Py_DECREF(fast);
    return 0;
}

/* Error handling ---------------------------------------------------------- */

/* Raise the registered DatabaseError (or RuntimeError if none) with the
 * message from a db_err_t. Frees the err's string buffer. Returns NULL. */
static PyObject *raise_db_err(db_err_t *err)
{
    PyObject *msg = wit_string_to_pystr(&err->message);
    sqlite_import_string_free(&err->message);
    PyObject *exc_cls = DatabaseError ? DatabaseError : PyExc_RuntimeError;
    /* Match sqlite3 stdlib args: (msg, code, ext_code) — the shim's
     * DatabaseError.__init__ accepts these as positional. */
    PyObject *args = Py_BuildValue("(Oii)",
                                   msg ? msg : Py_None,
                                   (int) err->code,
                                   (int) err->extended_code);
    Py_XDECREF(msg);
    if (args) {
        PyErr_SetObject(exc_cls, args);
        Py_DECREF(args);
    } else {
        PyErr_SetString(exc_cls, "sqlite error (unknown details)");
    }
    return NULL;
}

/* Capsule helpers --------------------------------------------------------- */

#define CONN_CAPSULE_NAME "_sqlite_cap.connection"

static void conn_capsule_destructor(PyObject *cap)
{
    int32_t handle = (int32_t)(intptr_t) PyCapsule_GetPointer(cap, CONN_CAPSULE_NAME);
    if (handle != 0) {
        conn_own_t own = {handle};
        sqlite_wasm_high_level_connection_drop_own(own);
    }
}

static int extract_conn_borrow(PyObject *cap_obj, conn_borrow_t *out)
{
    int32_t handle = (int32_t)(intptr_t)
        PyCapsule_GetPointer(cap_obj, CONN_CAPSULE_NAME);
    if (handle == 0) {
        /* Either the pointer is genuinely 0, or the capsule was renamed
         * (cap_close renames to ".closed") so the name check failed and
         * an exception is set. Replace with a uniform "closed" message. */
        PyErr_Clear();
        PyErr_SetString(PyExc_ValueError, "sqlite connection is closed");
        return -1;
    }
    /* Build a borrow from the own handle. */
    conn_own_t pseudo_own = {handle};
    *out = sqlite_wasm_high_level_borrow_connection(pseudo_own);
    return 0;
}

/* Module functions -------------------------------------------------------- */

static PyObject *cap_connect(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"path", "mode", NULL};
    PyObject *path_obj;
    int mode;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "Oi:connect", kwl,
                                     &path_obj, &mode)) {
        return NULL;
    }
    sqlite_import_string_t path;
    if (pystr_to_wit_string(path_obj, &path) < 0) return NULL;
    /* The constructor returns own handle on success; on cap-side failure
     * the resource is null and subsequent ops will fail. wit-bindgen-c
     * doesn't surface the constructor's error here — best-effort. */
    conn_own_t own = sqlite_wasm_high_level_constructor_connection(
        &path, (sqlite_wasm_high_level_open_mode_t) mode);
    if (own.__handle == 0) {
        PyErr_SetString(DatabaseError ? DatabaseError : PyExc_RuntimeError,
                        "failed to open sqlite database");
        return NULL;
    }
    return PyCapsule_New((void *)(intptr_t) own.__handle,
                         CONN_CAPSULE_NAME,
                         conn_capsule_destructor);
}

static PyObject *cap_close(PyObject *self, PyObject *args)
{
    PyObject *cap_obj;
    if (!PyArg_ParseTuple(args, "O:close", &cap_obj)) return NULL;
    int32_t handle = (int32_t)(intptr_t)
        PyCapsule_GetPointer(cap_obj, CONN_CAPSULE_NAME);
    if (handle == 0 && PyErr_Occurred()) PyErr_Clear();  /* already closed */
    if (handle != 0) {
        conn_own_t own = {handle};
        sqlite_wasm_high_level_connection_drop_own(own);
        /* Mark the capsule as closed. We can't use PyCapsule_SetPointer(NULL)
         * — it rejects NULL silently — so we rename the capsule (which makes
         * subsequent extract_conn_borrow's name check fail) AND clear the
         * destructor so it won't try to drop_own a second time when the
         * capsule itself is GC'd. */
        PyCapsule_SetDestructor(cap_obj, NULL);
        PyCapsule_SetName(cap_obj, CONN_CAPSULE_NAME ".closed");
    }
    Py_RETURN_NONE;
}

static PyObject *cap_execute(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"conn", "sql", "params", NULL};
    PyObject *cap_obj;
    PyObject *sql_obj;
    PyObject *params_obj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|O:execute", kwl,
                                     &cap_obj, &sql_obj, &params_obj)) {
        return NULL;
    }
    conn_borrow_t borrow;
    if (extract_conn_borrow(cap_obj, &borrow) < 0) return NULL;
    sqlite_import_string_t sql;
    if (pystr_to_wit_string(sql_obj, &sql) < 0) return NULL;
    list_value_t params;
    if (pyseq_to_value_list(params_obj, &params) < 0) {
        free(sql.ptr);
        return NULL;
    }
    exec_t result;
    db_err_t err;
    bool ok;
    if (params.len == 0) {
        ok = sqlite_wasm_high_level_method_connection_execute(
            borrow, &sql, &result, &err);
    } else {
        ok = sqlite_wasm_high_level_method_connection_execute_with_params(
            borrow, &sql, &params, &result, &err);
    }
    if (!ok) return raise_db_err(&err);
    return Py_BuildValue("(iL)", (int) result.changes,
                         (long long) result.last_insert_rowid);
}

static PyObject *cap_query(PyObject *self, PyObject *args, PyObject *kw)
{
    static char *kwl[] = {"conn", "sql", "params", NULL};
    PyObject *cap_obj;
    PyObject *sql_obj;
    PyObject *params_obj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kw, "OO|O:query", kwl,
                                     &cap_obj, &sql_obj, &params_obj)) {
        return NULL;
    }
    conn_borrow_t borrow;
    if (extract_conn_borrow(cap_obj, &borrow) < 0) return NULL;
    sqlite_import_string_t sql;
    if (pystr_to_wit_string(sql_obj, &sql) < 0) return NULL;
    list_value_t params;
    if (pyseq_to_value_list(params_obj, &params) < 0) {
        free(sql.ptr);
        return NULL;
    }
    qry_t result;
    db_err_t err;
    bool ok;
    if (params.len == 0) {
        ok = sqlite_wasm_high_level_method_connection_query(
            borrow, &sql, &result, &err);
    } else {
        ok = sqlite_wasm_high_level_method_connection_query_with_params(
            borrow, &sql, &params, &result, &err);
    }
    if (!ok) return raise_db_err(&err);

    /* Convert column_names to a Python tuple */
    PyObject *cols = PyTuple_New((Py_ssize_t) result.column_names.len);
    if (cols == NULL) {
        sqlite_wasm_high_level_query_result_free(&result);
        return NULL;
    }
    for (size_t i = 0; i < result.column_names.len; i++) {
        PyObject *name = wit_string_to_pystr(&result.column_names.ptr[i]);
        if (name == NULL) {
            Py_DECREF(cols);
            sqlite_wasm_high_level_query_result_free(&result);
            return NULL;
        }
        PyTuple_SET_ITEM(cols, (Py_ssize_t) i, name);
    }
    /* Convert rows to list of tuples */
    PyObject *rows = PyList_New((Py_ssize_t) result.rows.len);
    if (rows == NULL) {
        Py_DECREF(cols);
        sqlite_wasm_high_level_query_result_free(&result);
        return NULL;
    }
    for (size_t r = 0; r < result.rows.len; r++) {
        const row_t *src = &result.rows.ptr[r];
        PyObject *row_tup = PyTuple_New((Py_ssize_t) src->columns.len);
        if (row_tup == NULL) {
            Py_DECREF(cols);
            Py_DECREF(rows);
            sqlite_wasm_high_level_query_result_free(&result);
            return NULL;
        }
        for (size_t c = 0; c < src->columns.len; c++) {
            PyObject *val = value_to_py(&src->columns.ptr[c]);
            if (val == NULL) {
                Py_DECREF(row_tup);
                Py_DECREF(cols);
                Py_DECREF(rows);
                sqlite_wasm_high_level_query_result_free(&result);
                return NULL;
            }
            PyTuple_SET_ITEM(row_tup, (Py_ssize_t) c, val);
        }
        PyList_SET_ITEM(rows, (Py_ssize_t) r, row_tup);
    }
    sqlite_wasm_high_level_query_result_free(&result);
    return Py_BuildValue("(OO)", cols, rows);
}

/* Transaction control + introspection ----------------------------------- */

#define DEF_TXN(name, wit_fn)                                                  \
static PyObject *cap_##name(PyObject *self, PyObject *args) {                  \
    PyObject *cap_obj;                                                         \
    if (!PyArg_ParseTuple(args, "O:" #name, &cap_obj)) return NULL;            \
    conn_borrow_t borrow;                                                      \
    if (extract_conn_borrow(cap_obj, &borrow) < 0) return NULL;                \
    db_err_t err;                                                              \
    if (!wit_fn(borrow, &err)) return raise_db_err(&err);                      \
    Py_RETURN_NONE;                                                            \
}
DEF_TXN(begin,    sqlite_wasm_high_level_method_connection_begin_transaction)
DEF_TXN(commit,   sqlite_wasm_high_level_method_connection_commit)
DEF_TXN(rollback, sqlite_wasm_high_level_method_connection_rollback)
#undef DEF_TXN

static PyObject *cap_in_autocommit(PyObject *self, PyObject *args)
{
    PyObject *cap_obj;
    if (!PyArg_ParseTuple(args, "O:in_autocommit", &cap_obj)) return NULL;
    conn_borrow_t borrow;
    if (extract_conn_borrow(cap_obj, &borrow) < 0) return NULL;
    bool r = sqlite_wasm_high_level_method_connection_in_autocommit(borrow);
    if (r) Py_RETURN_TRUE; else Py_RETURN_FALSE;
}

static PyObject *cap_last_error(PyObject *self, PyObject *args)
{
    PyObject *cap_obj;
    if (!PyArg_ParseTuple(args, "O:last_error", &cap_obj)) return NULL;
    conn_borrow_t borrow;
    if (extract_conn_borrow(cap_obj, &borrow) < 0) return NULL;
    db_err_t err;
    bool has = sqlite_wasm_high_level_method_connection_last_error(borrow, &err);
    if (!has) Py_RETURN_NONE;
    PyObject *msg = wit_string_to_pystr(&err.message);
    sqlite_import_string_free(&err.message);
    PyObject *r = Py_BuildValue("(iiO)", (int) err.code, (int) err.extended_code,
                                msg ? msg : Py_None);
    Py_XDECREF(msg);
    return r;
}

/* Module-level utilities ------------------------------------------------- */

static PyObject *cap_version(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    sqlite_import_string_t v;
    sqlite_wasm_high_level_version(&v);
    PyObject *r = wit_string_to_pystr(&v);
    sqlite_import_string_free(&v);
    return r;
}

static PyObject *cap_version_number(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    int32_t n = sqlite_wasm_high_level_version_number();
    return PyLong_FromLong((long) n);
}

/* Called once by the Python shim during module init to register the
 * exception class to raise on db errors. */
static PyObject *cap_set_error_class(PyObject *self, PyObject *cls)
{
    Py_XDECREF(DatabaseError);
    Py_INCREF(cls);
    DatabaseError = cls;
    Py_RETURN_NONE;
}

/* Module plumbing -------------------------------------------------------- */

static PyMethodDef methods[] = {
    {"connect",        (PyCFunction) cap_connect,        METH_VARARGS | METH_KEYWORDS,
     "connect(path: str, mode: int) -> capsule"},
    {"close",          cap_close,                        METH_VARARGS,
     "close(conn) -> None  (idempotent)"},
    {"execute",        (PyCFunction) cap_execute,        METH_VARARGS | METH_KEYWORDS,
     "execute(conn, sql, params=None) -> (changes, lastrowid)"},
    {"query",          (PyCFunction) cap_query,          METH_VARARGS | METH_KEYWORDS,
     "query(conn, sql, params=None) -> (column_names, rows)"},
    {"begin",          cap_begin,                        METH_VARARGS,
     "begin(conn) -> None"},
    {"commit",         cap_commit,                       METH_VARARGS,
     "commit(conn) -> None"},
    {"rollback",       cap_rollback,                     METH_VARARGS,
     "rollback(conn) -> None"},
    {"in_autocommit",  cap_in_autocommit,                METH_VARARGS,
     "in_autocommit(conn) -> bool"},
    {"last_error",     cap_last_error,                   METH_VARARGS,
     "last_error(conn) -> None | (code, ext_code, msg)"},
    {"version",        cap_version,                      METH_NOARGS,
     "version() -> str   (sqlite library version)"},
    {"version_number", cap_version_number,               METH_NOARGS,
     "version_number() -> int"},
    {"_set_error_class", cap_set_error_class,            METH_O,
     "_set_error_class(cls) -> None  (called by Lib/sqlite3 shim init)"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef sqlite_cap_module = {
    PyModuleDef_HEAD_INIT,
    "_sqlite_cap",
    "Capability-routed sqlite3 bindings for python-wasm. Consumes the\n"
    "sqlite:wasm/high-level WIT interface, plugged at compose time by\n"
    "sqlite-core.wasm. The Python-side Lib/sqlite3/__init__.py shim\n"
    "translates this surface into the DB-API 2.0 / PEP 249 contract.",
    -1,
    methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__sqlite_cap(void)
{
    return PyModule_Create(&sqlite_cap_module);
}

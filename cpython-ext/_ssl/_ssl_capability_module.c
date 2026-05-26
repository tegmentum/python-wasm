/* _ssl_capability — Python C extension routing to the openssl-component
 * TLS capability over the Component Model. SCAFFOLD (Phase 3b.1).
 *
 * Statically linked into wasi-sdk CPython (Modules/Setup.local). The imported
 * WIT functions appear as wasm imports on python-component.wasm, satisfied at
 * compose time by wac/composectl plugging openssl-component.wasm.
 *
 * Scope of this scaffold:
 *   - Module exists, importable: `import _ssl_capability`.
 *   - One smoke function `_ssl_capability.probe_imports()` touches the
 *     openssl:component/x509 import so the resulting python.wasm declares
 *     the dependency.
 *
 * The real Python surface (`MemoryBIO`, `_SSLContext`, `_SSLSocket`,
 * `RAND_bytes`, `OPENSSL_VERSION`, error classes, etc.) lands in Phase 3b.2
 * through 3b.6. Naming is `_ssl_capability` (not `_ssl`) so the existing
 * static OpenSSL `_ssl` module keeps working unchanged during the bring-up;
 * Phase 5 retires the static path once `_ssl.py` re-routes to this module.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "gen/ssl_import.h"

/* probe_imports — touches the openssl:component/x509 import so the linker
 * keeps the import alive in the resulting wasm component. Future Phase 3b
 * functions will replace this with real work (TLS connect, etc.). */
static PyObject *probe_imports(PyObject *self, PyObject *Py_UNUSED(args))
{
    /* Construct an empty trust store and immediately drop it. This is the
     * smallest call that proves the openssl-component imports are wired:
     * if the wasm component doesn't declare openssl:component/x509 as an
     * import, link fails. */
    openssl_component_x509_own_store_t store =
        openssl_component_x509_constructor_store();
    openssl_component_x509_store_drop_own(store);
    Py_RETURN_NONE;
}

static PyMethodDef module_methods[] = {
    {"probe_imports", probe_imports, METH_NOARGS,
     "Scaffold smoke (Phase 3b.1): create and drop an x509 store. Confirms\n"
     "the openssl-component imports are wired into python.wasm. Will be\n"
     "removed when real Phase 3b methods land."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef ssl_capability_module = {
    PyModuleDef_HEAD_INIT,
    "_ssl_capability",
    "CPython binding for openssl:component/tls (Phase 3b scaffold).\n"
    "Composed in at component-build time via wac plug openssl-component.wasm.\n"
    "Not yet a drop-in for the static _ssl extension; see Phase 3 plan in\n"
    "docs/phase-3-tls.md.",
    -1,
    module_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__ssl_capability(void)
{
    return PyModule_Create(&ssl_capability_module);
}

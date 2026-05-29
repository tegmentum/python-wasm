"""
WASI compatibility shims applied at interpreter startup via site.py.

python-wasm freezes Lib/os.py into the binary, so build-time patches to that
file are shadowed at runtime. sitecustomize runs after the frozen import, so
it's the right place to attach user-id stubs that pure-Python deps (pip's
bundled platformdirs, setuptools, etc.) read at their own import time.

WASI Preview 2 has no user/group model; we report a single-user
container-style identity: uid/gid 0, ppid 1, login "wasi".
"""
import sys
import os

if not hasattr(os, "getuid"):
    os.getuid    = lambda: 0
    os.geteuid   = lambda: 0
    os.getgid    = lambda: 0
    os.getegid   = lambda: 0
    os.getppid   = lambda: 1
    os.getlogin  = lambda: "wasi"

# umask(): WASI Preview 2 has no umask concept (files have effectively the
# permission policy of the host's --dir mount). Stub to return 0 and ignore
# sets — pip uses it to mask file modes during wheel install.
if not hasattr(os, "umask"):
    _current_umask = 0
    def _umask(mask):
        global _current_umask
        prev = _current_umask
        _current_umask = mask
        return prev
    os.umask = _umask

# pip and other deps probe `import _ssl` to detect TLS support. We ship
# `_ssl_capability` instead (cap-routed through openssl-component). Alias
# the legacy name so the probe succeeds without surprising the caller.
if "_ssl" not in sys.modules:
    try:
        import _ssl_capability as _ssl
        sys.modules["_ssl"] = _ssl
    except ImportError:
        pass

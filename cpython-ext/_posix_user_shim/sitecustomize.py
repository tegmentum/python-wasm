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


# Phase 4: py-offload importhook auto-install. When OFFLOAD_MAILBOX_DIR and
# OFFLOAD_PACKAGES are set in the environment, route imports of those packages
# through the mailbox-transport offload boundary. See
# docs/coverage-implementation-plan.md and reference-worker/README.md.
try:
    import _offload_shim
    _offload_shim.install_from_env()
except ImportError:
    pass


# asyncio's BaseSelectorEventLoop._make_self_pipe wants socket.socketpair()
# to cross-signal a real selector wake-up. WASI Preview 2 has no socketpair
# primitive; Lib/socket.py's _fallback_socketpair (bind/listen/connect)
# fails on PermissionError before -S inherit-network and OSError after.
# In a single-threaded wasi-p2 guest we have neither signals nor concurrent
# threads, so the self-pipe is dead code: nothing ever calls _write_to_self.
# Replace the four methods that touch the pipe with no-ops so asyncio.run()
# can construct an event loop without trying to socketpair.
try:
    import asyncio.selector_events as _se

    def _no_self_pipe(self):
        self._ssock = None
        self._csock = None
        self._internal_fds = 0

    _se.BaseSelectorEventLoop._make_self_pipe   = _no_self_pipe
    _se.BaseSelectorEventLoop._close_self_pipe  = lambda self: None
    _se.BaseSelectorEventLoop._process_self_data = lambda self, data: None
    _se.BaseSelectorEventLoop._write_to_self    = lambda self: None
    del _se
except ImportError:
    pass

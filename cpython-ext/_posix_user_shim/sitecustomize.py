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

# chown(): WASI Preview 2 has no user/group model. tarfile's extract path
# calls os.chown when restoring archive owner/group metadata. Stub as a
# no-op so sdist extraction works.
if not hasattr(os, "chown"):
    os.chown = lambda *a, **kw: None
if not hasattr(os, "lchown"):
    os.lchown = lambda *a, **kw: None

# pip and other deps probe `import _ssl` to detect TLS support. We ship
# `_ssl_capability` instead (cap-routed through openssl-component). Alias
# the legacy name so the probe succeeds without surprising the caller.
if "_ssl" not in sys.modules:
    try:
        import _ssl_capability as _ssl
        sys.modules["_ssl"] = _ssl
    except ImportError:
        pass


# httpcore checks connection liveness via select.poll on the socket's
# fileno(). Across the wasm component boundary, an fd from
# openssl-component doesn't map to anything Python's select can poll
# (separate fd tables per component). Replace is_socket_readable with a
# wrapper that uses our cap-routed SSLSocket.socket_readable() when the
# socket is one of ours, and falls back to the real poll otherwise.
# Deferred until httpcore._utils is actually imported — sitecustomize
# runs before user code can pip-install httpcore.
def _install_httpcore_patch():
    import importlib.abc
    import importlib.machinery
    import sys

    class _Loader(importlib.abc.Loader):
        def __init__(self, real_loader):
            self.real_loader = real_loader
        def create_module(self, spec):
            return self.real_loader.create_module(spec)
        def exec_module(self, module):
            self.real_loader.exec_module(module)
            # Patch is_socket_readable to handle our SSLSocket.
            orig = getattr(module, "is_socket_readable", None)
            if orig is None:
                return
            def patched(sock):
                # `sock` may be an SSL-wrapped socket; check the inner
                # cap for socket_readable. If absent (not our SSL),
                # delegate to the original poll-based check.
                inner = getattr(sock, "_inner", None)
                if inner is not None and hasattr(inner, "socket_readable"):
                    try:
                        return inner.socket_readable()
                    except Exception:
                        pass
                return orig(sock)
            module.is_socket_readable = patched

    class _Finder(importlib.abc.MetaPathFinder):
        def find_spec(self, name, path, target=None):
            if name != "httpcore._utils":
                return None
            # Drop self to avoid recursion; let the real finder run.
            sys.meta_path.remove(self)
            try:
                spec = importlib.util.find_spec(name)
            finally:
                sys.meta_path.insert(0, self)
            if spec is None:
                return None
            spec.loader = _Loader(spec.loader)
            return spec

    import importlib.util  # noqa
    sys.meta_path.insert(0, _Finder())

try:
    _install_httpcore_patch()
except Exception:
    pass


# Same shape, different module: urllib3's HTTPConnection.is_connected
# calls util.wait.wait_for_read on the socket, which does
# select.poll().poll(fd). Our SSLSocket.fileno() returns the fd owned by
# openssl-component — meaningless across the component boundary, so
# poll raises EBADF. Patch wait_for_read to consult our cap's
# socket_readable() when the sock is one of ours.
def _install_urllib3_patch():
    import importlib.abc
    import importlib.util
    import sys

    class _Loader(importlib.abc.Loader):
        def __init__(self, real_loader):
            self.real_loader = real_loader
        def create_module(self, spec):
            return self.real_loader.create_module(spec)
        def exec_module(self, module):
            self.real_loader.exec_module(module)
            orig = getattr(module, "wait_for_read", None)
            if orig is None:
                return
            def patched(sock, timeout=None):
                inner = getattr(sock, "_inner", None)
                if inner is not None and hasattr(inner, "socket_readable"):
                    try:
                        return inner.socket_readable()
                    except Exception:
                        pass
                return orig(sock, timeout)
            module.wait_for_read = patched

    class _Finder(importlib.abc.MetaPathFinder):
        def find_spec(self, name, path, target=None):
            # Match both the top-level urllib3 (user-installed via pip)
            # AND pip's own bundled copy at pip._vendor.urllib3 — pip
            # routes all HTTP through the vendor path, so without the
            # vendor match, EBADF retries fire on every pip download.
            if name not in ("urllib3.util.wait",
                            "pip._vendor.urllib3.util.wait"):
                return None
            sys.meta_path.remove(self)
            try:
                spec = importlib.util.find_spec(name)
            finally:
                sys.meta_path.insert(0, self)
            if spec is None:
                return None
            spec.loader = _Loader(spec.loader)
            return spec

    sys.meta_path.insert(0, _Finder())

try:
    _install_urllib3_patch()
except Exception:
    pass


# Pip's `Installing collected packages: A, B, ...` step spawns rich's
# _TrackThread (a daemon thread that polls progress and updates a bar).
# Our threading shim runs Thread.start() inline → the thread's run-loop
# (which waits on an Event nobody else will set during this turn) blocks
# the main thread forever. The progress bar is purely cosmetic; force
# pip to bypass the threaded renderer when more than one package is
# being installed. Detected by importing pip._internal.cli.progress_bars
# lazily — only patches if pip is on sys.path.
def _patch_pip_progress():
    import importlib.util
    if importlib.util.find_spec("pip._internal.cli.progress_bars") is None:
        return
    import pip._internal.cli.progress_bars as _pb
    _orig = _pb.get_install_progress_renderer
    def _no_thread(*, bar_type, total):
        # Force `bar_type="off"` so the threaded rich progress is skipped
        # — the function falls back to a plain `iter` wrapper.
        return _orig(bar_type="off", total=total)
    _pb.get_install_progress_renderer = _no_thread
try:
    _patch_pip_progress()
except Exception:
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


# Phase 6: route multiprocessing.Pool through OffloadPool when
# OFFLOAD_POOL_DIR is set. multiprocessing.Pool needs os.fork in stock
# CPython — wasi-p2 doesn't have it, so the stdlib Pool.start_method
# fails. Inject OffloadPool as the Pool factory: same map/apply surface,
# real parallelism via N host-side worker processes.
try:
    import os
    if os.environ.get("OFFLOAD_POOL_DIR"):
        from _offload_shim import OffloadPool as _OffloadPool

        def _wasi_pool_factory(processes=None, initializer=None, initargs=(),
                               maxtasksperchild=None, context=None,
                               _root=os.environ["OFFLOAD_POOL_DIR"],
                               _default=int(os.environ.get("OFFLOAD_POOL_SIZE", "4")),
                               _Pool=_OffloadPool):
            """Pool() drop-in. processes defaults to OFFLOAD_POOL_SIZE."""
            if initializer is not None:
                raise NotImplementedError("OffloadPool: initializer not supported")
            return _Pool(processes or _default, mailbox_root=_root)

        import multiprocessing
        multiprocessing.Pool = _wasi_pool_factory
        try:
            from multiprocessing import pool as _mp_pool
            _mp_pool.Pool = _wasi_pool_factory
        except ImportError:
            pass
except (ImportError, KeyError, ValueError):
    pass


# asyncio.to_thread → BaseEventLoop.run_in_executor → ThreadPoolExecutor
# spawns a worker thread that immediately blocks on _queue.SimpleQueue.get(),
# which calls pthread_cond_wait — unimplemented in wasi-libc, traps with
# `unreachable`. Our threading shim runs Thread.start() inline so the worker
# never gets to run in parallel anyway. Replace run_in_executor with a
# synchronous shim: invoke the function immediately and return a pre-resolved
# concurrent.futures.Future. asyncio.wrap_future then bridges that to the
# event loop normally. No threads created, no pthread_cond_wait.
try:
    import asyncio.base_events as _be
    def _sync_run_in_executor(self, executor, func, *args):
        f = self.create_future()
        try:
            result = func(*args)
        except BaseException as exc:  # noqa: BLE001
            f.set_exception(exc)
        else:
            f.set_result(result)
        return f
    _be.BaseEventLoop.run_in_executor = _sync_run_in_executor
    del _be
except ImportError:
    pass


# anyio.to_thread.run_sync delegates to a WorkerThread (Thread subclass with
# overridden run()) that consumes from a queue. Our threading shim defers
# subclass-override Thread.run() to join() time so pip's rich progress bar
# doesn't deadlock the main thread. anyio doesn't join() the worker —
# it awaits a Future the worker is supposed to set — so the work never
# runs and the async caller hangs.
#
# Patch anyio.to_thread.run_sync to call func inline and return its result.
# Single-threaded execution = no parallelism gain, but no hang either.
# Deferred via a meta-path finder because anyio is an installed package
# that may not be on sys.path at startup.
def _install_anyio_to_thread_patch():
    import importlib.abc
    import importlib.machinery
    import importlib.util
    import sys

    class _Loader(importlib.abc.Loader):
        def __init__(self, real_loader):
            self.real_loader = real_loader
        def create_module(self, spec):
            return self.real_loader.create_module(spec)
        def exec_module(self, module):
            self.real_loader.exec_module(module)
            async def run_sync(func, *args, abandon_on_cancel=False,
                               cancellable=None, limiter=None):
                return func(*args)
            module.run_sync = run_sync

    class _Finder(importlib.abc.MetaPathFinder):
        def find_spec(self, name, path, target=None):
            if name != "anyio.to_thread":
                return None
            sys.meta_path.remove(self)
            try:
                spec = importlib.util.find_spec(name)
            finally:
                sys.meta_path.insert(0, self)
            if spec is None:
                return None
            spec.loader = _Loader(spec.loader)
            return spec

    sys.meta_path.insert(0, _Finder())

try:
    _install_anyio_to_thread_patch()
except Exception:
    pass


# asyncio's selector ticks call `select.poll().poll(timeout)` even when no
# fds are registered (the loop's _run_once still polls to honor scheduled
# callbacks / timeouts). wasi-libc forwards an empty poll list to wasmtime's
# `poll_oneoff`, which traps with `empty poll list`. The asyncio self-pipe
# (which would normally register at least one wakeup fd) is stubbed out
# above for wasi-p2 — so the registered set really is empty.
#
# Patch _PollLikeSelector.select to short-circuit when no fds are
# registered: sleep for the timeout (when finite) and return []. Loops
# without any pending IO still progress because asyncio interleaves
# scheduled-callback dispatch around the selector call.
try:
    import selectors as _sel
    import time as _time
    _orig_poll_select = _sel._PollLikeSelector.select
    def _safe_select(self, timeout=None, _orig=_orig_poll_select, _sleep=_time.sleep):
        if not self._fd_to_key:
            if timeout is not None and timeout > 0:
                _sleep(timeout)
            return []
        return _orig(self, timeout)
    _sel._PollLikeSelector.select = _safe_select
    del _sel, _time
except (ImportError, AttributeError):
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

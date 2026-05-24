"""Import hook that routes 'native-only' package calls through an offload backend.

A `meta_path` finder returns a proxy module for configured package names; attribute
access yields proxy callables, and calling one offloads
`<package>:<dotted.attr>(args, kwargs)` to a backend client (any object with
`run(env, task) -> Outcome` — SubprocessClient, MailboxClient, ...). This is the
Phase-4 mechanism (Issue #4): make a package whose native code has no wasip2 build
usable from the WASM interpreter by executing its calls in a backend (Tier 1 v86,
or any offload client).

v1 supports call-with-serializable-args. Transparent live-object proxying (ndarray
views, callbacks, stateful objects) is out of scope — that is Issue #5.
"""

from __future__ import annotations

import builtins
import importlib.abc
import importlib.machinery
import sys
import types
from typing import Iterable

from . import codecs
from .types import Codec, Raised, Task

_MISSING = object()


class OffloadError(RuntimeError):
    """A remote exception whose type has no local builtin equivalent."""


def _raise_remote(error) -> None:
    exc_type = getattr(builtins, error.kind, None)
    if isinstance(exc_type, type) and issubclass(exc_type, BaseException):
        exc: BaseException = exc_type(error.message)
    else:
        exc = OffloadError(f"{error.kind}: {error.message}")
    exc.remote_traceback = error.traceback  # type: ignore[attr-defined]
    raise exc


class _Router:
    def __init__(self, client, env: str, codec: Codec):
        self._client = client
        self._env = env
        self._codec = codec

    def invoke(self, package: str, attr_path: str, args: list, kwargs: dict):
        entry = f"{package}:{attr_path}"
        payload = {"args": args, "kwargs": kwargs}
        task = Task(entry=entry, args=codecs.encode(self._codec, payload), codec=self._codec)
        outcome = self._client.run(self._env, task)
        if isinstance(outcome, Raised):
            _raise_remote(outcome.error)
        return codecs.decode(self._codec, outcome.value)


class _Call:
    """Proxy for an attribute path inside a proxied package; call it to offload."""

    def __init__(self, router: _Router, package: str, path: tuple):
        self._router = router
        self._package = package
        self._path = path

    def __getattr__(self, name: str):
        if name.startswith("__") and name.endswith("__"):
            raise AttributeError(name)
        return _Call(self._router, self._package, self._path + (name,))

    def __call__(self, *args, **kwargs):
        return self._router.invoke(
            self._package, ".".join(self._path), list(args), dict(kwargs)
        )


class _ProxyModule(types.ModuleType):
    def __init__(self, router: _Router, package: str):
        super().__init__(package)
        self._router = router
        self._package = package

    def __getattr__(self, name: str):
        if name.startswith("__") and name.endswith("__"):
            raise AttributeError(name)
        return _Call(self._router, self._package, (name,))


class _Finder(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    def __init__(self, packages: Iterable[str], router: _Router):
        self._packages = set(packages)
        self._router = router

    def find_spec(self, fullname, path=None, target=None):
        if fullname in self._packages:
            return importlib.machinery.ModuleSpec(fullname, self)
        return None

    def create_module(self, spec):
        return _ProxyModule(self._router, spec.name)

    def exec_module(self, module):
        return None


class Hook:
    def __init__(self, finder: _Finder, saved: dict):
        self._finder = finder
        self._saved = saved

    def uninstall(self) -> None:
        try:
            sys.meta_path.remove(self._finder)
        except ValueError:
            pass
        for name, prior in self._saved.items():
            if prior is _MISSING:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = prior

    def __enter__(self) -> "Hook":
        return self

    def __exit__(self, *exc) -> None:
        self.uninstall()


def install(
    packages: Iterable[str],
    client,
    *,
    env: str = "local",
    codec: Codec = Codec.MSGPACK,
) -> Hook:
    """Route imports of `packages` through `client`. Returns a Hook to uninstall.

    `client` is any object with `run(env, task) -> Outcome`. Already-imported
    modules with these names are evicted so the proxy takes effect, and restored on
    uninstall.
    """
    names = list(packages)
    finder = _Finder(names, _Router(client, env, codec))
    saved = {}
    for name in names:
        saved[name] = sys.modules.get(name, _MISSING)
        sys.modules.pop(name, None)
    sys.meta_path.insert(0, finder)
    return Hook(finder, saved)

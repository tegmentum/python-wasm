"""
py-offload importhook shim for python-wasm (Phase 4).

Vendored slice of ~/git/python-wasm/reference-worker/py_offload/ — the
parts that make sense on the wasm guest side of the boundary. The host
side runs the worker loop separately (see scripts/serve-offload-host.sh).

Public surface:

    from _offload_shim import install_from_env
    install_from_env()  # idempotent; reads OFFLOAD_MAILBOX_DIR + OFFLOAD_PACKAGES

The catalog of native-only packages comes from the OFFLOAD_PACKAGES env
var (comma-separated). When unset, no importhook is installed.

See docs/native-execution-and-parallelism.md §4 for the WIT contract and
docs/c-ext-wheels.md for end-user usage.
"""

from __future__ import annotations

import os
import sys

from .types import Codec
from .importhook import install
from .mailbox import MailboxClient


_installed = None


def install_from_env():
    """Read OFFLOAD_MAILBOX_DIR + OFFLOAD_PACKAGES from env, install the hook.

    Returns the Hook object on success (or the existing one on a repeat call),
    or None if env vars are unset.
    """
    global _installed
    if _installed is not None:
        return _installed

    mailbox = os.environ.get("OFFLOAD_MAILBOX_DIR")
    packages_env = os.environ.get("OFFLOAD_PACKAGES", "")
    if not mailbox or not packages_env:
        return None
    packages = [p.strip() for p in packages_env.split(",") if p.strip()]
    if not packages:
        return None

    client = MailboxClient(mailbox)
    _installed = install(packages, client, env="local", codec=Codec.MSGPACK)
    return _installed


__all__ = ("install_from_env", "Codec", "install", "MailboxClient")

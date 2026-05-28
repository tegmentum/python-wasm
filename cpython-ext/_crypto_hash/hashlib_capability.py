"""hashlib_capability — backwards-compatibility shim (Phase 5.1+).

Before Phase 5.1, this module was the **opt-in** entrypoint to the
crypto-hash capability: `import hashlib_capability` monkey-patched the
stdlib hashlib to route through `_crypto_hash`.

After Phase 5.1 (committed alongside this file), the default `Lib/hashlib.py`
already routes through `_crypto_hash` — there's nothing to monkey-patch.
This shim is kept installed so that:

    import hashlib_capability  # no-op against the already-routing hashlib

still works (re-exports the public surface from the new hashlib for any
code that pulled symbols directly from this module). New code should
just use `hashlib` — `import hashlib_capability` is deprecated and may
be removed in a future release.

The auto-install at module-import time is now a no-op for the same
reason — the new Lib/hashlib.py is its own self-contained replacement,
not a target for monkey-patching.
"""

from __future__ import annotations

import warnings

# Re-export everything the new self-contained Lib/hashlib.py exposes.
# Importing it (rather than _crypto_hash directly) keeps the two paths
# in lockstep — if Lib/hashlib.py changes, this shim follows.
from hashlib import (  # noqa: F401  re-exports
    _CapHasher as _CapabilityHasher,
    new,
    algorithms_available,
    algorithms_guaranteed,
)

_SUPPORTED = algorithms_available


def install() -> None:
    """No-op since Phase 5.1.

    Pre-5.1, install() monkey-patched the stdlib hashlib to route
    through the capability. The new Lib/hashlib.py is the capability
    route by default, so there's nothing to install. Kept for source
    compatibility with code that called install() explicitly."""
    warnings.warn(
        "hashlib_capability.install() is a no-op since Phase 5.1 — "
        "Lib/hashlib.py already routes through the crypto-hash capability. "
        "Just use `import hashlib`.",
        DeprecationWarning,
        stacklevel=2,
    )


# Pre-5.1 behavior was install() at module import. Now a no-op (silent)
# so existing `import hashlib_capability` calls don't spam warnings.
# Explicit install() calls do emit the deprecation warning above.

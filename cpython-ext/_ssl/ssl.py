"""ssl — capability-routed replacement for CPython's stdlib ssl.

Phase 5.2 of the componentize-python plan. Installed at deps/cpython/
Lib/ssl.py by `make install-python-shims`, replacing CPython's stdlib
ssl wholesale. The default build no longer ships static `_ssl` (no
OpenSSL link), so the stdlib's `import _ssl` would fail — this shim
routes everything through `_ssl_capability` instead.

## Architecture

Two-layer:

  1. `ssl_capability.py` (Phase 3b) is the **implementation** — a
     fully-tested cap-routed module covering SSLContext / SSLSocket /
     create_default_context / etc., with 21 e2e assertions including
     real TLS 1.3 to example.com:443.

  2. **This file** (`ssl.py`) is a stdlib-compat **wrapper** —
     re-exports everything ssl_capability provides + adds the missing
     stdlib symbols (HAS_TLSv1_*, TLSVersion enum, SSLEOFError
     subclasses, MemoryBIO re-export, NotImplementedError stubs for
     deferred-to-v1.1 surface).

The split keeps the Phase 3b implementation untouched (still tested
via `make test-ssl-capability`) and lets this file be a thin adapter
that grows the compat surface incrementally.

## Coverage

What real-world code typically uses works:

  * `urllib.request.urlopen('https://...')` — full path including
    cert validation (Mozilla WebPKI roots bundled into the cap).
  * `ssl.create_default_context()` — Purpose.SERVER_AUTH / CLIENT_AUTH.
  * `ssl.SSLContext()`, `ctx.wrap_socket(...)` — explicit construction.
  * `ssl.CERT_REQUIRED` / `ssl.PROTOCOL_TLS_CLIENT` — context config.
  * `ssl.MemoryBIO` — for non-socket TLS (httpx, asyncio).
  * `ssl.HAS_TLSv1_*` probes — accurate constants for what the cap
    actually supports.

## Out-of-coverage surface

Symbols that raise NotImplementedError (with messages pointing at the
underlying gap):

  * `pbkdf2_hmac` etc. — wrong module (those live in hashlib; not here).
  * `SSLSession`, `SSLObject` — deferred to v1.1 of the openssl-component
    contract (see docs/phase-3-tls.md).
  * `DER_cert_to_PEM_cert`, `PEM_cert_to_DER_cert`, `get_server_certificate`,
    `cert_time_to_seconds`, `RAND_add`, `RAND_status` — niche helpers
    not yet wired through the capability.
  * `DefaultVerifyPaths`, `get_default_verify_paths` — capability uses
    bundled WebPKI roots, not OS verify paths.

Code that needs these can rebuild with STATIC_OPENSSL=1 for the old
static OpenSSL path until the cap covers them.
"""

from __future__ import annotations

import enum as _enum
import warnings as _warnings

# Re-export the cap-routed implementation. ssl_capability.py is the
# canonical Phase 3 surface — installed alongside this file by
# `make install-python-shims`.
import ssl_capability as _impl

# Direct C-extension access for items ssl_capability doesn't re-export
# (mainly MemoryBIO).
import _ssl_capability as _ext


# ---------------------------------------------------------------------------
# Re-exports from ssl_capability — the tested cap-routed surface
# ---------------------------------------------------------------------------


# Core classes (these ARE what `import ssl` users reach for)
SSLContext = _impl.SSLContext
SSLSocket = _impl.SSLSocket
SSLError = _impl.SSLError
SSLCertVerificationError = _impl.SSLCertVerificationError
Purpose = _impl.Purpose

# Constants
CERT_NONE = _impl.CERT_NONE
CERT_OPTIONAL = _impl.CERT_OPTIONAL
CERT_REQUIRED = _impl.CERT_REQUIRED
PROTOCOL_TLS_CLIENT = _impl.PROTOCOL_TLS_CLIENT
OPENSSL_VERSION = _impl.OPENSSL_VERSION
OPENSSL_VERSION_NUMBER = _impl.OPENSSL_VERSION_NUMBER
OPENSSL_VERSION_INFO = _impl.OPENSSL_VERSION_INFO

# Trust store anchored to the bundled WebPKI roots
CA_BUNDLE_SHA256 = _impl.CA_BUNDLE_SHA256
CA_BUNDLE_DATE = _impl.CA_BUNDLE_DATE
CA_BUNDLE_CERT_COUNT = _impl.CA_BUNDLE_CERT_COUNT

# Public constructors / helpers
create_default_context = _impl.create_default_context
_create_unverified_context = _impl._create_unverified_context

# http.client + urllib.request reach for `ssl._create_default_https_context`
# (note: with `default` in the name) to build the HTTPS context. ssl_capability
# exposes the same callable as `_create_https_context`; alias it here.
_create_default_https_context = _impl._create_https_context

# Random
RAND_bytes = _impl.RAND_bytes
RAND_priv_bytes = _impl.RAND_priv_bytes


# ---------------------------------------------------------------------------
# Symbols ssl_capability doesn't re-export but the cap provides
# ---------------------------------------------------------------------------


# MemoryBIO — the cap's MemoryBIO works (Phase 3b.2 has e2e tests for
# basic + EOF + FIFO + buffer-protocol semantics). Re-export directly.
MemoryBIO = _ext.MemoryBIO


# ---------------------------------------------------------------------------
# Stdlib-shape HAS_* feature flags
# ---------------------------------------------------------------------------

HAS_SSLv2 = False          # SSL 2/3 are dead; intentionally not supported.
HAS_SSLv3 = False
HAS_TLSv1 = True           # legacy TLS — openssl-component supports it.
HAS_TLSv1_1 = True
HAS_TLSv1_2 = True
HAS_TLSv1_3 = True
HAS_ALPN = True            # alpn-selected exposed via peer-info.alpn.
HAS_ECDH = True            # default cipher suites use ECDH.
HAS_NPN = False            # NPN was deprecated in favor of ALPN.
HAS_SNI = True             # SNI is always set when wrap_socket gets server_hostname.
HAS_PSK = False            # PSK not wired in the openssl-component WIT.
HAS_NEVER_CHECK_COMMON_NAME = True  # OpenSSL ≥ 1.0.2 default.


# ---------------------------------------------------------------------------
# Stdlib-shape error subclasses
# ---------------------------------------------------------------------------


class SSLEOFError(SSLError):
    """TLS connection ended without a clean shutdown."""


class SSLSyscallError(SSLError):
    """An underlying syscall failed during TLS I/O."""


class SSLWantReadError(SSLError):
    """Non-blocking SSLSocket needs to read more data — try again later."""


class SSLWantWriteError(SSLError):
    """Non-blocking SSLSocket needs to write more data — try again later."""


class SSLZeroReturnError(SSLError):
    """Peer closed the TLS connection (clean shutdown received)."""


CertificateError = SSLCertVerificationError  # stdlib alias kept for compat


# ---------------------------------------------------------------------------
# Stdlib enums (IntEnum / IntFlag) — provide for code that probes
# ---------------------------------------------------------------------------


class TLSVersion(_enum.IntEnum):
    """TLS protocol versions, matching stdlib ssl.TLSVersion."""
    MINIMUM_SUPPORTED = -2
    SSLv3 = 768
    TLSv1 = 769
    TLSv1_1 = 770
    TLSv1_2 = 771
    TLSv1_3 = 772
    MAXIMUM_SUPPORTED = -1


class VerifyMode(_enum.IntEnum):
    """Cert-verification modes — matches stdlib ssl.VerifyMode."""
    CERT_NONE = 0
    CERT_OPTIONAL = 1
    CERT_REQUIRED = 2


class Options(_enum.IntFlag):
    """SSLContext.options bitfield. Defaults loosely match stdlib OpenSSL
    defaults; advisory — the underlying capability decides effective behavior."""
    OP_NO_COMPRESSION = 0x00020000
    OP_CIPHER_SERVER_PREFERENCE = 0x00400000
    OP_SINGLE_DH_USE = 0x00100000
    OP_SINGLE_ECDH_USE = 0x00080000
    OP_NO_SSLv2 = 0x01000000
    OP_NO_SSLv3 = 0x02000000
    OP_NO_TLSv1 = 0x04000000
    OP_NO_TLSv1_1 = 0x10000000
    OP_NO_TLSv1_2 = 0x08000000
    OP_NO_TLSv1_3 = 0x20000000
    OP_NO_TICKET = 0x00004000
    OP_NO_RENEGOTIATION = 0x40000000


class VerifyFlags(_enum.IntFlag):
    """SSLContext.verify_flags — matches stdlib ssl.VerifyFlags."""
    VERIFY_DEFAULT = 0
    VERIFY_CRL_CHECK_LEAF = 0x4
    VERIFY_CRL_CHECK_CHAIN = 0xc
    VERIFY_X509_STRICT = 0x20
    VERIFY_X509_TRUSTED_FIRST = 0x8000
    VERIFY_ALLOW_PROXY_CERTS = 0x40
    VERIFY_X509_PARTIAL_CHAIN = 0x80000


# Module-level aliases that stdlib defines too (for `from ssl import *`)
PROTOCOL_TLS = PROTOCOL_TLS_CLIENT  # alias; TLS_CLIENT is the only one we support

# urllib3 (>=2) imports these as bare names from ssl, not via Options/VerifyFlags
# enums. Stdlib defines them at module level too.
OP_NO_COMPRESSION         = Options.OP_NO_COMPRESSION.value
OP_CIPHER_SERVER_PREFERENCE = Options.OP_CIPHER_SERVER_PREFERENCE.value
OP_SINGLE_DH_USE          = Options.OP_SINGLE_DH_USE.value
OP_SINGLE_ECDH_USE        = Options.OP_SINGLE_ECDH_USE.value
OP_NO_SSLv2               = Options.OP_NO_SSLv2.value
OP_NO_SSLv3               = Options.OP_NO_SSLv3.value
OP_NO_TLSv1               = Options.OP_NO_TLSv1.value
OP_NO_TLSv1_1             = Options.OP_NO_TLSv1_1.value
OP_NO_TLSv1_2             = Options.OP_NO_TLSv1_2.value
OP_NO_TLSv1_3             = Options.OP_NO_TLSv1_3.value
OP_NO_TICKET              = Options.OP_NO_TICKET.value
OP_NO_RENEGOTIATION       = Options.OP_NO_RENEGOTIATION.value
VERIFY_DEFAULT            = VerifyFlags.VERIFY_DEFAULT.value
VERIFY_CRL_CHECK_LEAF     = VerifyFlags.VERIFY_CRL_CHECK_LEAF.value
VERIFY_CRL_CHECK_CHAIN    = VerifyFlags.VERIFY_CRL_CHECK_CHAIN.value
VERIFY_X509_STRICT        = VerifyFlags.VERIFY_X509_STRICT.value
VERIFY_X509_TRUSTED_FIRST = VerifyFlags.VERIFY_X509_TRUSTED_FIRST.value
VERIFY_ALLOW_PROXY_CERTS  = VerifyFlags.VERIFY_ALLOW_PROXY_CERTS.value
VERIFY_X509_PARTIAL_CHAIN = VerifyFlags.VERIFY_X509_PARTIAL_CHAIN.value


# ---------------------------------------------------------------------------
# NotImplementedError stubs for deferred-to-v1.1 surface
# ---------------------------------------------------------------------------


def _stub(name: str, reason: str):
    """Return a callable that raises NotImplementedError with a clear
    message. Used for stdlib ssl API the capability doesn't (yet) cover."""
    def _raiser(*_args, **_kwargs):
        raise NotImplementedError(
            f"ssl.{name} is not available via the openssl-component "
            f"capability in the default build ({reason}). Rebuild with "
            f"STATIC_OPENSSL=1 to restore the static _ssl path that "
            f"supports it.")
    _raiser.__name__ = name
    return _raiser


def _stub_class(name: str, reason: str):
    """Same as _stub but as a class so isinstance() checks still work
    (returning False) and instantiation explodes with a clear message."""
    class _Stub:
        def __init__(self, *_args, **_kwargs):
            raise NotImplementedError(
                f"ssl.{name} is not available via the openssl-component "
                f"capability in the default build ({reason}). Rebuild "
                f"with STATIC_OPENSSL=1 for the static _ssl path that "
                f"supports it.")
    _Stub.__name__ = name
    _Stub.__qualname__ = name
    return _Stub


SSLSession = _stub_class(
    "SSLSession",
    "session resumption — deferred to openssl-component v1.1 per docs/phase-3-tls.md")
SSLObject = _stub_class(
    "SSLObject",
    "wrap_bio/asyncio MemoryBIO-driven path — deferred to openssl-component v1.1")

DefaultVerifyPaths = _stub_class(
    "DefaultVerifyPaths",
    "no OS verify-paths — capability bundles its own Mozilla WebPKI roots")

DER_cert_to_PEM_cert = _stub(
    "DER_cert_to_PEM_cert",
    "cert-format conversion helper not yet wired through the capability")
PEM_cert_to_DER_cert = _stub(
    "PEM_cert_to_DER_cert",
    "cert-format conversion helper not yet wired through the capability")
def get_server_certificate(addr, ssl_version=None, ca_certs=None, timeout=None):
    """Connect to (host, port) and return the server's certificate as PEM.

    Implemented as the standard pattern: open a TLS connection, call
    getpeercert(binary_form=True) → DER bytes, base64-wrap into PEM.

    The cap-side now exposes the peer cert via openssl-component's
    `peer-info.peer-chain` (which ssl_capability surfaces as
    `SSLSocket.getpeercert`), so this works without `STATIC_OPENSSL=1`."""
    import base64 as _base64
    import socket as _socket

    host, port = addr
    if ssl_version is not None and ssl_version != PROTOCOL_TLS_CLIENT:
        raise NotImplementedError(
            f"ssl.get_server_certificate: only PROTOCOL_TLS_CLIENT supported "
            f"(got {ssl_version})")

    ctx = create_default_context()
    if ca_certs is not None:
        # Allow the caller to override the bundled WebPKI roots
        with open(ca_certs, "rb") as fh:
            ctx.load_verify_locations(cadata=fh.read())
    # The cap doesn't currently support "fetch cert without validating it"
    # — server-cert verification runs against the bundled roots. For
    # untrusted-cert inspection use cases the caller should pass ca_certs
    # explicitly with the expected anchor.

    sock = _socket.create_connection(
        (host, port), timeout=timeout if timeout is not None else 15)
    try:
        ssock = ctx.wrap_socket(sock, server_hostname=host)
        try:
            der = ssock.getpeercert(binary_form=True)
        finally:
            try: ssock.close()
            except Exception: pass
    finally:
        try: sock.close()
        except Exception: pass

    if der is None:
        raise SSLError("server did not present a certificate")
    # Convert DER -> PEM (the format stdlib get_server_certificate returns)
    b64 = _base64.encodebytes(der).decode("ascii").rstrip("\n")
    return (f"-----BEGIN CERTIFICATE-----\n{b64}\n"
            f"-----END CERTIFICATE-----\n")
get_default_verify_paths = _stub(
    "get_default_verify_paths",
    "no OS verify-paths — capability bundles its own Mozilla WebPKI roots")
cert_time_to_seconds = _stub(
    "cert_time_to_seconds",
    "cert-time parsing helper not yet wired through the capability")
RAND_add = _stub(
    "RAND_add",
    "PRNG seeding helper not exposed by openssl:component/random "
    "(RAND_bytes / RAND_priv_bytes work)")
RAND_status = _stub(
    "RAND_status",
    "PRNG-status helper not exposed by openssl:component/random")
create_connection = _stub(
    "create_connection",
    "socket-creation helper not yet wired — construct your own socket "
    "+ ctx.wrap_socket() in the meantime")
get_protocol_name = _stub(
    "get_protocol_name",
    "minor helper not yet wired through the capability")


# ---------------------------------------------------------------------------
# AlertDescription / SSLErrorNumber — many constants, rarely consumed
# ---------------------------------------------------------------------------


class AlertDescription(_enum.IntEnum):
    """TLS alert descriptions. Wire-protocol identifiers; rarely consumed
    by user code but stdlib ships them for completeness."""
    ALERT_DESCRIPTION_CLOSE_NOTIFY = 0
    ALERT_DESCRIPTION_UNEXPECTED_MESSAGE = 10
    ALERT_DESCRIPTION_BAD_RECORD_MAC = 20
    ALERT_DESCRIPTION_RECORD_OVERFLOW = 22
    ALERT_DESCRIPTION_HANDSHAKE_FAILURE = 40
    ALERT_DESCRIPTION_BAD_CERTIFICATE = 42
    ALERT_DESCRIPTION_UNSUPPORTED_CERTIFICATE = 43
    ALERT_DESCRIPTION_CERTIFICATE_REVOKED = 44
    ALERT_DESCRIPTION_CERTIFICATE_EXPIRED = 45
    ALERT_DESCRIPTION_CERTIFICATE_UNKNOWN = 46
    ALERT_DESCRIPTION_ILLEGAL_PARAMETER = 47
    ALERT_DESCRIPTION_UNKNOWN_CA = 48
    ALERT_DESCRIPTION_ACCESS_DENIED = 49
    ALERT_DESCRIPTION_DECODE_ERROR = 50
    ALERT_DESCRIPTION_DECRYPT_ERROR = 51
    ALERT_DESCRIPTION_PROTOCOL_VERSION = 70
    ALERT_DESCRIPTION_INSUFFICIENT_SECURITY = 71
    ALERT_DESCRIPTION_INTERNAL_ERROR = 80
    ALERT_DESCRIPTION_USER_CANCELLED = 90
    ALERT_DESCRIPTION_NO_RENEGOTIATION = 100
    ALERT_DESCRIPTION_UNSUPPORTED_EXTENSION = 110


class SSLErrorNumber(_enum.IntEnum):
    """OpenSSL error codes. Matches stdlib ssl.SSLErrorNumber."""
    SSL_ERROR_NONE = 0
    SSL_ERROR_SSL = 1
    SSL_ERROR_WANT_READ = 2
    SSL_ERROR_WANT_WRITE = 3
    SSL_ERROR_WANT_X509_LOOKUP = 4
    SSL_ERROR_SYSCALL = 5
    SSL_ERROR_ZERO_RETURN = 6
    SSL_ERROR_WANT_CONNECT = 7
    SSL_ERROR_EOF = 8
    SSL_ERROR_INVALID_ERROR_CODE = 10


# Hoist the alert enum members to module level (stdlib does this too)
for _alert in AlertDescription:
    globals()[_alert.name] = _alert.value
del _alert


# ---------------------------------------------------------------------------
# __all__ — keep `from ssl import *` predictable
# ---------------------------------------------------------------------------


__all__ = (
    # Core classes
    "SSLContext", "SSLSocket", "SSLObject", "SSLSession",
    "SSLError", "SSLCertVerificationError", "CertificateError",
    "SSLEOFError", "SSLSyscallError", "SSLWantReadError",
    "SSLWantWriteError", "SSLZeroReturnError",
    "MemoryBIO",
    "Purpose",
    "DefaultVerifyPaths",
    # Enums
    "TLSVersion", "VerifyMode", "Options", "VerifyFlags",
    "AlertDescription", "SSLErrorNumber",
    # Constants
    "CERT_NONE", "CERT_OPTIONAL", "CERT_REQUIRED",
    "PROTOCOL_TLS", "PROTOCOL_TLS_CLIENT",
    "OPENSSL_VERSION", "OPENSSL_VERSION_NUMBER", "OPENSSL_VERSION_INFO",
    "CA_BUNDLE_SHA256", "CA_BUNDLE_DATE", "CA_BUNDLE_CERT_COUNT",
    "HAS_SSLv2", "HAS_SSLv3", "HAS_TLSv1", "HAS_TLSv1_1",
    "HAS_TLSv1_2", "HAS_TLSv1_3", "HAS_ALPN", "HAS_ECDH",
    "HAS_NPN", "HAS_SNI", "HAS_PSK", "HAS_NEVER_CHECK_COMMON_NAME",
    # Functions
    "create_default_context", "_create_unverified_context",
    "_create_default_https_context",
    "RAND_bytes", "RAND_priv_bytes", "RAND_add", "RAND_status",
    "DER_cert_to_PEM_cert", "PEM_cert_to_DER_cert",
    "get_server_certificate", "get_default_verify_paths",
    "cert_time_to_seconds", "create_connection", "get_protocol_name",
)

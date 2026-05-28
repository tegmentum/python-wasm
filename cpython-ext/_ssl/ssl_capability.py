"""ssl_capability — drop-in-ish ``ssl`` shim backed by ``_ssl_capability``
(the openssl-component-backed TLS extension shipped in Phase 3b of the
componentize-python plan).

Why a separate module instead of monkey-patching the stdlib ``ssl``?
CPython's ``ssl.py`` pulls dozens of symbols from ``_ssl`` (SSLSession,
txt2obj, nid2obj, _DEFAULT_CIPHERS, _OPENSSL_API_VERSION, …) that
``_ssl_capability`` doesn't expose. Stubbing them all out in v1 would be
fragile. A separate module:

  - lets v1 ship today,
  - keeps the static-OpenSSL ``ssl`` module unaffected (so existing code
    using static ``ssl`` keeps working unchanged),
  - covers what ``urllib.request.urlopen('https://...')`` actually needs.

Usage::

    import ssl_capability as ssl
    ctx = ssl.create_default_context()
    sock = ctx.wrap_socket(host='example.com', port=443,
                            server_hostname='example.com')
    sock.write(b'GET / HTTP/1.0\\r\\nHost: example.com\\r\\n\\r\\n')
    print(sock.read(4096))
    sock.shutdown()

For ``urllib``::

    import ssl_capability as _sslcap
    import ssl as _stdlib_ssl
    _stdlib_ssl._create_default_https_context = _sslcap.create_default_context
    # Then urllib.request.urlopen('https://...') uses our TLS.

Phase 5 of the componentize-python plan retires the static OpenSSL
``_ssl``; at that point this shim becomes the only TLS surface and ``ssl.py``
gets re-routed properly.
"""

from __future__ import annotations

import io as _io
import _ssl_capability

# ----------------------------------------------------------------------------
# Stdlib ssl mirror constants
# ----------------------------------------------------------------------------

# CERT_* — accepting/requiring peer certs.
CERT_NONE     = _ssl_capability.CERT_NONE
CERT_OPTIONAL = _ssl_capability.CERT_OPTIONAL
CERT_REQUIRED = _ssl_capability.CERT_REQUIRED

# Purpose flag accepted by create_default_context. Stdlib uses an enum;
# we accept any value (it's advisory in v1 — all callers want SERVER_AUTH).
PROTOCOL_TLS_CLIENT = _ssl_capability.PROTOCOL_TLS_CLIENT


class Purpose:
    """Mirror of ssl.Purpose. Both flags do the same thing in v1 — the
    capability has one default trust store + one client config shape."""
    SERVER_AUTH = "server_auth"
    CLIENT_AUTH = "client_auth"


# Errors — surface SSLError directly so `except ssl.SSLError` works.
SSLError = _ssl_capability.SSLError
class SSLCertVerificationError(SSLError):
    """Raised when verify_mode == CERT_REQUIRED and chain validation fails.
    In v1 we don't distinguish this from a generic SSLError at the binding
    layer; both surface as SSLError. Kept as a subclass for `except`
    compatibility — explicit handlers still match."""


# ----------------------------------------------------------------------------
# SSLContext — wrapper around _ssl_capability._SSLContext
# ----------------------------------------------------------------------------

class SSLContext:
    """Configuration for a TLS connection. Mirrors ``ssl.SSLContext`` for the
    methods/properties most TLS users actually touch."""

    def __init__(self, protocol: int = PROTOCOL_TLS_CLIENT):
        self._inner = _ssl_capability._SSLContext(protocol)
        # Stdlib-compat attributes callers (urllib, http.client, requests)
        # inspect or set. v1 either honors them where possible or ignores
        # them (with intent) — none affect TLS correctness.
        self.check_hostname        = True
        self.post_handshake_auth   = None   # TLS 1.3 post-handshake mTLS — out of v1 scope
        self.options               = 0      # opaque option bitmask; no-op in v1
        self.maximum_version       = None   # capability uses TLSv1.3 default
        self.minimum_version       = None   # capability uses TLSv1.2 default
        self.session_stats         = lambda: {}     # stdlib returns a dict
        self.set_ciphers           = lambda c: None # accept ssl.py's cipher str

    # --- verify ---
    @property
    def verify_mode(self) -> int:
        return self._inner.verify_mode

    @verify_mode.setter
    def verify_mode(self, value: int) -> None:
        self._inner.verify_mode = value

    # --- trust / cert material ---
    def load_default_certs(self, purpose=Purpose.SERVER_AUTH) -> None:
        """Install bundled Mozilla WebPKI roots. `purpose` is accepted for
        API parity (v1 ignores it; SERVER_AUTH and CLIENT_AUTH use the same
        bundle)."""
        self._inner.load_default_certs()

    def load_verify_locations(self, cafile=None, capath=None, cadata=None) -> None:
        """File/path forms require host-FS access we don't have in this lane.
        Accepts ``cadata`` (the in-memory form) as bytes (PEM) — matches the
        common case of pinned roots."""
        if cafile is not None or capath is not None:
            raise NotImplementedError(
                "ssl_capability.load_verify_locations: cafile/capath need "
                "host-FS access; pass cadata=<PEM bytes> instead.")
        if cadata is None:
            raise TypeError("must pass cadata=<PEM bytes>")
        if isinstance(cadata, str):
            cadata = cadata.encode("ascii")
        self._inner.set_ca_certs(cadata)

    def load_cert_chain(self, certfile=None, keyfile=None, password=None,
                        certdata=None, keydata=None) -> None:
        """Same file-vs-bytes story as load_verify_locations. Stdlib only
        takes paths; ssl_capability adds certdata/keydata bytes parameters
        for the path-less wasm lane."""
        if certfile is not None or keyfile is not None:
            raise NotImplementedError(
                "ssl_capability.load_cert_chain: certfile/keyfile need "
                "host-FS access; pass certdata=/keydata= bytes instead.")
        if certdata is None or keydata is None:
            raise TypeError("must pass certdata + keydata (PEM bytes)")
        self._inner.set_client_cert(certdata, keydata)

    def set_alpn_protocols(self, protocols) -> None:
        self._inner.set_alpn_protocols(list(protocols))

    # --- wrap ---
    def wrap_socket(self, sock=None, *, server_hostname=None,
                    host=None, port=None,
                    do_handshake_on_connect=True,
                    suppress_ragged_eofs=True,
                    session=None) -> "SSLSocket":
        """Two calling conventions:

          1) Stdlib-compatible: ``ctx.wrap_socket(socket_obj,
             server_hostname='...')``. The `socket_obj.getpeername()` is
             used as (host, port) so the openssl-component can re-establish
             its own internal TCP connection.

          2) Capability-native: ``ctx.wrap_socket(host='example.com',
             port=443, server_hostname='example.com')``. Skip the socket;
             let openssl-component handle the TCP itself.

        Mode 1 is provided for compatibility but openssl-component DOES NOT
        currently accept an existing fd — it always opens its own TCP.
        Practically, mode 1 closes `sock` and opens a fresh one. Document
        the limitation; the alternative is upstream changes to
        openssl-component to take an fd."""
        if sock is not None and (host is not None or port is not None):
            raise TypeError("pass either a socket OR host/port, not both")
        if sock is not None:
            # Mode 1: stdlib-compat. Pull host/port from the socket, close it.
            peer = sock.getpeername()
            host, port = peer[0], peer[1]
            try:
                sock.close()
            except Exception:
                pass
            if server_hostname is None:
                # Stdlib does check_hostname work here; we default to host.
                server_hostname = host
        elif host is None or port is None:
            raise TypeError("must pass either a socket or host+port")
        if server_hostname is None:
            server_hostname = host
        inner = self._inner.wrap_socket(host, int(port),
                                         server_hostname=server_hostname)
        return SSLSocket(inner)


class SSLSocket:
    """An open TLS connection. Wraps ``_ssl_capability._SSLSocket``; method
    names match stdlib ``ssl.SSLSocket`` for drop-in use."""

    def __init__(self, inner):
        self._inner = inner

    # --- I/O ---
    def read(self, buflen: int = 8192) -> bytes:
        # The C layer (openssl-component) has a sharp edge around clean
        # peer-shutdown: once it raises SSLError("SSL connection is closed"),
        # all further reads also raise — there's no way to recover any TLS
        # records that may still have been buffered when close-notify fired.
        # Concretely: a chunked HTTP response often arrives as one big TLS
        # record (headers + body) followed by a small trailing record (the
        # `0\r\n\r\n` chunk terminator). When urllib reads the first call's
        # data and then tries to read more, the TCP connection has typically
        # already been closed by the server. The first read returns the bulk
        # of the data; the second read raises before delivering the trailer.
        #
        # Workaround: PROACTIVELY drain after every successful read. Issue
        # a follow-up small read; if data comes back, concatenate; if the
        # close exception fires, swallow it — we already have data, so this
        # is the natural EOF. The cost is one extra C call per chunk, which
        # is negligible vs the alternative (a lost terminator + crashed
        # parser in the caller).
        try:
            data = self._inner.read(buflen)
        except SSLError as e:
            msg = str(e).lower()
            if "connection is closed" in msg or "zero return" in msg:
                return b""
            raise

        if not data:
            return data  # natural EOF, nothing to drain

        # Drain: combine any immediately-following trailing record into
        # this return. Stops on empty result or close exception.
        try:
            extra = self._inner.read(buflen)
            while extra:
                data += extra
                extra = self._inner.read(buflen)
        except SSLError as e:
            msg = str(e).lower()
            if "connection is closed" not in msg and "zero return" not in msg:
                # A real error mid-drain — propagate, but don't lose the
                # data we already have. Re-raising loses `data`; instead
                # store it on the exception so callers can recover it.
                e._drained = data  # type: ignore[attr-defined]
                raise
        return data

    def write(self, data) -> int:
        return self._inner.write(data)

    # Stdlib ssl.SSLSocket inherits socket methods; provide the minimum
    # needed by urllib (which goes through socket.makefile()).
    def recv(self, buflen: int = 8192, flags: int = 0) -> bytes:
        return self.read(buflen)

    def send(self, data, flags: int = 0) -> int:
        return self.write(data)

    def sendall(self, data, flags: int = 0) -> None:
        view = memoryview(data)
        while view:
            n = self.write(view)
            view = view[n:]

    def pending(self) -> int:
        return self._inner.pending()

    def shutdown(self, how=None) -> None:
        # Stdlib takes a SHUT_RD/WR/RDWR arg; we collapse to full shutdown.
        self._inner.shutdown()

    def close(self) -> None:
        self.shutdown()

    # --- introspection ---
    def version(self) -> str:
        return self._inner.version()

    def cipher(self) -> tuple:
        return self._inner.cipher()

    def selected_alpn_protocol(self):
        return self._inner.selected_alpn_protocol()

    def getpeercert(self, binary_form: bool = False):
        """Return the peer's certificate.

        With binary_form=True, returns DER bytes (the stdlib API). The
        binary_form=False variant (returning a parsed dict) isn't
        implemented — would need x509-info parsing wired through the
        capability. Most TLS validation users only need binary_form."""
        der = self._inner.peer_cert_der()
        if der is None:
            return {} if not binary_form else None
        if binary_form:
            return der
        raise NotImplementedError(
            "ssl_capability.getpeercert(binary_form=False) not implemented — "
            "parsed cert dict needs x509-info wiring through the cap. "
            "Pass binary_form=True to get DER bytes.")

    @property
    def server_hostname(self):
        return self._inner.server_hostname

    # urllib expects a few socket-ish things — makefile() in particular.
    def makefile(self, mode: str = "r", buffering=None, *, encoding=None,
                 errors=None, newline=None):
        """Minimal makefile() — opens an io wrapper over send/recv."""
        raw = _SocketReader(self) if "r" in mode else _SocketWriter(self)
        if "b" in mode:
            return _io.BufferedReader(raw) if "r" in mode else _io.BufferedWriter(raw)
        return _io.TextIOWrapper(_io.BufferedReader(raw), encoding=encoding or "utf-8",
                                  errors=errors, newline=newline)


class _SocketReader(_io.RawIOBase):
    def __init__(self, sock):
        super().__init__()
        self._sock = sock
    def readable(self):
        return True
    def readinto(self, buf):
        data = self._sock.read(len(buf))
        n = len(data)
        buf[:n] = data
        return n


class _SocketWriter(_io.RawIOBase):
    def __init__(self, sock):
        super().__init__()
        self._sock = sock
    def writable(self):
        return True
    def write(self, data):
        return self._sock.write(bytes(data))


# ----------------------------------------------------------------------------
# Module-level helpers — mirror what ssl.py / urllib expect
# ----------------------------------------------------------------------------

def create_default_context(purpose=Purpose.SERVER_AUTH, *,
                             cafile=None, capath=None, cadata=None) -> SSLContext:
    """Return an SSLContext configured for typical HTTPS-client use:
    TLS 1.2+, CERT_REQUIRED, WebPKI roots loaded."""
    ctx = SSLContext(PROTOCOL_TLS_CLIENT)
    ctx.verify_mode = CERT_REQUIRED
    if cadata is not None:
        ctx.load_verify_locations(cadata=cadata)
    elif cafile is not None or capath is not None:
        # Stdlib would load them; we don't have host FS. Fall back to defaults.
        ctx.load_default_certs(purpose)
    else:
        ctx.load_default_certs(purpose)
    return ctx


def _create_unverified_context(*args, **kwargs) -> SSLContext:
    """Match stdlib's name. Returns a context with verify_mode=CERT_NONE."""
    ctx = SSLContext(PROTOCOL_TLS_CLIENT)
    ctx.verify_mode = CERT_NONE
    return ctx


# Stdlib has an `ssl._create_default_https_context` that http.client invokes
# directly (not via create_default_context). Mirror it.
_create_default_https_context = create_default_context

# Stdlib http.client also accepts `_create_https_context = lambda http_version: ...`
# patched in. Provide a sensible default that ignores the version arg.
def _create_https_context(*args, **kwargs):
    return create_default_context()


# Bytes-level helpers re-exported from the C module.
RAND_bytes      = _ssl_capability.RAND_bytes
RAND_priv_bytes = _ssl_capability.RAND_priv_bytes

# Identity strings — mirror stdlib ssl module attributes.
OPENSSL_VERSION         = _ssl_capability.OPENSSL_VERSION
OPENSSL_VERSION_NUMBER  = _ssl_capability.OPENSSL_VERSION_NUMBER
OPENSSL_VERSION_INFO    = _ssl_capability.OPENSSL_VERSION_INFO

# CA bundle provenance.
CA_BUNDLE_SHA256        = _ssl_capability.CA_BUNDLE_SHA256
CA_BUNDLE_DATE          = _ssl_capability.CA_BUNDLE_DATE
CA_BUNDLE_CERT_COUNT    = _ssl_capability.CA_BUNDLE_CERT_COUNT


__all__ = [
    "CERT_NONE", "CERT_OPTIONAL", "CERT_REQUIRED",
    "PROTOCOL_TLS_CLIENT", "Purpose",
    "SSLError", "SSLCertVerificationError",
    "SSLContext", "SSLSocket",
    "create_default_context", "_create_unverified_context",
    "RAND_bytes", "RAND_priv_bytes",
    "OPENSSL_VERSION", "OPENSSL_VERSION_NUMBER", "OPENSSL_VERSION_INFO",
    "CA_BUNDLE_SHA256", "CA_BUNDLE_DATE", "CA_BUNDLE_CERT_COUNT",
]

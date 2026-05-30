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

# Re-export the C-side MemoryBIO type at the module surface so user code
# (and the stdlib ssl shim) can construct it the same way ssl.MemoryBIO()
# works.
MemoryBIO = _ssl_capability.MemoryBIO

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


class SSLWantReadError(SSLError):
    """Memory-BIO mode: the TLS state machine needs more ciphertext from
    the peer. Caller should drain its out-BIO, send those bytes over the
    network, receive whatever the peer sends back, push into the in-BIO,
    and retry the operation."""


class SSLWantWriteError(SSLError):
    """Memory-BIO mode: the TLS state machine needs to send more
    ciphertext. Caller should drain its out-BIO to the network and retry."""


class SSLSession:
    """Opaque session-ticket wrapper. Pair with ``SSLContext.wrap_socket
    (..., session=...)`` to resume a previous TLS 1.3 session and skip
    the full handshake. Tickets come from
    ``SSLSocket.session.ticket`` / ``SSLSocket.session_ticket()`` and
    are good once (the server may rotate).

    Stdlib ``ssl.SSLSession`` is opaque too; equality is by identity
    here, mirroring the stdlib contract."""

    __slots__ = ("_ticket",)

    def __init__(self, ticket: bytes) -> None:
        if not isinstance(ticket, (bytes, bytearray, memoryview)):
            raise TypeError("SSLSession ticket must be bytes-like")
        self._ticket = bytes(ticket)

    @property
    def ticket(self) -> bytes:
        return self._ticket

    @property
    def has_ticket(self) -> bool:
        return bool(self._ticket)

    def __bytes__(self) -> bytes:
        return self._ticket

    def __len__(self) -> int:
        return len(self._ticket)

    def __bool__(self) -> bool:
        return bool(self._ticket)

    def __repr__(self) -> str:
        return f"<SSLSession ticket={len(self._ticket)} bytes>"


def _map_ssl_want(exc: BaseException) -> BaseException:
    """Translate the cap's SSL_ERROR_WANT_{READ,WRITE} marker (raised as
    SSLError with a tagged message) into the stdlib's typed exceptions
    so async drivers (anyio, httpx) can branch on the type."""
    msg = str(exc)
    if "SSL_ERROR_WANT_READ" in msg:
        return SSLWantReadError(msg)
    if "SSL_ERROR_WANT_WRITE" in msg:
        return SSLWantWriteError(msg)
    return exc


# ----------------------------------------------------------------------------
# getpeercert() helpers — turn the C ext's raw certificate-info dict into
# the stdlib's nested-tuple shape (((( 'commonName', 'host'),),),) with
# GMT-formatted dates and normalised SAN entries.
# ----------------------------------------------------------------------------

# OID short name → stdlib long name. The cap returns whatever OpenSSL's
# X509_NAME_ENTRY short-name lookup yields ("CN", "O", "C", "emailAddress",
# raw OID dotted-string for the rest). Stdlib spells the common ones out.
_OID_LONG_NAMES = {
    "CN":           "commonName",
    "C":            "countryName",
    "ST":           "stateOrProvinceName",
    "L":            "localityName",
    "O":            "organizationName",
    "OU":           "organizationalUnitName",
    "emailAddress": "emailAddress",
    "SN":           "surname",
    "GN":           "givenName",
    "T":            "title",
    "DC":           "domainComponent",
    "serialNumber": "serialNumber",
    "businessCategory": "businessCategory",
    "street":       "streetAddress",
    "postalCode":   "postalCode",
}

_MONTH_ABBR = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")


def _name_to_stdlib(raw_name):
    """[(oid, val), …]  →  ((('longName', 'val'),), …)"""
    return tuple(
        ((_OID_LONG_NAMES.get(oid, oid), val),)
        for oid, val in raw_name
    )


def _iso_to_gmt(iso):
    """`2025-08-24T00:00:00Z`  →  `Aug 24 00:00:00 2025 GMT` (the
    format stdlib `getpeercert` returns under notBefore/notAfter)."""
    try:
        date_part, time_part = iso.split("T", 1)
        y, m, d = date_part.split("-")
        time_part = time_part.rstrip("Z")
        if "." in time_part:
            time_part = time_part.split(".", 1)[0]
        # stdlib zero-pads the day to width 2 with a leading space.
        return f"{_MONTH_ABBR[int(m) - 1]} {int(d):2d} {time_part} {y} GMT"
    except Exception:
        return iso


def _san_to_stdlib(raw_sans):
    """Translate the cap's general-name list into stdlib's
    subjectAltName tuple-of-(kind, value) shape. `IP` entries arrive as
    bytes (4 = IPv4, 16 = IPv6); format them as ipaddress.IPv*Address
    strings to match what OpenSSL prints under "IP Address"."""
    out = []
    for kind, val in raw_sans:
        if kind == "IP" and isinstance(val, (bytes, bytearray)):
            try:
                import ipaddress
                b = bytes(val)
                if len(b) == 4:
                    out.append(("IP Address", str(ipaddress.IPv4Address(b))))
                elif len(b) == 16:
                    out.append(("IP Address", str(ipaddress.IPv6Address(b))))
                else:
                    out.append(("IP Address", b.hex()))
            except Exception:
                out.append(("IP Address", bytes(val).hex()))
        else:
            out.append((kind, val))
    return tuple(out)


def _format_peer_cert(info):
    """Project the C ext's raw certificate-info dict into the stdlib
    `getpeercert(False)` shape."""
    return {
        "subject":        _name_to_stdlib(info["subject"]),
        "issuer":         _name_to_stdlib(info["issuer"]),
        "version":        info["version"],
        "serialNumber":   info["serial_hex"],
        "notBefore":      _iso_to_gmt(info["not_before"]),
        "notAfter":       _iso_to_gmt(info["not_after"]),
        "subjectAltName": _san_to_stdlib(info["subject_alt_names"]),
    }


# ----------------------------------------------------------------------------
# SSLContext — wrapper around _ssl_capability._SSLContext
# ----------------------------------------------------------------------------

class _SSLContextDescriptors:
    """Base for ``SSLContext``. Defines the property descriptors that
    truststore (and any future stdlib-protocol replicator) accesses
    via ``super(SSLContext, instance).<name>.__set__(...)``.

    truststore's ``SSLContext`` subclasses ``ssl.SSLContext`` and routes
    setter side effects through ``_original_super_SSLContext.<name>
    .__set__(self._ctx, value)``. That super-attribute lookup walks the
    MRO _after_ ``ssl.SSLContext`` — so for the descriptor to be found
    at all, it has to live on a base class, not on ``SSLContext`` itself.

    CPython's stdlib ``SSLContext`` is ``class SSLContext(_SSLContext)``
    where ``_SSLContext`` is the C-extension type that owns the
    getset descriptors. We mirror that layering."""

    @property
    def verify_mode(self) -> int:
        return self._inner.verify_mode

    @verify_mode.setter
    def verify_mode(self, value: int) -> None:
        self._inner.verify_mode = value

    @property
    def options(self) -> int:
        return getattr(self, "_options", 0)

    @options.setter
    def options(self, value: int) -> None:
        self._options = int(value)

    @property
    def verify_flags(self) -> int:
        return getattr(self, "_verify_flags", 0)

    @verify_flags.setter
    def verify_flags(self, value: int) -> None:
        self._verify_flags = int(value)

    @property
    def maximum_version(self):
        return getattr(self, "_maximum_version", None)

    @maximum_version.setter
    def maximum_version(self, value) -> None:
        self._maximum_version = value

    @property
    def minimum_version(self):
        return getattr(self, "_minimum_version", None)

    @minimum_version.setter
    def minimum_version(self, value) -> None:
        self._minimum_version = value


class SSLContext(_SSLContextDescriptors):
    """Configuration for a TLS connection. Mirrors ``ssl.SSLContext`` for the
    methods/properties most TLS users actually touch."""

    def __init__(self, protocol: int = PROTOCOL_TLS_CLIENT):
        self._inner = _ssl_capability._SSLContext(protocol)
        # Stdlib-compat attributes callers (urllib, http.client, requests)
        # inspect or set. v1 either honors them where possible or ignores
        # them (with intent) — none affect TLS correctness.
        self.check_hostname        = True
        self.post_handshake_auth   = None   # TLS 1.3 post-handshake mTLS — out of v1 scope
        # `options`, `verify_flags`, `maximum_version`, `minimum_version`
        # live on _SSLContextDescriptors as properties (so truststore's
        # super().<name>.__set__() lookup succeeds). Init the underlying
        # storage to the same defaults the prior direct-attribute form
        # exposed.
        self._options              = 0      # opaque option bitmask; no-op in v1
        self._verify_flags         = 0      # X509 verify-flags bitmask; advisory
        self._maximum_version      = None   # capability uses TLSv1.3 default
        self._minimum_version      = None   # capability uses TLSv1.2 default
        self.num_tickets           = 0
        self.session_stats         = lambda: {}     # stdlib returns a dict
        self.set_ciphers           = lambda c: None # accept ssl.py's cipher str
        self.set_alpn_protocols    = lambda p: None # also via SSLSocket; ctx-level no-op
        self.set_ecdh_curve        = lambda c: None

    # --- trust / cert material ---
    def load_default_certs(self, purpose=Purpose.SERVER_AUTH) -> None:
        """Install bundled Mozilla WebPKI roots. `purpose` is accepted for
        API parity (v1 ignores it; SERVER_AUTH and CLIENT_AUTH use the same
        bundle)."""
        self._inner.load_default_certs()

    def set_default_verify_paths(self) -> None:
        """Stdlib equivalent loads OPENSSL's compiled-in cafile/capath.
        We have no OS trust path; load our bundled Mozilla WebPKI roots
        instead. truststore's _configure_context (pip vendors it)
        calls this when get_default_verify_paths() reports a usable
        location — keeping both methods consistent lets the truststore
        path succeed without --use-deprecated=legacy-certs."""
        self._inner.load_default_certs()

    def load_verify_locations(self, cafile=None, capath=None, cadata=None) -> None:
        """Loads CA roots from a path (cafile/capath) or bytes (cadata).

        Wasmtime CLI gives us a mounted filesystem, so we read cafile/capath
        contents in Python and forward as bytes to the cap. Browser lane
        without a mounted FS will hit OSError on the open — caller should
        pass cadata directly there.
        """
        bundle = b""
        if cafile is not None:
            with open(cafile, "rb") as f:
                bundle += f.read()
            if not bundle.endswith(b"\n"):
                bundle += b"\n"
        if capath is not None:
            import os
            for name in sorted(os.listdir(capath)):
                if name.endswith((".pem", ".crt", ".cert")):
                    with open(os.path.join(capath, name), "rb") as f:
                        chunk = f.read()
                    bundle += chunk
                    if not chunk.endswith(b"\n"):
                        bundle += b"\n"
        if cadata is not None:
            if isinstance(cadata, str):
                cadata = cadata.encode("ascii")
            bundle += cadata
        if not bundle:
            raise TypeError("must pass cafile=, capath=, or cadata=")
        self._inner.set_ca_certs(bundle)

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
                    server_side=False,
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
        # session may be an SSLSession wrapper or raw bytes (forward-compat
        # with callers that stashed `prev.session_ticket()` directly).
        session_bytes = None
        if session is not None:
            session_bytes = session._ticket if isinstance(session, SSLSession) else bytes(session)
        if session_bytes:
            inner = self._inner.wrap_socket(host, int(port),
                                             server_hostname=server_hostname,
                                             session=session_bytes)
        else:
            inner = self._inner.wrap_socket(host, int(port),
                                             server_hostname=server_hostname)
        return SSLSocket(inner, context=self)

    # --- memory-BIO mode (async TLS) ---
    def wrap_bio(self, incoming, outgoing, *,
                 server_side=False, server_hostname=None,
                 session=None) -> "SSLObject":
        """Memory-BIO TLS — the caller owns the transport.
        Matches ``ssl.SSLContext.wrap_bio`` for anyio / httpx async TLS.
        Returns an ``SSLObject`` that drives the cap-side TLS state
        machine; the caller pumps ciphertext between the network and
        ``incoming``/``outgoing`` MemoryBIOs."""
        if session is not None:
            raise NotImplementedError("wrap_bio: session resumption not supported")
        cap = self._inner.wrap_bio(incoming, outgoing,
                                    server_side=int(bool(server_side)),
                                    server_hostname=server_hostname)
        return SSLObject(cap, incoming, outgoing, server_hostname)


class SSLObject:
    """Memory-BIO TLS connection. Same method surface as
    ``ssl.SSLObject``. The caller drives I/O via two MemoryBIOs:
    write ciphertext received from the peer into ``_incoming``, read
    ciphertext to send to the peer from ``_outgoing``. Plaintext
    reads/writes go through ``read()`` / ``write()``."""

    def __init__(self, cap, incoming, outgoing, server_hostname):
        self._cap = cap
        self._incoming = incoming
        self._outgoing = outgoing
        self.server_hostname = server_hostname

    def _push_in(self) -> None:
        """Drain the user's in-BIO into the cap."""
        # ssl.MemoryBIO.read() drains the buffer; the cap's BIO is the
        # actual ingress queue, so this is a one-way pump.
        if getattr(self._incoming, "pending", 0):
            data = self._incoming.read()
            if data:
                self._cap.bio_write(data)

    def _pull_out(self) -> None:
        """Drain the cap's out-BIO into the user's out-BIO."""
        while self._cap.bio_pending() > 0:
            data = self._cap.bio_read(16384)
            if not data:
                break
            self._outgoing.write(data)

    def _drive(self, fn, *args):
        """Push in-BIO bytes, call the cap method, pull out-BIO bytes,
        re-raise SSL_ERROR_WANT_* as the typed stdlib exceptions."""
        self._push_in()
        try:
            result = fn(*args)
        except SSLError as e:
            # Always drain outgoing even on error: the TLS layer may
            # have queued bytes (e.g. handshake's ClientHello) before
            # raising WANT_READ.
            self._pull_out()
            raise _map_ssl_want(e) from None
        self._pull_out()
        return result

    def do_handshake(self) -> None:
        self._drive(self._cap.do_handshake)

    def read(self, len_=1024, buffer=None) -> bytes:
        # CPython's SSLObject.read takes (len, buffer=None); when buffer
        # is given, fill it and return number of bytes written.
        data = self._drive(self._cap.read, int(len_))
        if buffer is None:
            return data
        n = len(data)
        buffer[:n] = data
        return n

    def write(self, data) -> int:
        return self._drive(self._cap.write, bytes(data))

    def unwrap(self):
        """Initiate close_notify; returns None (stdlib returns the wrapped
        socket, which we never owned)."""
        try:
            self._drive(self._cap.shutdown)
        except SSLError:
            pass
        return None

    def pending(self) -> int:
        # Plaintext bytes the SSL has already decrypted but the caller
        # hasn't read. The cap doesn't expose a pending-plaintext probe
        # for memory BIOs (the read() returns whatever's available);
        # report 0 — matches the conservative path for ssl.SSLObject
        # callers that fall back to read() when this is 0.
        return 0

    def getpeercert(self, binary_form=False):
        der = self._cap.peer_cert_der()
        if der is None:
            return None if binary_form else {}
        if binary_form:
            return der
        # Real parsed dict via the cap's x509.certificate.info() — falls
        # back to the synthetic dict if the cap can't parse (e.g. very
        # old peers, or post-handshake cert eviction).
        try:
            info = self._cap.peer_cert_info()
        except AttributeError:
            info = None
        if info:
            return _format_peer_cert(info)
        host = self.server_hostname or ""
        return {
            "subjectAltName": (("DNS", host),) if host else (),
            "subject":        ((("commonName", host),),) if host else (),
            "issuer":         ((("commonName", "python-wasm ssl_capability"),),),
        }

    def version(self) -> str:
        return self._cap.version()

    def cipher(self):
        # Forward to the cap (mem-bio-client.peer post-handshake).
        # Stdlib shape: (name, protocol_version, secret_bits).
        return self._cap.cipher()

    def selected_alpn_protocol(self):
        return self._cap.selected_alpn_protocol()

    def get_unverified_chain(self):
        """Same shape as SSLSocket.get_unverified_chain — see that
        docstring. Returns the full peer chain (leaf first); falls
        back to the leaf-only list if the cap returns an empty chain
        for any reason."""
        chain = self._cap.peer_chain_der()
        if not chain:
            der = self._cap.peer_cert_der()
            return [der] if der else []
        return chain

    def session_reused(self) -> bool:
        return False

    def get_channel_binding(self, cb_type="tls-unique"):
        # Channel binding requires SSL_get_tls_unique / SSL_get_finished
        # which aren't exposed by openssl-component yet. Return None;
        # anyio/httpx tolerate it (only matters for SCRAM-SHA auth).
        return None

    def compression(self):
        # TLS-level compression has been removed in modern TLS (CRIME
        # mitigation). Return None — stdlib returns None when no
        # compression is in effect.
        return None

    def shared_ciphers(self):
        return None


class SSLSocket:
    """An open TLS connection. Wraps ``_ssl_capability._SSLSocket``; method
    names match stdlib ``ssl.SSLSocket`` for drop-in use."""

    def __init__(self, inner, context=None):
        self._inner = inner
        # httpx/httpcore reach for `_sslobj` (the stdlib's internal _ssl
        # C-extension SSL object) to introspect the connection. We don't
        # expose a separate C-level object — the inner cap IS that level —
        # so present self as the sslobj.
        self._sslobj = self
        # truststore (and any future verify-impl) expects an SSLSocket to
        # carry a `.context` back-reference to the SSLContext that made
        # it. Optional in case a future code path constructs SSLSocket
        # without going through wrap_socket().
        self.context = context
        # Internal buffer for excess bytes the drain loop pulled past the
        # caller's buflen. Served on the next read() before going back to
        # the cap.
        self._read_buf = b""

    # --- I/O ---
    def read(self, buflen: int = 8192) -> bytes:
        # Serve any bytes the previous drain loop pulled past the
        # caller's buflen first. Buffer is only populated when the
        # tail-record speculative drain returned more than buflen
        # — common case is buf is empty and we go straight to inner.read.
        if self._read_buf:
            if len(self._read_buf) >= buflen:
                out, self._read_buf = self._read_buf[:buflen], self._read_buf[buflen:]
                return out
            out, self._read_buf = self._read_buf, b""
            buflen -= len(out)
        else:
            out = b""

        # openssl-component@0.2.x exposes two non-blocking probes:
        #   * pending()         — wraps SSL_has_pending; true when OpenSSL
        #                          has buffered data (decrypted plaintext
        #                          OR unprocessed ciphertext in its BIO).
        #   * socket_readable() — non-blocking POSIX poll(0) on
        #                          SSL_get_fd; true when the kernel TCP
        #                          buffer has bytes OpenSSL hasn't pulled
        #                          into the BIO yet.
        # The drain loop uses both: pending() catches the partial-record
        # case where OpenSSL has already pulled bytes, socket_readable
        # catches the trailing-record case where a record arrived on the
        # wire after the first read returned (the urllib chunked +
        # Connection: close → IncompleteRead bug).
        #
        # Connection: keep-alive + idle server: both probes return false
        # → loop exits → no block waiting on bytes the server isn't
        # sending until the next request.
        try:
            data = self._inner.read(buflen)
        except SSLError as e:
            msg = str(e).lower()
            if "connection is closed" in msg or "zero return" in msg:
                return out  # may have leftover-buffer bytes to deliver
            raise

        if not data:
            return out + data  # leftover (if any) + natural EOF

        # Cheap drain: pull anything OpenSSL has already buffered. This
        # never blocks on the network — pending() reflects only the BIO.
        while self._inner.pending() > 0:
            try:
                extra = self._inner.read(buflen)
            except SSLError as e:
                msg = str(e).lower()
                if "connection is closed" in msg or "zero return" in msg:
                    break
                e._drained = data  # type: ignore[attr-defined]
                raise
            if not extra:
                break
            data += extra

        # NOTE: openssl-component@0.2.x also exposes socket_readable
        # (non-blocking poll on the underlying TCP fd). We tried wiring
        # it in as a speculative drain — fixes urllib + chunked-encoding
        # + Connection: close (the chunk-end sentinel `0\r\n\r\n` ships
        # in a separate small record after the body, gets lost when
        # close-notify is processed before we re-enter SSL_read). But
        # speculative socket-readable drains broke pip's urllib3 path:
        # it reads many small records and expects each read to bound at
        # one record, not aggressively pull until the socket goes quiet.
        # Deferred until the drain heuristic is refined enough to tell
        # "tail record before close" apart from "next chunk of a bulk
        # response."

        # Honor the caller's buflen contract: never return more than
        # they asked for. Park any excess for the next read().
        full = out + data
        room = len(out) + buflen   # original caller buflen, accounting for what we already prepended
        if len(full) > room:
            self._read_buf = full[room:]
            full = full[:room]
        return full

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

    # Socket-shape stubs needed by urllib3 / requests. The cap owns the
    # underlying TCP socket; these are no-ops at the Python wrapper layer.
    def settimeout(self, value) -> None:  # noqa: D401
        self._timeout = value
    def gettimeout(self):
        return getattr(self, "_timeout", None)
    def setblocking(self, flag: bool) -> None:
        pass
    def fileno(self) -> int:
        # Real underlying TCP fd from openssl-component (SSL_get_fd).
        # Pollable via select.poll / select.select — httpx/httpcore call
        # this for connection-alive checks. Falls back to a synthetic
        # id when the inner cap doesn't expose the method (older
        # openssl-component or detached state).
        try:
            fd = self._inner.fileno()
            if fd >= 0:
                return fd
        except (AttributeError, OSError):
            pass
        return id(self) & 0x7fffffff
    def getpeername(self):
        host = self._inner.server_hostname or ""
        return (host, 0)
    def getsockname(self):
        return ("0.0.0.0", 0)
    def setsockopt(self, *args, **kwargs):
        pass

    def shutdown(self, how=None) -> None:
        # Stdlib takes a SHUT_RD/WR/RDWR arg; we collapse to full shutdown.
        self._inner.shutdown()

    def close(self) -> None:
        # Stdlib ssl.SSLSocket.close decrements the underlying socket ref
        # rather than synchronously tearing down the TLS state — the actual
        # close happens when the last reference is dropped. http.client
        # relies on this: when it sees Connection: close it calls
        # self.sock.close() right after handing the response file object
        # to the user, expecting the response.fp.read() loop to keep working
        # against whatever bytes are still in flight. If close() actually
        # destroyed the TLS state here, the body would truncate (this was
        # the urllib chunked + Connection: close → IncompleteRead bug).
        # Defer the real cap teardown to dealloc: when nothing references
        # this SSLSocket (or its _SocketReader child) anymore, the inner
        # cap handle's dealloc closes the openssl-component client.
        pass

    # --- introspection ---
    def version(self) -> str:
        return self._inner.version()

    def cipher(self) -> tuple:
        return self._inner.cipher()

    def selected_alpn_protocol(self):
        return self._inner.selected_alpn_protocol()

    def getpeercert(self, binary_form: bool = False):
        """Return the peer's certificate.

        binary_form=True returns DER bytes (the stdlib API). binary_form=False
        returns a parsed dict built from the cap's
        ``x509.certificate.info()`` — subject/issuer/version/serialNumber/
        notBefore/notAfter/subjectAltName populated; signature/key-usage
        fields omitted because no stdlib caller reads them. Falls back to
        the synthetic single-SAN dict when the cap can't parse (very rare
        — usually means the peer sent no cert)."""
        der = self._inner.peer_cert_der()
        if der is None:
            return {} if not binary_form else None
        if binary_form:
            return der
        try:
            info = self._inner.peer_cert_info()
        except AttributeError:
            info = None
        if info:
            return _format_peer_cert(info)
        host = self._inner.server_hostname or ""
        return {
            "subjectAltName": (("DNS", host),) if host else (),
            "subject":        ((("commonName", host),),) if host else (),
            "issuer":         ((("commonName", "python-wasm ssl_capability"),),),
        }

    def get_unverified_chain(self):
        """Stdlib API added in 3.13: full peer chain as DER bytes (leaf
        first, then intermediates the peer sent). Implemented via the
        cap's `peer_chain_der` helper. truststore's `_verify_peercerts`
        walks `sslobj._sslobj` in a `while not hasattr(sslobj,
        'get_unverified_chain')` loop; since our `_sslobj` is `self`
        for httpx-compat, that loop would spin forever without this
        method."""
        chain = self._inner.peer_chain_der()
        if not chain:
            der = self._inner.peer_cert_der()
            return [der] if der else []
        return chain

    @property
    def session(self) -> "SSLSession | None":
        """Stdlib API: an opaque SSLSession the caller can stash and
        pass back via ``wrap_socket(..., session=...)`` to resume on
        the next connect. Returns None if the server didn't issue a
        ticket (e.g. TLS 1.2 without tickets, or session-tickets
        disabled)."""
        try:
            ticket = self._inner.session_ticket()
        except AttributeError:
            return None
        return SSLSession(ticket) if ticket else None

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
        # Empty-on-first-try usually means EOF, but for HTTP responses
        # with Connection: close + chunked-encoding the trailer record
        # (`0\r\n\r\n`) can be in flight while the body record was
        # already read. The cap's socket_readable() check pokes the
        # kernel buffer non-blockingly; if it says data is ready, give
        # one more read a chance before reporting EOF. Avoids
        # http.client's IncompleteRead at the body trailer without
        # affecting bulk reads (those never reach this branch since
        # they return data on the first try).
        if not data:
            inner = self._sock._inner
            try:
                if inner.socket_readable():
                    data = self._sock.read(len(buf))
            except AttributeError:
                pass
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
# C ext currently returns a string; stdlib spec is (major, minor, fix, patch, status).
# Derive from OPENSSL_VERSION_NUMBER (packed as MNNFFPPS). PyPI urllib3 etc.
# compare this as a tuple.
_v = _ssl_capability.OPENSSL_VERSION_NUMBER
OPENSSL_VERSION_INFO    = (
    (_v >> 28) & 0xF,
    (_v >> 20) & 0xFF,
    (_v >> 12) & 0xFF,
    (_v >>  4) & 0xFF,
    _v & 0xF,
)
del _v

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

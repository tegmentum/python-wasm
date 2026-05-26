# HTTPS in the browser Python interpreter

A constraint guide for users who want to run TLS-using Python code (e.g.
`urllib.request.urlopen('https://...')`, `requests`, anything that opens a
socket to a remote host) inside the browser python interpreter.

If you're running the interpreter under `wasmtime`, you can stop reading —
wasmtime has real TCP via `wasi:sockets/tcp`, and `make test-ssl-network`
exercises a real HTTPS round trip end-to-end.

## The constraint

Browsers don't expose raw TCP. They expose **WebSocket** and **fetch**. So
when python-wasm runs in the browser, its `wasi:sockets/tcp` import — which
`openssl-component`'s TLS uses internally — can't be satisfied directly.

This is a browser fundamental, not a python-wasm issue: the same applies
to any wasm component (or any web app) that wants outbound TCP. The
universal workaround is **tunneling TCP over a WebSocket** to a server
("gateway") on the network that does the real TCP for you.

```
  python.wasm  (in the browser)
       │
       │ wasi:sockets/tcp  ←──── @tegmentum/wasi-polyfill ws-gateway plugin
       │
       │     framed as KSW1 protocol over a single WebSocket
       ▼
  wss://your-gateway.example/ws    (a server YOU run)
       │
       │ real TCP
       ▼
  example.com:443                  (the TLS endpoint)
```

The TLS handshake itself runs **inside the browser** (in
`openssl-component`'s wasm). The gateway is purely a byte forwarder; it
never sees plaintext, never has your private keys, and can't MITM the
connection because the cert validation runs in your Python process against
the bundled Mozilla WebPKI roots.

## Configuring the python-wasm web demo

The web runner reads the gateway URL from a Vite environment variable:

```sh
# .env.local (gitignored)
VITE_TCP_GATEWAY_URL=wss://my-gateway.example/ws
VITE_TCP_GATEWAY_TOKEN=optional-auth-token   # passed in WebSocket headers
```

Then:

```sh
make web-build       # bakes the URL into the bundle
# or
make web-dev         # hot-reload dev server
```

If `VITE_TCP_GATEWAY_URL` is not set, the polyfill registers virtual sockets
that return `NotSupported` for any TCP op. Code that doesn't open network
connections still works; code that tries `urlopen('https://...')` will fail
with an SSLError at handshake time.

## Running the reference gateway

`wasi-polyfill` ships a reference gateway under `examples/tcp-proxy/` — a
small Node script that implements the KSW1 protocol and bridges to real
TCP. Run it locally for development:

```sh
cd ~/git/wasi-polyfill/examples/tcp-proxy   # if it exists
node proxy.js --port 8443
# then VITE_TCP_GATEWAY_URL=ws://localhost:8443/ws
```

**For production:**

- Run your own gateway. Pin **`Origin`** to your domain in the WebSocket
  handshake; reject requests from anywhere else.
- Allowlist destinations. A wss-gateway with no allowlist is a tunneling
  primitive; anyone who can connect to your gateway can connect to any TCP
  endpoint. Default the reference gateway to **deny-all** and require
  explicit `--allow host:port` rules for production.
- Terminate the WebSocket over **TLS** (`wss://`, not `ws://`) so the
  gateway hop itself is encrypted. The end-to-end TLS to the destination
  is separate and unrelated.

## Using TLS from Python

Today the path is opt-in via `ssl_capability` (Phase 3b.6 of the
componentize-python plan; Phase 5 collapses it into the standard `ssl`
module):

```python
import ssl_capability, ssl
ssl.SSLContext = ssl_capability.SSLContext
ssl._create_default_https_context = ssl_capability.create_default_context

import urllib.request
print(urllib.request.urlopen('https://example.com').read(200))
```

Or directly:

```python
import ssl_capability as ssl
ctx = ssl.create_default_context()    # CERT_REQUIRED + bundled WebPKI roots
sock = ctx.wrap_socket(host='example.com', port=443,
                       server_hostname='example.com')
sock.sendall(b'GET / HTTP/1.0\r\nHost: example.com\r\n\r\n')
print(sock.read(2048))
sock.close()
```

## What works, what doesn't

| | Status |
|---|---|
| `urllib.request.urlopen('https://...')` | ✅ via the monkey-patch above |
| `ssl.create_default_context()` | ✅ as `ssl_capability.create_default_context()` |
| CERT_REQUIRED + 121 Mozilla WebPKI roots | ✅ `ctx.load_default_certs()` |
| Reject expired / self-signed certs | ✅ verified in `make test-ssl-network` |
| Client certificates (mTLS) | ✅ `ctx.load_cert_chain(certdata=, keydata=)` (bytes-only) |
| ALPN | ✅ `ctx.set_alpn_protocols(['h2','http/1.1'])` |
| `socket.socket()` | ❌ raises `NotSupported` (browsers don't have raw TCP) |
| `ssl.wrap_socket(socket_obj, ...)` | ⚠ accepts a socket but closes it; openssl-component opens its own internally |
| `requests` library | ❌ not bundled; needs to be added to the stdlib bundle |
| `asyncio` SSL (StartTLS / loop.start_tls) | ❌ uses MemoryBIO which openssl-component doesn't support yet (v1.1) |
| Session resumption | ❌ deferred to v1.1 |
| Channel binding (HTTP/2 client auth, SCRAM-PLUS) | ❌ openssl-component doesn't expose `SSL_get_finished` yet |

## Why not just use `fetch`?

Two reasons:

1. **`fetch` is a higher-level abstraction.** It does TLS for you, but it
   also opaquely handles headers, cookies, redirects, encoding. Python
   code that calls `urllib.request.urlopen` expects bytes flowing through a
   socket; bridging that to `fetch` means lying about what's happening
   (cookies handled by the browser, not Python's `http.cookiejar`; CORS
   blocking responses Python expected to receive; etc.).

2. **Static analysis of what the Python code does.** TLS in the wasm means
   the Python process owns the connection, certificate validation, ALPN
   choice, etc. The browser is just providing the transport, the same way
   any wasi-p2 runtime would.

If you only need to fetch a URL and don't care about Python's networking
contract, you can absolutely just call `fetch` from a small bit of JS glue
and hand the bytes to Python. The componentize-python TLS path is for code
that wants the real Python networking semantics.

## Future work (not in v1)

- An `httpx`/`requests` shim that bridges to `fetch` (when the Python code
  doesn't need socket-level TLS).
- `asyncio` integration once openssl-component grows memory-BIO support
  (upstream PR).
- Phase 5 of the componentize-python plan retires the static `_ssl`
  altogether; after that, `import ssl` Just Works without the monkey-patch.

Reference: see `docs/phase-3-tls.md` for the full TLS architecture.

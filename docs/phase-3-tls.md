# Phase 3 — TLS capability + `_ssl` for python.wasm

A comprehensive plan for the deferred phase of
[docs/componentize-python.md](componentize-python.md). Replaces CPython's
static OpenSSL `_ssl` with a `_ssl` C extension that imports a
`tegmentum:tls` capability component, following the same pattern as
`_compression` (Phase 1) and `_crypto_hash`/`_xxhash` (Phase 2).

**Goal:** retire the OpenSSL static link in CPython by replacing `_ssl` with
a CPython C extension that imports a `tegmentum:tls` capability component,
the same way `_compression` imports compression-multiplexer.

**Non-goals:** building OpenSSL from scratch (we reuse `~/git/openssl-wasm`'s
static libs as the engine for the new component); supporting every `_ssl`
symbol exhaustively in v1 (we cover what `ssl.py` actually calls); adding
browser raw-TCP sockets (the polyfill side is its own story).

---

## 0. Why this is one of the harder phases

Three things that are different from Phase 1/2:

1. **TLS is stateful.** Compression had per-call inputs; hashing had buffered
   inputs. TLS is a per-connection state machine with a handshake, a
   bidirectional record stream, and a tear-down. The capability has to expose
   that as a long-lived **resource** with explicit data-in / data-out methods.
2. **TLS needs transport.** `SSL_read`/`SSL_write` don't actually do I/O —
   they call back into the host via a BIO that reads/writes the underlying
   socket. In wasm we can't hand OpenSSL a socket; we have to operate it via
   the **memory-BIO pattern** (give it an in-buffer to read from + an
   out-buffer to read from). That's how the rest of the wasm TLS world does
   it (boringtun, rustls in wasm, hyper-rustls, etc.).
3. **Transport is a separate capability.** Once TLS is BIO-driven, the caller
   has to pump bytes between the TLS state machine and a TCP socket. The "TCP
   socket" can be `wasi:sockets/tcp` (works in wasmtime, not in the browser
   today), or a custom transport capability the browser hosts implement (a
   thin shim over WebTransport or fetch streams). v1 ships the **TLS state
   machine** as a clean capability; transport is decoupled.

Acknowledging this up front because it changes the shape of the deliverables
vs. Phase 1/2's "one capability + one extension" pattern.

---

## 1. Target architecture

```
                                                      (CPython, Phase 3b)
                                                      ┌─────────────────────┐
                                                      │ Modules/_ssl.c      │  wit-bindgen-c
  ssl.py (stdlib, ~unchanged)                         │   (rewritten)       │  generated
       │                                              └──────────┬──────────┘  bindings
       │ import _ssl                                              │
       ▼                                                          ▼
  _ssl.SSLContext / SSLObject  ────►  tegmentum:tls/context (WIT) ── imported
                                                                  │
                                                                  │ composed in
                                                                  ▼
                  ┌──────────────────────────────────────────────────────────┐
                  │  tls-wasm  (Phase 3a)  — a tegmentum capability component│
                  │                                                          │
                  │     Rust component + wit-bindgen-rt                      │
                  │     wraps openssl-wasm's libssl.a + libcrypto.a          │
                  │     uses BIO_s_mem() for ALL I/O                         │
                  │                                                          │
                  │   exports:  tegmentum:tls/context                        │
                  │   imports:  wasi:random (handshake nonces, no sockets!)  │
                  └──────────────────────────────────────────────────────────┘
                                                                  │
                                                                  │ TCP / transport
                                                                  │ is the CALLER's
                                                                  │ responsibility
                                                                  ▼
                  Phase 3c (separate, lightweight wiring inside _ssl.c):
                  pump bytes between tls.context's in/out buffers and
                  the underlying socket — wasi:sockets/tcp under wasmtime;
                  a host-implemented transport under jco+wasi-polyfill.
```

Three boxes, three phases — each independently shippable.

---

## 2. WIT design for `tegmentum:tls`

This is the load-bearing decision. Get this wrong and we redo a lot. The
pattern is borrowed from how rustls and ring's TLS bindings expose themselves
to non-blocking hosts.

```wit
package tegmentum:tls@0.1.0;

interface context {
    /// A TLS connection, client-side. Memory-BIO based: the caller pumps bytes
    /// between this and the underlying transport. Drop = teardown (no
    /// notify_close TLS alert; call `close-notify` first if you want one).
    resource client {
        /// Construct a TLS client targeting `server-name` (used for SNI and
        /// certificate verification). `alpn` is the offered ALPN list (may be
        /// empty). `ca-roots` is PEM-encoded root certificates the client will
        /// trust (empty = use the bundled WebPKI roots from the capability).
        constructor(
            server-name: string,
            alpn: list<string>,
            ca-roots: option<list<u8>>,
        );

        /// Push bytes received from the network into the TLS state machine.
        /// Returns the number of bytes consumed (may be < `bytes.len`).
        push-tls-input: func(bytes: list<u8>) -> result<u64, tls-error>;

        /// Pull TLS bytes that should be sent on the wire. Returns up to `max`
        /// bytes; empty list = nothing to send right now.
        pull-tls-output: func(max: u64) -> list<u8>;

        /// Push plaintext from the application to be encrypted and emitted as
        /// TLS records (retrieved via pull-tls-output).
        write-plaintext: func(bytes: list<u8>) -> result<u64, tls-error>;

        /// Decrypt and return up to `max` bytes of received plaintext.
        read-plaintext: func(max: u64) -> result<list<u8>, tls-error>;

        /// Where in the lifecycle the connection is.
        state: func() -> connection-state;

        /// Convenience for the handshake loop:
        ///   loop {
        ///       drain pull-tls-output to socket
        ///       if handshake-complete, break
        ///       feed socket bytes into push-tls-input
        ///   }
        /// Returns true when handshake is complete (state == established).
        handshake-complete: func() -> bool;

        /// Send a `close_notify` TLS alert; subsequent pull-tls-output will
        /// emit it. Caller should then close the underlying transport.
        close-notify: func() -> result<_, tls-error>;

        /// Peer certificate as DER bytes (after handshake). Empty if not yet
        /// available or peer sent none (won't happen for a valid TLS server).
        peer-cert-der: func() -> option<list<u8>>;

        /// Negotiated ALPN protocol, "" if none.
        alpn-selected: func() -> string;

        /// Negotiated TLS version, e.g. "TLSv1.3".
        version: func() -> string;
    }

    enum connection-state {
        handshaking,
        established,
        closing,         // close_notify sent, waiting for peer's
        closed,
    }

    variant tls-error {
        would-block,                 // need more input or output drain
        cert-verify-failed(string),  // CN/SAN/chain failure with detail
        handshake-failed(string),    // protocol-level handshake error
        protocol(string),            // mid-connection protocol error
        closed-by-peer,              // saw close_notify (graceful)
        io(string),                  // unexpected internal failure
    }
}

interface server {
    /// Same shape, server side. Server identity comes from a PEM cert + key.
    resource handle {
        constructor(
            cert-chain-pem: list<u8>,
            private-key-pem: list<u8>,
            alpn: list<string>,
            require-client-cert: bool,
        );
        push-tls-input: func(bytes: list<u8>) -> result<u64, tls-error>;
        pull-tls-output: func(max: u64) -> list<u8>;
        write-plaintext: func(bytes: list<u8>) -> result<u64, tls-error>;
        read-plaintext: func(max: u64) -> result<list<u8>, tls-error>;
        state: func() -> connection-state;
        handshake-complete: func() -> bool;
        close-notify: func() -> result<_, tls-error>;
        peer-cert-der: func() -> option<list<u8>>;
        alpn-selected: func() -> string;
        version: func() -> string;
    }
    use context.{connection-state, tls-error};
}

world tls {
    export context;
    export server;
}
```

### Key design choices

- **No sockets in the capability.** TLS does not own I/O. Net wins: works
  under jco+wasi-polyfill (no raw TCP), works under wasmtime, works under
  any future transport.
- **Memory-BIO semantics, exposed plainly.** The "push/pull" terminology
  mirrors rustls's `Connection::process_new_packets` / `read_tls` /
  `write_tls` so a reader who knows rustls will recognize the shape
  immediately.
- **One resource per side, lifecycle methods on the resource.** Not separate
  "context" + "session" types — context-per-connection works fine and avoids
  an extra resource lifetime.
- **`tls-error` as a variant**, not just a string. `would-block` is critical
  because that's the signal to go pump the socket; the caller has to
  distinguish it from a real failure.
- **CA roots as input, not baked in.** With an empty list, the capability
  falls back to bundled WebPKI roots (Mozilla CA bundle, ~250 KB embedded).
  This is the same default Python's `ssl.create_default_context()` produces.
- **Server side is optional in v1.** Listed for completeness but the
  bring-up plan ships client first; `handle` can land in v1.1.

### WIT review checkpoint

Before any code is written, this WIT is reviewed against:

1. **Python's `ssl.SSLObject` API surface** — every method `ssl.py` calls into
   `_ssl` must map to *something* in this WIT. Walk through `Lib/ssl.py`'s
   `_SSLContext`, `_create_socket`, `wrap_socket`, `do_handshake`, `read`,
   `write`, `shutdown` to verify.
2. **rustls's public API** — sanity check that a rustls-based implementation
   can sit behind this WIT without bending the interface.
3. **OpenSSL's BIO + SSL_* function set** — sanity check the same for an
   openssl-backed implementation.

If any of those three reveals a needed method we forgot, the WIT changes
*here*, before we start writing C or Rust.

---

## 3. Phases

### Phase 3a — `tls-wasm` capability component

**Repo:** new `~/git/tls-wasm`. The Rust template mirrors the existing
`~/git/compression-multiplexer` shape (a `tegmentum:*` capability component
built with `cargo component`).

**Internals:** wrap `openssl-wasm`'s prebuilt `libssl.a` + `libcrypto.a` via
Rust FFI. The Rust component code itself is small — most of the work is
correctly driving OpenSSL's memory BIOs and translating its errors into the
`tls-error` variant.

**Sub-deliverables (in order):**

| # | Deliverable | Acceptance |
|---|---|---|
| **3a.1** | Repo scaffold (`Cargo.toml` with `crate-type = ["cdylib"]`, wit/world.wit imports tegmentum:tls, links libssl.a + libcrypto.a from a sibling `~/git/openssl-wasm` checkout) | `cargo component build --target wasm32-wasip2` produces a wasm. WIT introspection shows `export tegmentum:tls/context`. |
| **3a.2** | Implement `context.client::constructor`: build SSL_CTX (TLS 1.2/1.3, peer-verify on), create SSL, set SNI, set ALPN, attach memory BIOs. No actual connection yet. | Native unit test: construct + verify SSL_get_servername returns the SNI. |
| **3a.3** | Implement `push-tls-input` / `pull-tls-output` / `handshake-complete`. This is where the BIO pump lives. | Native unit test against an in-process libssl server (the openssl-wasm artifacts include `s_server` equivalent or we vendor a minimal one): both sides handshake, ALPN agrees. |
| **3a.4** | Implement `write-plaintext` / `read-plaintext` / `close-notify` / `peer-cert-der` / `version` / `alpn-selected`. | Same self-loop test: send "ping", receive "ping", `peer-cert-der()` returns the server's DER, `version()` == "TLSv1.3". |
| **3a.5** | Implement `server::handle` (same shape, server-side). Optional for v1 but cheap to add since we already have the BIO plumbing. | Self-loop test passes with our component on both sides. |
| **3a.6** | Bundle Mozilla WebPKI roots (compile-time `include_bytes!` of a fetched bundle) and use them when `ca-roots` is empty. | A real handshake test against `tls13.akamai.com:443` or similar succeeds. (Needs a real TCP, so this test runs under wasmtime with `wasi:sockets/tcp`, not jco.) |
| **3a.7** | jco-instantiation smoke: feed canned record bytes (recorded from a real handshake) into the component, verify it produces the expected output bytes. | Cross-checks that the component runs cleanly under jco even though we can't do real TCP there. |

**Estimated size of the component:** OpenSSL static libs are huge but already
needed for current `_ssl` — the *additional* size for the component wrapping
is ~200 KB of Rust glue + ~250 KB of CA roots = under 500 KB on top of what
we already ship. Total tls-wasm should land around the size of the current
static OpenSSL link (~3 MB).

**Risks:**

- **R1: BIO pumping has gotchas.** `SSL_read`/`SSL_write` return
  `SSL_ERROR_WANT_READ` even when there's nothing in the input BIO; the loop
  has to handle every combination. Mitigation: follow rustls's API contract
  closely (their state machine is well-tested) and lift their test corpus
  where applicable.
- **R2: OpenSSL randomness.** Handshake nonces come from `RAND_bytes`.
  openssl-wasm's `_RAND` needs entropy; we wire it from
  `wasi:random/random@0.2.x` via a custom `RAND_METHOD`. The crypto-hash +
  compression caps already do similar.
- **R3: openssl-wasm is no-threads.** OpenSSL has internal locks that no-op
  in this build. That's fine for single-connection use; document it.
- **R4: Cert chain validation in wasm.** OpenSSL does PKI itself; we don't
  need rustls's webpki. But the time source for "is this cert expired" comes
  from `wasi:clocks`. Confirm openssl-wasm's `time()` is wired to that and
  not zero.

**Effort:** **4–6 days** for an experienced dev once the WIT is reviewed.

---

### Phase 3b — `_ssl` C extension

**Where it lives:** `cpython-ext/_ssl/` — same shape as
`cpython-ext/_compression/` and `cpython-ext/_crypto_hash/`. Replaces
(eventually) CPython's `Modules/_ssl.c`.

**Approach: incremental Python-API surface mapping.**

CPython's `_ssl` exposes a lot (SSLContext, SSLSocket, SSLObject, error
classes, dozens of options). We don't need all of it for v1 — we need what
`ssl.py` actually drives, which is: context creation, wrap_socket, handshake
loop, read/write, shutdown, peer cert retrieval, version/cipher
introspection.

**Sub-deliverables:**

| # | Deliverable | Acceptance |
|---|---|---|
| **3b.1** | Scaffold `cpython-ext/_ssl/` (WIT vendored from tls-wasm artifact, `wit-bindgen c` output, Setup.local entry — pattern identical to _compression). | Stub module builds. `import _ssl` works. python.wasm declares `import tegmentum:tls/context`. |
| **3b.2** | Implement `_ssl.MemoryBIO` (CPython's BIO wrapper class). Phase 3a's resource gives us exactly this; just expose it. | A test creating two MemoryBIOs and pumping between them works. |
| **3b.3** | Implement the minimal `_ssl._SSLContext` + `_SSLSocket` surface: `wrap_bio`, `do_handshake`, `read`, `write`, `pending`, `shutdown`. Backed by tls-wasm's `context.client`. | Pure-Python test using `ssl.SSLObject` (BIO-based — no socket) does an end-to-end handshake against our `tls-wasm` server resource. |
| **3b.4** | Add `_ssl.RAND_bytes`, `_ssl.OPENSSL_VERSION` (string from `version()`), and a small set of constants `ssl.py` references at import time. | `import ssl` in the composed wasm doesn't AttributeError. |
| **3b.5** | Wire the socket-backed path: `_ssl._SSLSocket` over a real fd. Under wasmtime this uses `wasi:sockets/tcp`; the C extension pumps bytes between the tls resource and `recv()/send()` on the socket. | `python -c "import ssl, socket; ctx = ssl.create_default_context(); s = ctx.wrap_socket(socket.create_connection(('tls13.akamai.com', 443)), server_hostname='tls13.akamai.com'); s.send(b'GET / ...'); print(s.read(80))"` under wasmtime returns an HTTPS response. |
| **3b.6** | Re-route or proxy CPython's existing `_ssl` symbols so `import ssl` keeps working. Two-mode build: the old static `_ssl` (from build-openssl.sh) and our new `_ssl_capability` coexist; `_ssl` re-exports the latter. Phase 5 removes the static path. | `Lib/test/test_ssl.py` subset that doesn't need a real network (`test_constructor`, `test_wrap_bio_handshake`, `test_pending`, `test_shutdown`) passes. |

**Risks:**

- **R5: `_ssl` is enormous.** Real CPython `_ssl.c` is 6000+ lines. We're
  *not* reimplementing it — we're implementing what `ssl.py` uses.
  Identifying that surface up front (a `grep _ssl. Lib/ssl.py` and inventory)
  is part of 3b.1.
- **R6: Subtle behavior mismatches.** Things like "what does `read(0)`
  return", "how is partial write reported", "what's `pending()` after a
  half-closed connection." Mitigation: drive 3b's tests directly against the
  CPython `test_ssl.py` subset that doesn't need real sockets, treat any
  divergence as a v1 bug.
- **R7: Async I/O.** `ssl.SSLObject` is the non-blocking surface; `asyncio`
  uses it. Verify our BIO-pumped implementation works with `asyncio.StartTLS`
  in the browser interpreter. Probably fine because asyncio drives it through
  `BufferedTransport` and we're matching SSLObject semantics, but verify.

**Effort:** **5–7 days** for the minimum viable replacement that covers what
the browser interpreter actually does.

---

### Phase 3c — transport wiring (Browser & host)

This is where we make the composed wasm actually do TLS in the browser.

Two halves:

**3c.1 — wasmtime / CLI path.** Under wasmtime python.composed.wasm gets
`wasi:sockets/tcp@0.2.x` for free. The `_ssl` extension uses the standard
wasi-socket fd via Python's `socket` module, pumping bytes through the
tls-wasm resource. Should work as-is after 3b.5. The CI build job adds a
network-required smoke test (gated on a flag — CI runners can hit
`tls13.akamai.com`).

**3c.2 — browser path (jco + wasi-polyfill).** The browser has no raw TCP.
We need a transport shim. Two viable options:

- **Option A: wasi-polyfill implements `wasi:sockets/tcp` via WebTransport /
  WebSocket.** Pro: looks like a normal wasi socket to the Python code;
  nothing in `_ssl` changes. Con: browsers can't open arbitrary TCP —
  limited to wss:// targets that proxy TCP. Real but constrained.
- **Option B: A `tegmentum:transport/stream` capability** the browser
  polyfill implements directly, that the `_ssl` extension uses *instead of*
  wasi:sockets when wasi:sockets isn't available. Pro: explicit about
  constraints; the polyfill maps it to WebSocket / WebTransport / fetch
  streams as appropriate. Con: divergent code paths between wasmtime and
  browser; `socket.socket()` in Python wouldn't work.

Pick A. Reason: Option B makes `import socket` lie in the browser; that's
worse than telling people "browser TCP goes through a wss:// proxy." Document
the constraint clearly.

**Sub-deliverables:**

| # | Deliverable | Acceptance |
|---|---|---|
| **3c.1** | CI smoke test against a real TLS server, gated on a `network=true` flag, under wasmtime. | `make test-ssl-extension` (new target) does a real HTTPS GET. |
| **3c.2.a** | wasi-polyfill ships a `wasi:sockets/tcp` implementation backed by WebSocket-over-wss-proxy. Companion change in `~/git/wasi-polyfill`. | Browser demo can call `socket.create_connection(('example.com', 443))` via the proxy. |
| **3c.2.b** | Document the browser TLS constraint clearly in `docs/componentize-python.md` and a new `docs/browser-tls.md`. | The constraint is unambiguous to a new user. |

**Risks:**

- **R8: WebSocket-as-TCP-proxy.** Requires an actual proxy server. We don't
  run one. Document it as user-deployable; ship a tiny reference proxy as
  part of wasi-polyfill examples.

**Effort:** **3–4 days for 3c.1**, **3c.2 is its own short project** (a
couple of days in wasi-polyfill).

---

### Phase 3d — retire `build-openssl.sh`

Per the original plan's Phase 5 gating: don't delete until verified.

| | |
|---|---|
| What | Remove `scripts/build-openssl.sh` from the default `make build` chain. Stop static-linking OpenSSL. Drop `Modules/_ssl` and `Modules/_hashopenssl` from CPython's Setup. |
| When | After 3b.6 passes the `test_ssl.py` subset AND 3c.1 has been green in CI for one release. |
| How | Same retirement-of-build-zlib.sh shape from Phase 5: delete the script, remove it from Makefile, remove the `--with-openssl` flag from `make build`, update docs. |
| Acceptance | The composed wasm has no static OpenSSL; `import ssl` still works (now via tls-wasm); `python.composed.wasm` shrinks by ~3 MB. |

**Effort:** **1 day** of cleanup once 3c.1 is stable.

---

## 4. Total estimate

| Phase | Days |
|---|---|
| WIT review checkpoint | 1 |
| 3a (tls-wasm component) | 4–6 |
| 3b (_ssl extension) | 5–7 |
| 3c.1 (wasmtime transport) | 2 |
| 3c.2 (browser transport) | 3–5 (wasi-polyfill side) |
| 3d (retire static) | 1 |
| **Total** | **16–22 eng-days** |

The variance is on tls-wasm correctness (R1 — BIO pumping has gotchas) and
on `_ssl` Python-API breadth (R5 — sizing what `ssl.py` actually needs).

---

## 5. Cross-cutting concerns

### Test strategy

Three test surfaces, each tested before the next is built:

1. **tls-wasm native unit tests** (Rust) — self-loop client↔server with our
   own state machine on both sides. Tests handshake, ALPN, plaintext
   round-trip, close_notify, cert validation pass + fail. Run on every
   `cargo test`.
2. **tls-wasm jco-instantiation smoke** — feeds canned record bytes into the
   component via jco, verifies it doesn't crash and emits the expected output.
   Confirms the component runs in the browser-targeted instantiator.
3. **`_ssl` Python tests** — subset of CPython's `Lib/test/test_ssl.py` runs
   against the composed wasm under wasmtime. Network-gated tests run on
   `pull_request` (with `network=true` flag), not on every push.

### Memory + perf budgets

| | Current (static OpenSSL) | After Phase 3 (composed) |
|---|---|---|
| python.composed.wasm size | ~38 MB | ~38 MB ± 500 KB |
| TLS handshake latency (wasmtime, local) | baseline | within 1.5× baseline (canonical-ABI overhead per BIO call) |
| Per-record encrypt/decrypt latency | baseline | within 1.5× baseline |

Per-call overhead matters more for TLS than for compression because TLS
issues many small operations. Watch the cost. If it's too high, add a
`read-plaintext-into(buffer)` variant that writes to a caller-owned buffer to
skip the result-list allocation per call.

### Documentation

- `docs/componentize-python.md` Phase 3 section gets fleshed out from
  "deferred" to "shipped" once 3b is in.
- `docs/browser-tls.md` (new) — what TLS does and doesn't do in the browser
  interpreter; the wss-proxy story; how to point at a different proxy.
- README of `tls-wasm` repo — standalone usage examples (it's reusable from
  any language, like every tegmentum capability).

### Compatibility

Python's `ssl` module's **public Python API** (`ssl.SSLContext`,
`ssl.SSLSocket`, `ssl.create_default_context()`, etc.) does not change.
Internally those route to `_ssl` which now uses tls-wasm. User code keeps
working.

Two known divergences from CPython's `_ssl` that must be documented:

- **No raw memory access to peer certs.** We expose DER bytes; if user code
  needs OpenSSL X509 objects, that's not available. (Probably nobody does
  this in regular code.)
- **SSL session resumption.** v1 doesn't expose session ticket
  save/restore. Adds latency for repeated connections but doesn't break
  correctness. Add in v1.1 if needed.

---

## 6. Decision points

Three places we deliberately reassess:

1. **End of WIT review (before any code).** Did walking through `ssl.py`
   reveal a method the WIT can't satisfy? If yes, fix the WIT *now*, not
   after we've written 500 lines of Rust.
2. **End of 3a.6 (CA roots smoke).** Does a real handshake against a real
   TLS server actually work? If not, root-cause before proceeding to 3b.
   Most likely failure: cert validation chain order, RAND seeding, or BIO
   pump corner case.
3. **End of 3b.6 (test_ssl.py subset).** Is the Python API actually behaving
   like CPython's `_ssl`? Anything that diverges either gets fixed or gets
   documented as a Phase 3 known issue (with a follow-up task) — *not*
   shipped silently.

---

## 7. Open questions to settle before starting

A short list — answer once, then proceed:

- **TLS implementation engine:** OpenSSL (via openssl-wasm) or rustls?
  OpenSSL pros: we already have the artifacts, ABI-compatible static libs,
  comprehensive. rustls pros: pure Rust, cleaner integration with the Rust
  component, smaller. Recommendation: **OpenSSL for v1** — we reuse the
  existing artifact + we don't want to debug rustls's wasm32-wasip2 support
  as the second new thing in this phase. Revisit after v1.
- **WebPKI root bundle source:** Mozilla CA bundle via `webpki-roots` crate?
  Or pin a specific bundle release? Recommendation: `webpki-roots` for now,
  document the pin.
- **Client cert auth in v1:** in or out? Recommendation: **out** — `ssl.py`
  callers very rarely use it; add as a v1.1 method on the resource.
- **OCSP stapling / SCT validation:** in or out? Recommendation: **out for
  v1**; documented limitation.

These are real choices but they're scoped — answering them is a half-hour
design call, not a multi-day investigation.

---

## 8. What this plan does *not* try to do

Naming explicitly so we don't scope-creep:

- Build a new TLS implementation. We wrap OpenSSL.
- Replicate every `_ssl` symbol. We replicate what `ssl.py` uses.
- Solve browser raw TCP. We use a wss proxy and document the constraint.
- Replace `_hashlib` (it's a separate static link). That's covered by
  Phase 2's `_crypto_hash` already shipped.

If any of those becomes a goal, it's its own plan.

---

## 9. Order of work for the first three days

If you say "go" tomorrow, this is the order:

| Day | Phase | Output |
|---|---|---|
| 1 (morning) | WIT review | `~/git/tls-wasm/wit/tls.wit` with the design above, walked against `ssl.py` and rustls's API and OpenSSL's BIO contract. Any needed adjustments made here. |
| 1 (afternoon) | 3a.1 | tls-wasm repo scaffold; `cargo component build` produces a wasm with the right exports. |
| 2 | 3a.2 + 3a.3 | constructor + handshake plumbing. Native test: two of our own clients hit a libssl-server in the same process; handshake completes. |
| 3 | 3a.4 | plaintext round-trip + cert metadata. Native: send/receive bytes, peer-cert-der returns valid DER. |

By end of day 3 we have a working tls-wasm component that loops back to
itself. The hard core is built. Everything after is plumbing.

---

## 10. The honest call

This is the single biggest piece of work in the broader componentize-python
plan. It's bigger than Phases 0+1+2 combined (those came in around 8 days of
work; this is 16–22). The reason: TLS is genuinely complicated and the
wasm-side ecosystem for it isn't as turnkey as compression or hashing.

But it's also the right shape — the WIT design is solid, the OpenSSL-based
engine reuses artifacts we already have, and once tls-wasm exists it's
reusable from any language consuming the org's capability layer (not just
Python). The `_ssl` C extension is the smallest piece because we don't need
full `_ssl.c` parity.

**Recommend:** start with the WIT review (day 1). It's the highest-leverage
half-day in the whole phase — the right WIT shape makes everything
downstream straightforward; the wrong one costs days of rework. Once the WIT
is reviewed and the openssl-wasm linkage is confirmed working in a tiny Rust
hello-world component (~half a day on top of 3a.1), the rest follows the
patterns we already proved in Phases 1+2.

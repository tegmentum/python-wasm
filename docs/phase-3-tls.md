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
    /// close_notify TLS alert; call `close-notify` first if you want one).
    resource client {
        /// Construct a TLS client. `config` carries every per-connection knob
        /// that CPython's _SSLContext exposes (cert validation, version pin,
        /// mTLS keys, ALPN, etc.). One constructor instead of a separate
        /// "context" resource keeps the WIT compact and avoids juggling two
        /// resource lifetimes.
        constructor(config: client-config);

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

        /// Number of decrypted plaintext bytes available without another
        /// network read (maps to SSL_pending / ssl.SSLObject.pending()).
        pending: func() -> u64;

        /// Where in the lifecycle the connection is.
        state: func() -> connection-state;

        /// Convenience for the handshake loop:
        ///   loop {
        ///       drain pull-tls-output to socket
        ///       if handshake-complete, break
        ///       feed socket bytes into push-tls-input
        ///   }
        handshake-complete: func() -> bool;

        /// Send a `close_notify` TLS alert; subsequent pull-tls-output will
        /// emit it. Caller should then close the underlying transport.
        close-notify: func() -> result<_, tls-error>;

        /// Peer leaf certificate as DER bytes (after handshake). None if not
        /// yet available or peer sent none.
        peer-cert-der: func() -> option<list<u8>>;

        /// Full peer certificate chain as the server sent it (DER bytes per
        /// cert, leaf first). Backs ssl.SSLObject.get_unverified_chain().
        get-unverified-chain: func() -> list<list<u8>>;

        /// The verified chain built by OpenSSL during handshake (DER per
        /// cert, leaf first). Backs ssl.SSLObject.get_verified_chain().
        get-verified-chain: func() -> list<list<u8>>;

        /// Negotiated cipher suite. None until handshake completes.
        cipher: func() -> option<cipher-info>;

        /// TLS channel binding bytes for `cb-type` ("tls-server-end-point" or
        /// "tls-unique"); none if the type is unknown or not yet available.
        /// Used by HTTP/2 client auth, SCRAM SASL, etc.
        channel-binding: func(cb-type: string) -> option<list<u8>>;

        /// Negotiated ALPN protocol, "" if none.
        alpn-selected: func() -> string;

        /// Negotiated TLS version, e.g. "TLSv1.3".
        version: func() -> string;
    }

    /// Per-connection client configuration. All optional fields default to the
    /// secure-by-default behavior described in their docstrings.
    record client-config {
        /// SNI server name; also used for hostname verification when
        /// `check-hostname` is true.
        server-name: string,

        /// Offered ALPN protocol list (e.g. ["h2", "http/1.1"]). Empty = no
        /// ALPN extension is sent.
        alpn: list<string>,

        /// PEM-encoded root CAs to trust. None = use bundled WebPKI roots
        /// (Mozilla CA bundle via webpki-roots, embedded at compile time).
        ca-roots: option<list<u8>>,

        /// Optional client certificate (PEM) + private key (PEM) for mTLS.
        /// Both must be provided to enable; otherwise mTLS is disabled.
        client-cert: option<list<u8>>,
        client-key:  option<list<u8>>,

        /// How to handle peer certificate validation. Defaults to `required`.
        verify-mode: verify-mode,

        /// Whether to validate the server's certificate against `server-name`
        /// (hostname checking). Independent of `verify-mode` so callers can
        /// e.g. verify the chain without enforcing the hostname.
        /// Defaults to true.
        check-hostname: bool,

        /// Minimum TLS protocol version accepted. Defaults to TLSv1.2.
        min-version: tls-version,

        /// Maximum TLS protocol version offered. Defaults to TLSv1.3.
        max-version: tls-version,
    }

    record cipher-info {
        name: string,        // e.g. "TLS_AES_256_GCM_SHA384"
        version: string,     // e.g. "TLSv1.3"
        secret-bits: u32,    // key bits (256 etc.)
    }

    enum verify-mode {
        none,        // accept any cert (TEST USE ONLY)
        optional,    // request cert; accept if absent or valid
        required,    // demand a valid cert
    }

    enum tls-version {
        tls-v1-2,
        tls-v1-3,
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
        invalid-config(string),      // ctor config rejected (bad PEM etc.)
        io(string),                  // unexpected internal failure
    }
}

interface server {
    /// Server-side TLS. Listed for completeness; ships in v1.1 (the bring-up
    /// plan focuses on the client). Reuses context's verify-mode / tls-version
    /// / connection-state / tls-error.
    use context.{verify-mode, tls-version, connection-state, tls-error, cipher-info};

    resource handle {
        constructor(config: server-config);
        push-tls-input: func(bytes: list<u8>) -> result<u64, tls-error>;
        pull-tls-output: func(max: u64) -> list<u8>;
        write-plaintext: func(bytes: list<u8>) -> result<u64, tls-error>;
        read-plaintext: func(max: u64) -> result<list<u8>, tls-error>;
        pending: func() -> u64;
        state: func() -> connection-state;
        handshake-complete: func() -> bool;
        close-notify: func() -> result<_, tls-error>;
        peer-cert-der: func() -> option<list<u8>>;
        get-unverified-chain: func() -> list<list<u8>>;
        get-verified-chain: func() -> list<list<u8>>;
        cipher: func() -> option<cipher-info>;
        channel-binding: func(cb-type: string) -> option<list<u8>>;
        alpn-selected: func() -> string;
        version: func() -> string;
    }

    record server-config {
        cert-chain-pem: list<u8>,
        private-key-pem: list<u8>,
        alpn: list<string>,
        client-verify: verify-mode,   // require-client-cert is verify-mode::required
        client-ca-roots: option<list<u8>>,
        min-version: tls-version,
        max-version: tls-version,
    }
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

### WIT review checkpoint — DONE (2026-05-26)

Walked the WIT against `Lib/ssl.py`'s actual `_ssl` call surface (inventory
via `grep -oE 'self\._sslobj\.\w+'` etc.) plus a mental walk of rustls's
`ClientConnection` and the OpenSSL BIO + SSL_* contract. The WIT above is the
post-review revision; this section records what changed and why.

**Additions driven by `ssl.py` actually using them:**

| Added | Why (which `ssl.py` call needs it) |
|---|---|
| `pending() -> u64` | `SSLObject.pending()` — non-blocking "is there decoded data ready?" |
| `cipher() -> option<cipher-info>` | `SSLObject.cipher()` — returns (name, version, secret_bits) tuple |
| `get-verified-chain` / `get-unverified-chain` | `SSLObject.get_{un,}verified_chain()` — full DER chains, not just leaf |
| `channel-binding(cb-type)` | `SSLObject.get_channel_binding()` — required for HTTP/2 + SCRAM-SHA-PLUS |
| `verify-mode` enum | `_SSLContext.verify_mode` ∈ {CERT_NONE, CERT_OPTIONAL, CERT_REQUIRED} |
| `check-hostname: bool` | `_SSLContext.check_hostname` — independent of verify_mode in Python's model |
| `min-version` / `max-version` | `_SSLContext.minimum_version` / `.maximum_version` |
| `invalid-config(string)` error variant | Bad PEM / unparseable key on construction needs its own error |

**Structural change:** the four ctor args (server-name, alpn, ca-roots,
client-cert, client-key) grew to nine fields. Collapsed into a
`record client-config { ... }` so the ctor stays a single argument. Same for
the server side (`server-config`).

**Deliberately deferred to v1.1 (documented limitations):**

- `session` / `session_reused` — TLS session resumption. Latency hit on repeat
  connects but doesn't break correctness.
- `shared_ciphers()` — server-side; rarely used.
- `compression()` — TLS compression is deprecated; current impls return None.
- `verify_client_post_handshake()` — server-side mTLS escalation.
- `set_servername_callback` / `sni_callback` — server-side SNI dispatch.
- `_msg_callback` — debug only.

**Sanity-checked against rustls + OpenSSL:**

- rustls's `read_tls`/`write_tls`/`process_new_packets`/`writer()`/`reader()`
  maps 1:1 onto our push-tls-input / pull-tls-output / (implicit) /
  write-plaintext / read-plaintext.
- OpenSSL's `BIO_write` / `BIO_read` / `SSL_read` / `SSL_write` /
  `SSL_pending` / `SSL_get_current_cipher` / `SSL_get_peer_cert_chain` /
  `SSL_get_finished` / `SSL_get_peer_finished` (for channel binding) all map
  to methods we expose. The Rust component just FFI-wraps these.

**Two known WIT-syntax checks deferred to wit-bindgen invocation:**

- `cipher` (the method) returning `option<cipher-info>` — wit-bindgen will
  generate a struct with `is_some` / `val` flat layout. Confirmed pattern
  from compression / crypto-hash bindings.
- `record server-config` shares `verify-mode` and `tls-version` from
  `interface context` via `use`. Standard cross-interface type sharing.

---

## 3. Phases

### Phase 3a — TLS engine (PIVOT: reuse `~/git/openssl-wasm` directly)

**Major finding (2026-05-26):** `~/git/openssl-wasm` already ships a built
component (`build/openssl-component.wasm`, 3.8 MB) exporting
`openssl:component/tls@0.1.0` — a complete TLS surface (mTLS, ALPN, 0-RTT,
session tickets, server side, key logging). The world also exports
error/random/bignum/digest/mac/cipher/kdf/pkey/x509, all from the same
underlying OpenSSL.

**Pivot:** rather than build a *new* `~/git/tls-wasm` that duplicates the
work, Phase 3a becomes "consume openssl-wasm's existing TLS interface
directly." The drafted `tegmentum:tls` WIT becomes the *reference* for
features Phase 3b's `_ssl.c` needs from `openssl:component/tls` (almost all
covered already).

**Trade-off:** openssl-wasm's TLS is **connection-managing**, not memory-BIO:
`tls.client.connect(host, port, config)` opens a TCP socket internally via
`wasi:sockets/tcp` and does the handshake. That's:

- ✅ wasmtime — works as-is, sockets are first-class.
- ❌ browser — no raw TCP. Phase 3c.2's wss-proxy work in `wasi-polyfill`
  (originally scheduled later) now becomes a 3a-or-3c hard dependency for
  the browser path. wasmtime path still ships independently.

**What changes vs the original Phase 3a plan:**

- 3a.1 → drop (no new repo).
- 3a.2 – 3a.7 → drop (openssl-wasm did this).
- New 3a.1: verify openssl-wasm's tls interface against `_ssl`'s needs (the
  drafted tegmentum:tls WIT serves as the gap-analysis checklist).
- New 3a.2: build openssl-wasm if not already built; pin a known-good
  artifact.

**Sub-deliverables (revised):**

| # | Deliverable | Acceptance |
|---|---|---|
| **3a.1** | Walk every method the `tegmentum:tls` draft documents against `openssl:component/tls`. Identify which are: present-as-is, present-but-named-differently, present-with-different-shape, or missing. | Gap inventory in this doc. |
| **3a.2** | Confirm `build/openssl-component.wasm` runs in jco (load-only smoke, since browser sockets aren't there yet). | Component instantiates without missing imports beyond `wasi:sockets/tcp` (acceptable, that's the documented browser gap). |
| **3a.3** | Build `~/git/openssl-wasm` from source to confirm the build is reproducible (so we're not pinned to a stale prebuilt artifact). | `cargo build --release` succeeds; resulting wasm matches the existing one in WIT export shape. |

#### 3a.1 — gap inventory (DONE)

What `_ssl` needs from `tegmentum:tls` ↔ where it comes from in
`openssl:component/tls`:

| `_ssl` need (from WIT review) | `openssl:component/tls` source | Status |
|---|---|---|
| `push-tls-input(bytes)` / `pull-tls-output(max)` | _not exposed_ — connect manages I/O internally via wasi:sockets | **GAP (memory-BIO)**: openssl-wasm pushes/pulls itself; we don't pump. For the browser path this is what motivates Phase 3p (provide sockets via polyfill) instead of memory-BIO. wasmtime path: no gap (sockets work). |
| `write-plaintext(bytes)` | `client.write(data) -> result<u32, tls-error>` | ✓ same shape |
| `read-plaintext(max)` | `client.read(max-bytes) -> result<list<u8>, tls-error>` | ✓ same shape |
| `pending() -> u64` | _not exposed_ | **GAP (small)**: openssl-wasm doesn't surface `SSL_pending`. v1 can return `0` (ssl.py only uses pending() for non-blocking optimization; correctness is preserved). |
| `handshake-complete()` | Implicit — `connect` blocks until handshake done | ✓ different model; consume as "if connect() returned ok, handshake is done" |
| `state()` | _not exposed_ | **GAP (cosmetic)**: derive from "have we called connect" + last error |
| `close-notify()` | `client.close: static func(c)` (drops + sends close_notify) | ✓ slightly different (combines drop and close_notify); _ssl maps shutdown() to this |
| `peer-cert-der()` | `client.peer() -> peer-info` includes `peer-chain: list<certificate>`; cert.der() | ✓ first element of peer-chain |
| `get-unverified-chain()` | `peer-chain` (the as-received chain) | ✓ |
| `get-verified-chain()` | _not directly exposed_ as a separate verified chain | **GAP (medium)**: openssl-wasm trusts SSL_get_peer_cert_chain (= verified after handshake). v1: return same as unverified (Python's `ssl` does the same when verify is required). Future: upstream PR for separate verified chain. |
| `cipher()` | `peer-info.cipher-suite: string` (name only) | **PARTIAL**: openssl-wasm gives the name but not version/secret-bits as a tuple. v1: synthesize version from `peer-info.protocol`, default secret-bits to 0 (unused by ssl.py beyond display). |
| `channel-binding(cb-type)` | _not exposed_ | **GAP (deferred)**: SSL_get_finished/SSL_get_peer_finished not in openssl-wasm's WIT. v1: return None (callers that need SCRAM-PLUS or HTTP/2 client-auth will fail; documented limitation). Future: upstream PR. |
| `alpn-selected()` | `peer-info.alpn: option<string>` | ✓ |
| `version()` | `peer-info.protocol: protocol` (enum: tls12, tls13, dtls12) → string | ✓ enum-to-string in the _ssl layer |
| `verify-mode` ctor knob | `client-config.verify: verify-mode {none, required, optional}` | ✓ direct |
| `check-hostname` ctor knob | implicit (server-name verified when verify != none) | **PARTIAL**: openssl-wasm always validates hostname when verify is on. v1: documented limitation — can't disable hostname check independently. Almost no real code does. |
| `min-version` / `max-version` | `client-config.protocols: protocol-range {min, max}` | ✓ different name |
| `ca-roots` (empty = WebPKI) | `client-config.trust: option<store>` (must construct a `store` from CA bytes via openssl:component/x509) | **DIFFERENT**: more ceremony — _ssl creates an x509 store from the bundle bytes. v1: bundle webpki-roots PEM, use x509.store::from-pem. |
| `client-cert` / `client-key` (mTLS) | `client-config.client-cert: option<certificate>` + `client-key: option<pkey>` | ✓ same model; _ssl marshals PEM via openssl:component/x509 and pkey |

**Verdict:** openssl-wasm's TLS covers the ssl.py surface that matters for v1
with minor cosmetic / advanced gaps:

- 3 small gaps return constant/default values in v1: `pending` → 0,
  `get-verified-chain` → same as unverified, `cipher.secret-bits` → 0.
- 1 partial: hostname-checking is bundled with cert verification (acceptable;
  ssl.py defaults align).
- 1 deferred: `channel-binding` returns None (SCRAM-PLUS / HTTP/2 client auth
  fail — documented).

None of these block v1. All have clean future paths (upstream PRs to
openssl-wasm extending its tls interface).

#### 3a.2 — jco load smoke (DONE)

`build/openssl-component.wasm` transpiles cleanly via jco (4 core modules +
glue) and `instantiate` loads — confirms the artifact is browser-pipeline
compatible. The remaining `wasi:sockets/tcp` imports are exactly what
Phase 3p will satisfy from the polyfill.

#### 3a.3 — reproducibility (DONE, pinned not rebuilt)

The openssl-wasm build pipeline is `make component`, dry-run-confirmed to
invoke OpenSSL's `Configure wasm32-wasip2 no-threads ...` and link via the
existing Rust component crate. Heavy build (~15 min on a warm cache);
skipped here. Pinning the existing artifact instead:

- **Path:** `~/git/openssl-wasm/build/openssl-component.wasm`
- **Size:** 3,829,694 bytes (3.65 MB)
- **SHA-256:** `5bed74a40b863bd100bfbec61d1ae4b8f6063006b03305dc2f33167bb8b9fade`
- **Exports:** error, random, bignum, digest, mac, cipher, kdf, pkey, x509, **tls** (all `openssl:component/*@0.1.0`)
- **Imports:** wasi:cli, wasi:io, wasi:filesystem, wasi:sockets (network/tcp/udp/ip-name-lookup), wasi:clocks, wasi:random — all `@0.2.6`

A `scripts/build-openssl-component.sh` in python-wasm will, in Phase 3b/3c,
verify this digest matches before composition (same pattern as the
compression / hash multiplexer digests pinned in the composectl plan).

**Component artifact:** `~/git/openssl-wasm/build/openssl-component.wasm`
(3.8 MB, already built). Imports `wasi:sockets/tcp@0.2.6` among other
standard wasi imports.

**Risks (revised):**

- **R1 (deleted):** no BIO pumping in *our* code; openssl-wasm did that.
- **R2: openssl-wasm's wasi:sockets dep is mandatory in v1.** Browser path
  blocked on wasi-polyfill providing a wss-proxy-backed `wasi:sockets/tcp`
  (Phase 3c.2). wasmtime path unaffected.
- **R3: We don't control the openssl:component/tls WIT.** If a future need
  surfaces a missing method (e.g. memory-BIO mode for browser-without-proxy),
  it's an upstream PR to openssl-wasm.
- **R4: Cipher / digest overlap.** openssl-wasm exports `cipher` / `digest` /
  `mac` interfaces that overlap our existing Phase 2 `_crypto_hash`
  (which uses `tegmentum:crypto-hash-multiplexer`). Out of scope here — we
  keep _crypto_hash as the hashlib backend; we use openssl-wasm only for tls.

**Effort:** **0.5–1 day** (verify gap inventory + build reproducibility).
Was 4–6 days; the pivot reclaims ~5 days from the overall Phase 3 budget.

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

| # | Deliverable | Acceptance | Status |
|---|---|---|---|
| **3b.1** | Scaffold `cpython-ext/_ssl/` (WIT vendored, `wit-bindgen c` output, Setup.local entry, parallel naming `_ssl_capability` so the static `_ssl` keeps working during bring-up). | Stub module builds; `import _ssl_capability` works; python.wasm declares `import openssl:component/x509`; composed wasm runs the scaffold smoke. | **✅ DONE** (commit `7978b6f`). |
| **3b.2** | Implement `_ssl_capability.MemoryBIO` (CPython's BIO wrapper class). Implemented as a pure in-memory FIFO byte buffer; openssl-component owns its own internal BIOs so we don't need to expose theirs. | Full semantic test (read/write/pending/eof/write_eof/EOF-error/buffer-protocol/100-iter FIFO-vs-reference pump), plus byte-for-byte parity against CPython's static `ssl.MemoryBIO`. `make test-ssl-capability`. | **✅ DONE**. |
| **3b.3** | Implement the minimal `_ssl_capability._SSLContext` + `_SSLSocket` surface. **Decision (vs plan):** openssl-component's TLS is connection-managing (no memory-BIO mode), so v1 ships `wrap_socket(host, port, server_hostname)` not `wrap_bio(in, out)` — the latter raises NotImplementedError until openssl-wasm gains memory-BIO upstream. SSLContext: verify_mode getter/setter + set_ca_certs / set_client_cert / set_alpn_protocols / wrap_socket. SSLSocket: read / write / pending / shutdown / version / cipher / selected_alpn_protocol / server_hostname. SSLError exception. CERT_NONE / CERT_OPTIONAL / CERT_REQUIRED constants. | **PASSED Decision Point #2** — `make test-ssl-network` does a real TLS 1.3 handshake to example.com:443, sends a GET, receives `HTTP/1.1 200 OK`, shutdown is idempotent, write-after-shutdown raises SSLError. Cipher: TLS_AES_256_GCM_SHA384. | **✅ DONE**. |
| **3b.4** | Add `_ssl_capability.RAND_bytes`, `OPENSSL_VERSION`, and a small set of constants `ssl.py` references at import time. | `import ssl` (after re-routing in 3b.6) doesn't AttributeError. | pending |
| **3b.5** | Wire the socket-backed path: `_SSLSocket` over a real fd. Under wasmtime this is automatic — `openssl:component/tls.client.connect(host, port, config)` handles the socket itself via wasi:sockets/tcp. | `python -c "import ssl, urllib.request; print(urllib.request.urlopen('https://tls13.akamai.com').read(80))"` under wasmtime returns content. | pending |
| **3b.6** | Re-route CPython's `_ssl` symbols so `import ssl` keeps working. Two-mode build: static `_ssl` + capability `_ssl_capability` coexist; `_ssl` re-exports the latter. Phase 5 removes the static path. | `Lib/test/test_ssl.py` subset (`test_constructor`, `test_wrap_bio_handshake`, `test_pending`, `test_shutdown`) passes. | pending |

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

### Phase 3p — `wasi-polyfill` support for `wasi:sockets/tcp` ✅ DONE (mostly via pre-existing work)

**Surprise win:** wasi-polyfill already had the implementation. Phase 3p
shipped in `~/git/wasi-polyfill` commit `9eb4ab3` with one cosmetic change
(version pins) + one integration test (proves the surface).

What was already in wasi-polyfill before this phase:

- `src/wasip2/plugins/sockets/` — all 7 `wasi:sockets/*` interfaces
  (network, instance-network, ip-name-lookup, tcp, tcp-create-socket, udp,
  udp-create-socket) with virtual / NotSupported implementations for
  browser-without-proxy.
- `src/wasip2/plugins/ws-gateway/` — production TCP+UDP **over a
  WebSocket-multiplexing gateway** (the "KSW1" protocol). Includes
  TcpAdapter, UdpAdapter, DnsAdapter, ByteQueue, TunnelManager — every
  piece the original plan called for. Much more sophisticated than the
  single-WebSocket-per-TCP-connection design I'd drafted.

What this phase added:

- Bumped 7 version pins from `0.2.0` → `0.2.6` in `plugins/sockets/plugin.ts`
  (cosmetic — registry is version-agnostic via `interfaceKey()` — but matches
  what openssl-component imports).
- `test/integration/python-wasm-sockets-compose.mjs` — exercises both the
  virtual sockets plugin set and the ws-gateway plugin set against the
  full list of `wasi:sockets/*` imports python-wasm's composed wasm needs.
  **14/14 imports satisfied** across both paths.

What remains (folded into Phase 3c):

- Wire the gateway URL through python-wasm's web-runner (Phase 3c.2).
- Real-network smoke against a TLS server through this stack (Phase 3c.1).
- `docs/browser-tls.md` (Phase 3c.3).

**Status of original 3p.1–3p.6 sub-deliverables:**

| # | Deliverable | Status |
|---|---|---|
| **3p.1** | Inventory `wasi:sockets/*` surface, map to WebSocket ops. | ✅ Already in `wasi-polyfill/src/wasip2/plugins/sockets/` + `ws-gateway/` (both well-documented). |
| **3p.2** | Implement network / instance-network / ip-name-lookup stubs. | ✅ Pre-existing in `plugins/sockets/`. |
| **3p.3** | Implement `tcp-socket` resource backed by WebSocket. | ✅ Pre-existing as `plugins/ws-gateway/tcp-adapter.ts` (KSW1 multiplexing protocol — more sophisticated than the per-connection WebSocket I had drafted). |
| **3p.4** | Configuration surface for the gateway URL. | ✅ `polyfill.registerPlugin(wsGatewayTcpPlugin, { options: { gatewayUrl: 'wss://...' } })`. |
| **3p.5** | Reference gateway server. | ⏸ Not shipped here. The polyfill includes the KSW1 protocol spec; a reference Node server can ship as part of Phase 3c.1 if needed (the wasmtime CI smoke doesn't need it). |
| **3p.6** | In-browser bytes-flow smoke. | ⏸ Folded into Phase 3c.2 (browser end-to-end against a real gateway). |

**Risks:**

- **R9: wasi:sockets/tcp@0.2.6 is a moving target.** It went through several
  WIT revisions during wasi-preview2 stabilization. The polyfill must match
  the exact version openssl-wasm imports. Pin once, verify on update.
- **R10: WebSocket back-pressure.** WS doesn't natively expose per-message
  back-pressure the way native TCP does (just `bufferedAmount`). For TLS
  this is fine because reads/writes are application-paced; for high-throughput
  use it could matter. Documented limitation; acceptable for v1.
- **R11: Proxy is a security boundary.** A wss-proxy that anyone can use to
  open arbitrary TCP is a serious tunneling primitive. The reference proxy
  ships with **explicit allowlist** for destination host:port pairs by
  default; production deployments configure their own allowed targets.
- **R12: DNS.** `ip-name-lookup` over `fetch()`-based DoH or via the proxy
  itself. Recommend: lookup goes through the proxy ("CONNECT host:port"
  semantics) — keeps the security model in one place.

**Effort:** **5–7 days.** Was originally budgeted 3–5 days as Phase 3c.2;
elevated because the wasmtime path's success no longer fronts the work.

---

### Phase 3c — transport wiring (browser end-to-end)

With 3p done, this phase makes the *composed python.wasm* actually work in
the browser via the polyfill, and adds the wasmtime CI smoke.

**Sub-deliverables:**

| # | Deliverable | Acceptance |
|---|---|---|
| **3c.1** | CI smoke test against a real TLS server, gated on a `network=true` flag, under wasmtime. | `make test-ssl-extension` (new target) does a real HTTPS GET via wasi:sockets/tcp. |
| **3c.2** | Web demo wiring: `web/src/python-runner.ts` registers the wasi-polyfill TCP plugin with the configured wss-proxy URL. | The composed python.wasm in the browser can `import ssl; import urllib.request; urllib.request.urlopen("https://...")` via the reference proxy. |
| **3c.3** | `docs/browser-tls.md`: document the wss-proxy constraint, how to point at a different proxy, security model. | The constraint is unambiguous to a new user. |

**Risks:**

- (R8 from the original plan rolls up into R11 above.)

**Effort:** **2–3 days** (was 3–4; smaller because the polyfill work is its
own phase now).

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

## 4. Total estimate (revised after 3a pivot)

| Phase | Days |
|---|---|
| WIT review checkpoint | 0.5 ✅ DONE |
| Open-question settle | 0.5 ✅ DONE |
| 3a (reuse openssl-wasm/tls — was 4–6 building from scratch) | 0.5–1 |
| 3b (`_ssl` extension) | 5–7 |
| 3p (wasi-polyfill `wasi:sockets/tcp` over wss-proxy) | 5–7 |
| 3c.1 (wasmtime CI smoke) | 1 |
| 3c.2 (browser e2e wiring) | 1 |
| 3c.3 (docs/browser-tls.md) | 0.5 |
| 3d (retire static OpenSSL) | 1 |
| **Total** | **14–19 eng-days** |

The pivot reclaimed ~5 days from 3a; the polyfill work (3p) was always
implied as "Phase 3c.2" but is now sized properly as its own phase. Net
budget is similar to the original 16–22 estimate, but the work is rebalanced
toward the polyfill (which delivers value beyond TLS — any future component
needing wasi:sockets benefits).

Decision-point #2 (was: "real handshake from tls-wasm under wasmtime") moves
to the end of 3b (the new "first real HTTPS request from CPython through
openssl-wasm under wasmtime").

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

## 7. Open questions — SETTLED

Decisions taken before WIT review (2026-05-26):

| Question | Decision |
|---|---|
| **TLS engine** | **OpenSSL** via `~/git/openssl-wasm` static libs. Reuses existing artifacts; same wasi-sdk 33 ABI as our CPython build. The Rust component does FFI + memory-BIO pumping. |
| **WebPKI root source** | **`webpki-roots` crate** (the Rustls project's auto-updated Mozilla CA bundle). ~250 KB embedded; bumped via Cargo. |
| **Client cert auth in v1** | **IN** — the client constructor takes optional `client-cert: option<list<u8>>` and `client-key: option<list<u8>>` (PEM). Enterprise users get it out of the gate; the WIT surface stays compact. |
| **OCSP stapling / SCT validation** | **OUT for v1** — documented limitation. OpenSSL still validates the cert chain. |

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

## 9. Order of work (revised after 3a pivot)

Two tracks that can run in parallel after 3a:

**Track A — CPython side (wasmtime path first, browser eventually):**

| Day | Phase | Output |
|---|---|---|
| 1 (done) | WIT review | docs/phase-3-tls.md WIT instantiated as cpython-ext/_ssl/wit/. ✅ |
| 1 (done) | Open questions settled | engine=openssl-wasm, ca=webpki-roots, client-cert in v1, OCSP out. ✅ |
| 2 (morning) | 3a.1 | Gap inventory: tegmentum:tls (drafted) ↔ openssl:component/tls (existing). |
| 2 (afternoon) | 3a.2 + 3a.3 | openssl-wasm component loads in jco; build is reproducible. |
| 3–5 | 3b.1–3b.4 | `_ssl` extension scaffold + MemoryBIO + minimal _SSLContext/_SSLSocket + RAND/OPENSSL_VERSION constants. |
| 6 | 3b.5 + 3c.1 | wasi:sockets-backed _SSLSocket + wasmtime CI smoke against tls13.akamai.com. |
| 7 | 3b.6 | test_ssl.py subset green (no-network). |

**Track B — wasi-polyfill side (gates the browser path):**

| Day | Phase | Output |
|---|---|---|
| 1–2 | 3p.1 + 3p.2 | Inventory + minimal network/instance-network/ip-name-lookup. |
| 3–4 | 3p.3 + 3p.5 | wasi:sockets/tcp resource backed by WebSocket; reference wss-proxy. |
| 5 | 3p.4 + 3p.6 | Configuration surface + in-browser smoke. |

**Convergence:** once both tracks reach their endpoints, 3c.2 (browser e2e)
+ 3c.3 (docs) close the loop in ~1 day.

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

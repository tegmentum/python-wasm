// End-to-end smoke for ws-gateway-server.mjs.
//
// Runs the gateway + python wasm in the same process. The polyfill is
// configured to route TCP through the gateway (ws://127.0.0.1:8088/ws)
// just like the browser would. The Python code does a real HTTPS
// request via ssl_capability + cap-routed sockets.
//
//   WASI_POLYFILL_ASYNC_READ_YIELD=1 \
//   WASI_POLYFILL_USE_WS_PKG=1 \
//     node web/gateway-smoke.mjs
//
// WASI_POLYFILL_ASYNC_READ_YIELD=1 is required (since wasi-polyfill
// 60f852b) for the polyfill's input-stream.read to yield the host
// event loop -- the JSPI bundle's read trampoline is manuallyAsync so
// the polyfill's Promise return works correctly.
//
// WASI_POLYFILL_USE_WS_PKG=1 is optional; both globalThis.WebSocket
// and the `ws` npm package work.
//
// CURRENT STATUS (2026-05-30): END-TO-END FULL HTTP LIFECYCLE WORKING.
//
//   wasm Python `s.connect((host, port))`   -> tunneled, ok
//   wasm Python `s.sendall(b"GET ...")`      -> reaches upstream
//   upstream sends 200 + Connection:close
//   gateway forwards response back via WS
//   wasm Python `s.recv(4096)` iter=1        -> 133-byte body
//   wasm Python `s.recv(4096)` iter=2        -> 0 bytes (clean EOF)
//   loop breaks; "done. total bytes: 133"; exit 0
//
// Requires wasi-polyfill ≥ 60f852b (createTcpSocket signature,
// canonical-ABI connect lifecycle for JSPI, synthetic localAddress,
// real readiness Pollable for input-stream, closed-rxQueue EOF in
// read paths, atomic null-on-empty-closed in readDataAsync, and the
// opt-in async-read microtask yield gated by
// WASI_POLYFILL_ASYNC_READ_YIELD=1).

import { spawn, execFileSync } from 'node:child_process'
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { gunzipSync } from 'node:zlib'
import { tmpdir } from 'node:os'
import path from 'node:path'
import url from 'node:url'

import {
  registerCorePlugins,
  registerPlugin,
  Polyfill,
  createPolicy,
} from '@tegmentum/wasi-polyfill'
import {
  EmptyInputStream,
  createCustomStdio,
  setGlobalStdioProvider,
  resetGlobalStdioState,
  ComponentExitError,
} from '@tegmentum/wasi-polyfill/plugins/cli'
import {
  MemoryFileSystem,
  setGlobalFilesystem,
  resetGlobalFilesystem,
} from '@tegmentum/wasi-polyfill/plugins/filesystem'
import { socketPlugins } from '@tegmentum/wasi-polyfill/plugins/sockets'
import {
  wsGatewayTcpPlugin,
  wsGatewayTcpCreateSocketPlugin,
} from '@tegmentum/wasi-polyfill/plugins/ws-gateway'

const HERE = path.dirname(url.fileURLToPath(import.meta.url))
const REPO = path.resolve(HERE, '..')
// JSPI bundle. Note: --async-imports is required for input-stream.read
// in addition to --async-wasi-imports (the wasi defaults don't include
// read since it's spec'd non-blocking; tunneled streams need it to
// yield the event loop between Python recv-loop polls). Recreate with:
//   npx --prefix web jco transpile build/3.14-current/python.composed.wasm \
//     -o /tmp/python-component-node-jspi --no-nodejs-compat \
//     --instantiation async --name python --async-mode jspi \
//     --async-wasi-imports --async-wasi-exports \
//     --async-imports 'wasi:io/streams@0.2.6#[method]input-stream.read'
//   sed -i.bak 's|^const definedResourceTables = \[.*\];$|const definedResourceTables = new Proxy([], { get: () => true });|' /tmp/python-component-node-jspi/python.js
const COMPONENT_DIR = '/tmp/python-component-node-jspi'
const STDLIB_TGZ = path.join(REPO, 'web/public/stdlib.tar.gz')
const GATEWAY_URL = 'ws://127.0.0.1:8088/ws'
const PYTHONPATH = '/Lib:/cross-build/wasm32-wasip2/build/lib.wasi-wasm32-3.14'

const WASI_INTERFACES = [
  'wasi:cli/environment@0.2.0','wasi:cli/stdin@0.2.0','wasi:cli/stdout@0.2.0','wasi:cli/stderr@0.2.0',
  'wasi:cli/exit@0.2.0','wasi:cli/terminal-input@0.2.0','wasi:cli/terminal-output@0.2.0',
  'wasi:cli/terminal-stdin@0.2.0','wasi:cli/terminal-stdout@0.2.0','wasi:cli/terminal-stderr@0.2.0',
  'wasi:io/streams@0.2.0','wasi:io/poll@0.2.0','wasi:io/error@0.2.0',
  'wasi:clocks/monotonic-clock@0.2.0','wasi:clocks/wall-clock@0.2.0','wasi:random/random@0.2.0',
  'wasi:filesystem/types@0.2.0','wasi:filesystem/preopens@0.2.0',
  'wasi:sockets/network@0.2.0','wasi:sockets/instance-network@0.2.0','wasi:sockets/ip-name-lookup@0.2.0',
  'wasi:sockets/tcp@0.2.0','wasi:sockets/tcp-create-socket@0.2.0',
  'wasi:sockets/udp@0.2.0','wasi:sockets/udp-create-socket@0.2.0',
]

function parseUstar(buf) {
  const files = new Map()
  let off = 0
  while (off + 512 <= buf.length) {
    const block = buf.subarray(off, off + 512)
    if (block.every((b) => b === 0)) break
    const name = block.subarray(0, 100).toString('utf8').replace(/\0+$/, '')
    if (!name) break
    const size = parseInt(block.subarray(124, 136).toString('utf8').replace(/\0/g, '').trim() || '0', 8)
    const typeflag = String.fromCharCode(block[156])
    const prefix = block.subarray(345, 500).toString('utf8').replace(/\0+$/, '')
    const fullName = prefix ? `${prefix}/${name}` : name
    off += 512
    if (typeflag === '0' || typeflag === '\0' || typeflag === '') {
      files.set(fullName, buf.subarray(off, off + size))
    }
    off += Math.ceil(size / 512) * 512
  }
  return files
}

function buildStdlibFs() {
  const tar = gunzipSync(readFileSync(STDLIB_TGZ))
  const entries = parseUstar(tar)
  const fs = new MemoryFileSystem()
  for (const [name, content] of entries) {
    const parts = name.split('/')
    let dir = ''
    for (let i = 0; i < parts.length - 1; i++) {
      dir += '/' + parts[i]
      if (fs.getNode(dir).tag === 'err') fs.createDirectory(dir)
    }
    const r = fs.createFile('/' + name, { create: true, truncate: true })
    if (r.tag === 'ok') r.val.content = content
  }
  return fs
}

class TextCapture {
  isTTY = false
  constructor() { this.chunks = [] }
  write(bytes)  { this.chunks.push(Buffer.from(bytes).toString('utf8')); return Promise.resolve() }
  flush()       { return Promise.resolve() }
  get text()    { return this.chunks.join('') }
}

async function waitFor(url, timeoutMs = 10000) {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    try {
      const { WebSocket } = await import('ws')
      const ws = new WebSocket(url)
      await new Promise((resolve, reject) => {
        ws.once('open', () => { ws.close(); resolve(undefined) })
        ws.once('error', reject)
      })
      return
    } catch {
      await new Promise((r) => setTimeout(r, 200))
    }
  }
  throw new Error(`Gateway didn't come up in time: ${url}`)
}

async function main() {
  // 1a) Two upstream servers, each in a child process. HTTP for the
  //     warm-up sanity, HTTPS for the full openssl-component + TLS
  //     exercise. Self-signed cert generated inline; gets cleaned up
  //     in stopAll.
  const certDir = mkdtempSync(path.join(tmpdir(), 'gateway-smoke-tls-'))
  const certPath = path.join(certDir, 'cert.pem')
  const keyPath  = path.join(certDir, 'key.pem')
  execFileSync('openssl', [
    'req', '-x509', '-nodes', '-newkey', 'rsa:2048',
    '-keyout', keyPath, '-out', certPath,
    '-days', '1', '-subj', '/CN=localhost',
  ], { stdio: ['ignore', 'ignore', 'ignore'] })
  // Stash the cert PEM so the python guest can trust it.
  const certPem = readFileSync(certPath, 'utf8')

  const upstream = spawn(process.execPath, [path.join(HERE, 'test-upstream.mjs')], {
    stdio: ['ignore', 'inherit', 'inherit'],
    detached: false,
  })
  const upstreamHttps = spawn(process.execPath, [path.join(HERE, 'test-upstream-https.mjs')], {
    stdio: ['ignore', 'inherit', 'inherit'],
    detached: false,
    env: { ...process.env, UPSTREAM_CERT: certPath, UPSTREAM_KEY: keyPath },
  })

  // 1b) Start gateway
  const gateway = spawn(process.execPath, [path.join(HERE, 'ws-gateway-server.mjs')], {
    stdio: ['ignore', 'inherit', 'inherit'],
    detached: false,
  })
  const stopAll = () => {
    try { gateway.kill('SIGTERM') } catch {}
    try { upstream.kill('SIGTERM') } catch {}
    try { upstreamHttps.kill('SIGTERM') } catch {}
    try { rmSync(certDir, { recursive: true, force: true }) } catch {}
  }
  process.on('exit', stopAll)
  process.on('SIGINT', () => { stopAll(); process.exit(1) })
  process.on('SIGTERM', () => { stopAll(); process.exit(1) })
  await waitFor(GATEWAY_URL)
  await new Promise((r) => setTimeout(r, 500))  // let upstream bind
  console.log('gateway reachable, booting python...')

  // 2) Register polyfill plugins (with ws-gateway TCP)
  await registerCorePlugins()
  for (const p of socketPlugins) registerPlugin(p)
  registerPlugin(wsGatewayTcpPlugin)
  registerPlugin(wsGatewayTcpCreateSocketPlugin)

  // 3) Build stdio + filesystem
  const stdoutCap = new TextCapture()
  const stderrCap = new TextCapture()
  resetGlobalStdioState()
  setGlobalStdioProvider(createCustomStdio(new EmptyInputStream(), stdoutCap, stderrCap))
  resetGlobalFilesystem()
  setGlobalFilesystem(buildStdlibFs())

  // 4) Policy: route TCP through the gateway, route DNS through the
  //    gateway too (it does node:dns lookups). Filesystem in-memory.
  // Hit our own local HTTP server through the gateway. Server is known
  // to respond with a small 200 + Connection:close, so we can verify
  // the full send/recv/close loop without depending on an external host.
  //
  // Real HTTPS through ssl_capability + openssl-component +
  // tunneled TCP. Local upstream on 127.0.0.1:28443 (self-signed cert
  // passed in via CERT_PEM env so the python guest can trust it
  // without a hostname-verified CA chain).
  const certPemEscaped = certPem.replace(/\\/g, '\\\\').replace(/\n/g, '\\n').replace(/"/g, '\\"')
  const code = `
import ssl_capability as ssl, socket

print("--- warm-up plain TCP via gateway to 127.0.0.1:28080 ---")
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    s.connect(("127.0.0.1", 28080))
    s.sendall(b"GET / HTTP/1.0\\r\\nHost: local\\r\\nConnection: close\\r\\n\\r\\n")
    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk: break
        data += chunk
        if len(data) > 65536: break
    s.close()
    print(f"  http-ok bytes={len(data)} contains-hello={b'hello, wasm!' in data}")
except Exception as e:
    print(f"  http-err: {type(e).__name__} {e}")

print("--- HTTPS via wrap_bio to 127.0.0.1:28443 (verify_mode=NONE) ---")
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
incoming, outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
obj = ctx.wrap_bio(incoming, outgoing, server_hostname="localhost")

tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
tcp.connect(("127.0.0.1", 28443))
tcp.settimeout(5.0)
print(f"  tcp connected, attempting handshake...")

try:
    iters = 0
    while iters < 20:
        iters += 1
        try:
            obj.do_handshake()
            print(f"  handshake ok after {iters} iters; version={obj.version()}")
            break
        except ssl.SSLWantReadError:
            out = outgoing.read()
            if out:
                print(f"  iter {iters}: sending {len(out)} bytes")
                tcp.sendall(out)
            try:
                data = tcp.recv(16384)
                print(f"  iter {iters}: recv {len(data) if data else 'EOF'}")
                if not data: raise RuntimeError("eof mid-handshake")
                incoming.write(data)
            except socket.timeout:
                print(f"  iter {iters}: recv timeout")
                if not outgoing.pending and not incoming.pending: break
    else:
        raise RuntimeError(f"handshake didn't complete in {iters} iters")
    obj.write(b"GET / HTTP/1.0\\r\\nHost: localhost\\r\\nConnection: close\\r\\n\\r\\n")
    out = outgoing.read()
    if out: tcp.sendall(out)
    body = b""
    for _ in range(20):
        try:
            data = tcp.recv(16384)
        except socket.timeout: break
        if not data: break
        incoming.write(data)
        try:
            chunk = obj.read(16384)
            if chunk: body += chunk
        except ssl.SSLWantReadError: pass
        if len(body) > 32768: break
    tcp.close()
    print(f"  https bytes={len(body)} contains-tls-hello={b'hello, tls + wasm!' in body}")
except Exception as e:
    print(f"  https err: {type(e).__name__} {e}")
`
  const policy = createPolicy({
    defaultAllow: true,
    env: { PYTHONHOME: '/', PYTHONPATH, PYTHONUNBUFFERED: '1' },
    args: ['python', '-c', code],
    overrides: [
      { interface: 'wasi:filesystem/preopens@0.2.0', implementation: 'memory',
        options: { preopens: [{ path: '/' }] } },
      { interface: 'wasi:filesystem/types@0.2.0', implementation: 'memory' },
      { interface: 'wasi:sockets/tcp@0.2.0', implementation: 'tunneled',
        options: { gatewayUrl: GATEWAY_URL } },
      { interface: 'wasi:sockets/tcp-create-socket@0.2.0', implementation: 'tunneled',
        options: { gatewayUrl: GATEWAY_URL } },
      // DNS via the same gateway (DnsQuery frames). This avoids needing
      // global fetch+TLS for DoH and matches what a real browser deploy
      // would prefer when a gateway is already required for TCP.
      { interface: 'wasi:sockets/ip-name-lookup@0.2.0', implementation: 'tunneled',
        options: { gatewayUrl: GATEWAY_URL } },
    ],
  })

  // 5) Boot the wasm
  const pythonModule = await import(path.join(COMPONENT_DIR, 'python.js'))
  const polyfill = new Polyfill({ policy })
  // Debug: confirm the override engages the tunneled TCP impl.
  const tcpCfg = policy.configure({ package: 'wasi:sockets', name: 'tcp', version: '0.2.0' })
  console.log('[debug] TCP override:', JSON.stringify(tcpCfg))
  let exit = 0
  try {
    const { imports } = await polyfill.forInterfaces(WASI_INTERFACES, { jcoCompat: true })
    console.log('[debug] imports keys for sockets:',
      Object.keys(imports).filter((k) => k.includes('sockets')).join(', '))
    const getCoreModule = async (name) => WebAssembly.compile(readFileSync(path.join(COMPONENT_DIR, name)))
    const instance = await pythonModule.instantiate(getCoreModule, imports)
    // JSPI: run() is WebAssembly.promising -> returns a Promise. Await
    // or the wasm guest never finishes.
    await instance.run.run()
  } catch (e) {
    if (e instanceof ComponentExitError) exit = e.status.code
    else { console.error('[harness] uncaught:', e?.stack || e); exit = 1 }
  } finally {
    polyfill.destroy()
  }
  console.log('--- stdout ---')
  console.log(stdoutCap.text.trim())
  if (stderrCap.text.trim()) {
    console.log('--- stderr ---')
    console.log(stderrCap.text.trim().split('\n').slice(-10).join('\n'))
  }
  console.log(`--- exit ${exit} ---`)
  stopAll()
  process.exit(exit)
}

main().catch((e) => { console.error('[harness] fatal:', e?.stack || e); process.exit(1) })

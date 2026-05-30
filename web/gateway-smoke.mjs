// End-to-end smoke for ws-gateway-server.mjs.
//
// Runs the gateway + python wasm in the same process. The polyfill is
// configured to route TCP through the gateway (ws://127.0.0.1:8088/ws)
// just like the browser would. The Python code does a real HTTPS
// request via ssl_capability + cap-routed sockets.
//
//   node web/gateway-smoke.mjs
//
// CURRENT STATUS (2026-05-30): END-TO-END WORKING.
//
//   wasm Python `s.connect((host, port))`   -> tunneled, ok
//   wasm Python `s.sendall(b"GET ...")`      -> reaches upstream
//   upstream sends 200 + Connection:close
//   gateway forwards response back via WS
//   wasm Python `s.recv(4096)`               -> 133 bytes of response,
//                                               body "hello, wasm!"
//
// Requires wasi-polyfill commits: 6e3d429 (createTcpSocket signature)
// + 412b84b (canonical-ABI connect lifecycle + io blocking-op async +
// tuple-2-resource WRAP) + c0f938a (synthetic localAddress) +
// 1e94091 (real readiness Pollable for input-stream subscribe;
// previously always-ready -> wasm tight-spin -> host event-loop
// starved -> 4 GB OOM).
//
// Run with `WASI_POLYFILL_USE_WS_PKG=1` to use the `ws` npm package
// as the WebSocket impl (instead of node's built-in globalThis.WebSocket).
// Both implementations work; the env var is a knob in case JSPI vs.
// node-WebSocket interactions ever regress.

import { spawn } from 'node:child_process'
import { createServer } from 'node:http'
import { readFileSync } from 'node:fs'
import { gunzipSync } from 'node:zlib'
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
// JSPI bundle so pollable.block can actually suspend the wasm guest
// while awaiting the tunneled-TCP connect Promise. Recreate with:
//   npx --prefix web jco transpile build/3.14-current/python.composed.wasm \
//     -o /tmp/python-component-node-jspi --no-nodejs-compat \
//     --instantiation async --name python --async-mode jspi \
//     --async-wasi-imports --async-wasi-exports
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
  // 1a) Upstream in a child process. Co-resident with the wasm event
  //     loop, JSPI suspension wouldn't reliably yield to the http server's
  //     accept/read handlers — Cloudflare-style RST + node OOM ensued.
  const upstream = spawn(process.execPath, [path.join(HERE, 'test-upstream.mjs')], {
    stdio: ['ignore', 'inherit', 'inherit'],
    detached: false,
  })

  // 1b) Start gateway
  const gateway = spawn(process.execPath, [path.join(HERE, 'ws-gateway-server.mjs')], {
    stdio: ['ignore', 'inherit', 'inherit'],
    detached: false,
  })
  const stopAll = () => {
    try { gateway.kill('SIGTERM') } catch {}
    try { upstream.kill('SIGTERM') } catch {}
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
  const code = `
import socket
print("opening plain TCP via gateway to 127.0.0.1:28080 ...")
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(10.0)
try:
    s.connect(("127.0.0.1", 28080))
    print("connected; sending GET")
    s.sendall(b"GET / HTTP/1.0\\r\\nHost: local\\r\\nConnection: close\\r\\n\\r\\n")
    print("recv loop...")
    data = b""
    iters = 0
    while iters < 50:
        iters += 1
        chunk = s.recv(4096)
        print(f"  iter={iters} got={len(chunk)} bytes")
        if not chunk: break
        data += chunk
        if len(data) > 65536: break
    s.close()
    print("done. total bytes:", len(data))
    if data:
        print("first-line:", data.split(b"\\r\\n", 1)[0].decode(errors="replace")[:80])
        print("contains-hello:", b"hello, wasm!" in data)
except Exception as e:
    print("err:", type(e).__name__, e)
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

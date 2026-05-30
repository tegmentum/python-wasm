// End-to-end smoke for ws-gateway-server.mjs.
//
// Runs the gateway + python wasm in the same process. The polyfill is
// configured to route TCP through the gateway (ws://127.0.0.1:8088/ws)
// just like the browser would. The Python code does a real HTTPS
// request via ssl_capability + cap-routed sockets.
//
//   node web/gateway-smoke.mjs
//
// CURRENT STATUS (2026-05-30): the gateway server itself is verified
// working end-to-end at the protocol level (see gateway-client-probe.mjs
// — real HTTP/1.1 200 from example.com via tunneled WebSocket). This
// harness exercises the same gateway via wasi-polyfill's tunneled TCP
// implementation; one of two known polyfill bugs is fixed (see
// memory:ws-gateway-polyfill-tcp-gap), the other still blocks us:
//
//   * FIXED upstream (wasi-polyfill 6e3d429): createTcpSocket had a
//     wrong arg signature (`(network, family)` vs the WIT spec's
//     `(family)`); every call returned InvalidArgument.
//   * NOT FIXED: TunneledTcpInstance does its async work (tunnel open,
//     stream open) inside `finish-connect`, but jco treats that as a
//     sync result-collector — blocking belongs in `pollable.block`.
//     The Promise returned by finish-connect trips guardSyncReturn.
//     Needs a polyfill refactor to move the async into start-connect +
//     a Pollable wired to the result.
//
// Until the second bug is fixed, this harness fails at
// finish-connect; keep it around as a regression check for any future
// integration progress.

import { spawn } from 'node:child_process'
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
const COMPONENT_DIR = path.join(REPO, 'web/public/python-component')
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
  // 1) Start gateway
  const gateway = spawn(process.execPath, [path.join(HERE, 'ws-gateway-server.mjs')], {
    stdio: ['ignore', 'inherit', 'inherit'],
    detached: false,
  })
  const stopGateway = () => { try { gateway.kill('SIGTERM') } catch {} }
  process.on('exit', stopGateway)
  process.on('SIGINT', () => { stopGateway(); process.exit(1) })
  await waitFor(GATEWAY_URL)
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
  // Skip DNS - hit example.com by IP directly. If TCP-through-gateway
  // works for IP connects, DNS is a separable problem.
  const code = `
import socket, ssl_capability as ssl
print("connecting to 93.184.215.14:443 (example.com)...")
ctx = ssl.create_default_context()
ctx.check_hostname = False  # we're using IP, not hostname
ctx.verify_mode = ssl.CERT_NONE
sock = ctx.wrap_socket(host="93.184.215.14", port=443, server_hostname="example.com")
print("version:", sock.version(), "cipher:", sock.cipher()[0])
req = b"GET / HTTP/1.0\\r\\nHost: example.com\\r\\nConnection: close\\r\\n\\r\\n"
sock.write(req)
data = b""
while True:
    chunk = sock.read(4096)
    if not chunk: break
    data += chunk
    if len(data) > 16384: break
print("bytes:", len(data))
print("first-line:", data.split(b"\\r\\n",1)[0].decode())
print("contains-Example:", b"Example Domain" in data)
sock.shutdown()
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
    instance.run.run()
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
  stopGateway()
  process.exit(exit)
}

main().catch((e) => { console.error('[harness] fatal:', e?.stack || e); process.exit(1) })

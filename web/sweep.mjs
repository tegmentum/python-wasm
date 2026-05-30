// Node-side sweep of the jco-transpiled Python component.
//
// Exercises every cap path the browser runner uses, so a failure here is
// a real bug (same wasi-polyfill API; the COMPONENT_DIR bundle is the
// browser-served `web/public/python-component/`).
//
// Use:
//   1) build + compose:        make build && bash scripts/compose-python-component.sh
//   2) transpile (patched):    bash scripts/transpile-component.sh
//   3) run:                    (cd web && node sweep.mjs)
//
// The transpile step applies a `definedResourceTables -> Proxy(true)`
// patch -- otherwise jco's emitted JS leaves the resource table flag
// undefined for cap-defined RTIDs, transferBorrow returns a wrapped
// handle instead of the rep, and the cap's export-wrapper dereferences
// garbage. See scripts/transpile-component.sh for the rationale.

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

const HERE = path.dirname(url.fileURLToPath(import.meta.url))
const REPO = path.resolve(HERE, '..')
// Reuse the browser bundle directly -- it's already the patched output of
// scripts/transpile-component.sh. No separate node transpile needed.
const COMPONENT_DIR = path.join(REPO, 'web/public/python-component')
const STDLIB_TGZ = path.join(REPO, 'web/public/stdlib.tar.gz')
const PYTHONPATH = '/Lib:/cross-build/wasm32-wasip2/build/lib.wasi-wasm32-3.14'

const WASI_INTERFACES = [
  'wasi:cli/environment@0.2.0',
  'wasi:cli/stdin@0.2.0',
  'wasi:cli/stdout@0.2.0',
  'wasi:cli/stderr@0.2.0',
  'wasi:cli/exit@0.2.0',
  'wasi:cli/terminal-input@0.2.0',
  'wasi:cli/terminal-output@0.2.0',
  'wasi:cli/terminal-stdin@0.2.0',
  'wasi:cli/terminal-stdout@0.2.0',
  'wasi:cli/terminal-stderr@0.2.0',
  'wasi:io/streams@0.2.0',
  'wasi:io/poll@0.2.0',
  'wasi:io/error@0.2.0',
  'wasi:clocks/monotonic-clock@0.2.0',
  'wasi:clocks/wall-clock@0.2.0',
  'wasi:random/random@0.2.0',
  'wasi:filesystem/types@0.2.0',
  'wasi:filesystem/preopens@0.2.0',
  'wasi:sockets/network@0.2.0',
  'wasi:sockets/instance-network@0.2.0',
  'wasi:sockets/ip-name-lookup@0.2.0',
  'wasi:sockets/tcp@0.2.0',
  'wasi:sockets/tcp-create-socket@0.2.0',
  'wasi:sockets/udp@0.2.0',
  'wasi:sockets/udp-create-socket@0.2.0',
]

// --- tar.gz reader (minimal, ustar entries only) ---------------------------
function parseUstar(buf) {
  const files = new Map()
  let off = 0
  while (off + 512 <= buf.length) {
    const block = buf.subarray(off, off + 512)
    if (block.every((b) => b === 0)) break
    const name = block.subarray(0, 100).toString('utf8').replace(/\0+$/, '')
    if (!name) break
    const sizeOctal = block.subarray(124, 136).toString('utf8').replace(/\0/g, '').trim()
    const size = parseInt(sizeOctal || '0', 8)
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
  const raw = readFileSync(STDLIB_TGZ)
  const tar = gunzipSync(raw)
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

// --- stdio capture --------------------------------------------------------
// Mirror web/src/output-capture.ts — minimal OutputStreamLike: write +
// flush, both returning Promise<void>. Polyfill builds the wasi:io
// blocking variants on top.
class TextCapture {
  isTTY = false
  constructor() { this.chunks = [] }
  write(bytes)   { this.chunks.push(Buffer.from(bytes).toString('utf8')); return Promise.resolve() }
  flush()        { return Promise.resolve() }
  get text()     { return this.chunks.join('') }
}

// --- one run --------------------------------------------------------------
async function runCode(pythonModule, code) {
  const stdoutCap = new TextCapture()
  const stderrCap = new TextCapture()
  resetGlobalStdioState()
  setGlobalStdioProvider(createCustomStdio(new EmptyInputStream(), stdoutCap, stderrCap))
  resetGlobalFilesystem()
  setGlobalFilesystem(buildStdlibFs())

  const policy = createPolicy({
    defaultAllow: true,
    env: { PYTHONHOME: '/', PYTHONPATH, PYTHONUNBUFFERED: '1' },
    args: ['python', '-c', code],
    overrides: [
      { interface: 'wasi:filesystem/preopens@0.2.0', implementation: 'memory',
        options: { preopens: [{ path: '/' }] } },
      { interface: 'wasi:filesystem/types@0.2.0', implementation: 'memory' },
      // Mirror browser python-runner: DoH for ip-name-lookup (the
      // 'virtual' default returns NotSupported via a Pollable that the
      // polyfill's caller doesn't expect, which can corrupt resource
      // tables on startup even for code paths that never touch the network).
      { interface: 'wasi:sockets/ip-name-lookup@0.2.0', implementation: 'doh',
        options: { dohResolverUrl: 'https://cloudflare-dns.com/dns-query' } },
    ],
  })

  const polyfill = new Polyfill({ policy })
  let exitCode = 0
  try {
    const { imports } = await polyfill.forInterfaces(WASI_INTERFACES, { jcoCompat: true })
    const getCoreModule = async (name) => {
      const bytes = readFileSync(path.join(COMPONENT_DIR, name))
      return await WebAssembly.compile(bytes)
    }
    const instance = await pythonModule.instantiate(getCoreModule, imports)
    instance.run.run()
  } catch (e) {
    if (e instanceof ComponentExitError) {
      exitCode = e.status.code
    } else {
      stderrCap.chunks.push(`\n[harness] uncaught: ${e?.stack || e}\n`)
      exitCode = -1
    }
  } finally {
    polyfill.destroy()
  }
  return { stdout: stdoutCap.text, stderr: stderrCap.text, exitCode }
}

async function main() {
  await registerCorePlugins()
  for (const p of socketPlugins) registerPlugin(p)

  const pythonModule = await import(path.join(COMPONENT_DIR, 'python.js'))

  const checks = [
    ['baseline', 'print("py-ok", 1+1)'],
    ['zlib resource',
     'import zlib; print("zlib", zlib.compress(b"hello world hello world hello world").hex()[:16])'],
    ['sqlite cap',
     'import sqlite3\nc = sqlite3.connect(":memory:")\nprint("sqlite", list(c.execute("select 1+1")))'],
    ['hashlib cap',
     'import hashlib; print("hash", hashlib.sha256(b"abc").hexdigest()[:16])'],
    ['bz2 cap',
     'import bz2; print("bz2", bz2.compress(b"hello world hello world").hex()[:16])'],
  ]

  for (const [label, code] of checks) {
    process.stdout.write(`\n--- ${label} ---\n`)
    const { stdout, stderr, exitCode } = await runCode(pythonModule, code)
    process.stdout.write(`exit=${exitCode}\n`)
    if (stdout) process.stdout.write(`stdout: ${stdout.trim()}\n`)
    if (stderr) process.stdout.write(`stderr: ${stderr.trim().split('\n').slice(-6).join('\n')}\n`)
  }
}

main().catch((e) => { console.error('[harness] fatal:', e); process.exit(1) })

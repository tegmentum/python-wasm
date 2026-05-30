import {
  registerCorePlugins,
  registerPlugin,
  Polyfill,
  createPolicy,
  type Policy,
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
import { CaptureOutputStream } from './output-capture.js'
import { loadStdlib } from './stdlib-loader.js'

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
  // wasi:sockets — required by openssl-component/tls (Phase 3 of the
  // componentize-python plan). Without a wss-gateway URL the polyfill
  // provides virtual stubs that return NotSupported; with one, the
  // ws-gateway plugin tunnels TCP through a WebSocket. See registerSockets().
  'wasi:sockets/network@0.2.0',
  'wasi:sockets/instance-network@0.2.0',
  'wasi:sockets/ip-name-lookup@0.2.0',
  'wasi:sockets/tcp@0.2.0',
  'wasi:sockets/tcp-create-socket@0.2.0',
  'wasi:sockets/udp@0.2.0',
  'wasi:sockets/udp-create-socket@0.2.0',
]

const PYTHONPATH = '/Lib:/cross-build/wasm32-wasip2/build/lib.wasi-wasm32-3.14'

// eslint-disable-next-line @typescript-eslint/no-explicit-any
let pythonModule: any = null
let stdlibFiles: Map<string, Uint8Array> | null = null

export interface RunResult {
  stdout: string
  stderr: string
  exitCode: number
}

/**
 * Initialize the Python runtime: register plugins, load stdlib, import transpiled module.
 */
/**
 * Configuration the host page can provide to enable network access through
 * the polyfill. Phase 3 of the componentize-python plan ships TLS via the
 * openssl-component capability, which imports wasi:sockets/tcp; that has no
 * native browser implementation, so it tunnels over a WebSocket gateway.
 *
 *   tcpGatewayUrl: e.g. "wss://my-gateway.example/ws" — see
 *                  wasi-polyfill/examples/tcp-proxy for a reference impl.
 *
 * If unset, the virtual sockets plugins are registered and any TLS call
 * (or other socket op) returns NotSupported. This is fine for code that
 * doesn't open network connections.
 */
export interface NetworkConfig {
  tcpGatewayUrl?: string
  authToken?: string
}

function getNetworkConfig(): NetworkConfig {
  // Read from Vite env at build time, with safe fallbacks.
  const env = (import.meta as { env?: Record<string, string> }).env ?? {}
  return {
    tcpGatewayUrl: env.VITE_TCP_GATEWAY_URL,
    authToken: env.VITE_TCP_GATEWAY_TOKEN,
  }
}

async function registerSocketsPlugins(
  onProgress?: (message: string) => void,
): Promise<void> {
  // Always register the virtual sockets plugin set first: it covers network/
  // instance-network/ip-name-lookup and provides a baseline TCP/UDP. The
  // ws-gateway plugins below override TCP if a gateway URL is configured.
  for (const plugin of socketPlugins) {
    registerPlugin(plugin)
  }

  const net = getNetworkConfig()
  if (net.tcpGatewayUrl) {
    onProgress?.(`Registering ws-gateway TCP (${net.tcpGatewayUrl})...`)
    // wsGatewayTcpPlugin / wsGatewayTcpCreateSocketPlugin are registered
    // through the polyfill instance with their `options` payload in run().
    // The globalRegistry registration here just claims the interface key
    // so forInterfaces() finds the gateway implementation by default.
    registerPlugin(wsGatewayTcpPlugin)
    registerPlugin(wsGatewayTcpCreateSocketPlugin)
  } else {
    onProgress?.('TCP gateway not configured (set VITE_TCP_GATEWAY_URL to enable HTTPS)')
  }
}

export async function initialize(
  onProgress?: (message: string) => void,
): Promise<void> {
  onProgress?.('Registering WASI plugins...')
  await registerCorePlugins()
  await registerSocketsPlugins(onProgress)

  onProgress?.('Loading Python stdlib...')
  stdlibFiles = await loadStdlib('/stdlib.tar.gz')
  onProgress?.(`Loaded ${stdlibFiles.size} stdlib files`)

  onProgress?.('Loading Python component...')
  // Fetch the jco-transpiled module as text and import via blob URL.
  // Vite refuses to import() files from public/ directly.
  const resp = await fetch('/python-component/python.js')
  const src = await resp.text()
  const blob = new Blob([src], { type: 'application/javascript' })
  const url = URL.createObjectURL(blob)
  pythonModule = await import(/* @vite-ignore */ url)
  URL.revokeObjectURL(url)

  onProgress?.('Ready')
}

function createPythonPolicy(userCode: string): Policy {
  const net = getNetworkConfig()
  const overrides: Array<{
    interface: string
    implementation?: string
    options?: Record<string, unknown>
  }> = [
    {
      interface: 'wasi:filesystem/preopens@0.2.0',
      implementation: 'memory',
      options: { preopens: [{ path: '/' }] },
    },
    {
      interface: 'wasi:filesystem/types@0.2.0',
      implementation: 'memory',
    },
    {
      interface: 'wasi:sockets/ip-name-lookup@0.2.0',
      implementation: 'doh',
      options: { dohResolverUrl: 'https://cloudflare-dns.com/dns-query' },
    },
  ]
  if (net.tcpGatewayUrl) {
    // Route TCP through the WebSocket gateway. The two interfaces share the
    // 'tunneled' implementation registered by wsGateway{Tcp,TcpCreateSocket}Plugin.
    const options: Record<string, unknown> = { gatewayUrl: net.tcpGatewayUrl }
    if (net.authToken) options.authToken = net.authToken
    overrides.push(
      { interface: 'wasi:sockets/tcp@0.2.0', implementation: 'tunneled', options },
      { interface: 'wasi:sockets/tcp-create-socket@0.2.0', implementation: 'tunneled', options },
    )
  }
  return createPolicy({
    defaultAllow: true,
    env: { PYTHONHOME: '/', PYTHONPATH, PYTHONUNBUFFERED: '1' },
    args: ['python', '-c', userCode],
    overrides,
  })
}

/**
 * Populate the in-memory filesystem with stdlib files (first run only).
 */
function buildStdlibFilesystem(): MemoryFileSystem {
  if (!stdlibFiles) {
    throw new Error('stdlib not loaded yet — call initialize() first')
  }
  const fs = new MemoryFileSystem()
  for (const [path, content] of stdlibFiles) {
    // Ensure parent directories exist
    const parts = path.split('/')
    let dirPath = ''
    for (let i = 0; i < parts.length - 1; i++) {
      dirPath += '/' + parts[i]
      const result = fs.getNode(dirPath)
      if (result.tag === 'err') {
        fs.createDirectory(dirPath)
      }
    }
    const filePath = '/' + path
    const createResult = fs.createFile(filePath, { create: true, truncate: true })
    if (createResult.tag === 'ok') {
      createResult.val.content = content
    }
  }
  return fs
}

/**
 * Run Python code and return captured output.
 */
export async function runPython(code: string): Promise<RunResult> {
  if (!pythonModule || !stdlibFiles) {
    throw new Error('Python runtime not initialized. Call initialize() first.')
  }

  const stdoutCapture = new CaptureOutputStream()
  const stderrCapture = new CaptureOutputStream()

  // Reset and configure stdio for this run
  resetGlobalStdioState()
  setGlobalStdioProvider(
    createCustomStdio(new EmptyInputStream(), stdoutCapture, stderrCapture),
  )

  // Per-polyfill FS isolation (wasi-polyfill Phase 2.10): the FS instance
  // gets created lazily inside forInterfaces, so populating after the fact
  // is too late. Stage a pre-populated MemoryFileSystem via
  // setGlobalFilesystem before constructing the Polyfill — the next FS
  // plugin instantiated picks it up.
  resetGlobalFilesystem()
  setGlobalFilesystem(buildStdlibFilesystem())

  const policy = createPythonPolicy(code)
  const polyfill = new Polyfill({ policy })

  let exitCode = 0

  try {
    const { imports } = await polyfill.forInterfaces(WASI_INTERFACES, {
      jcoCompat: true,
    })

    // Provide core module loader for jco instantiation
    const getCoreModule = (name: string) =>
      WebAssembly.compileStreaming(fetch(`/python-component/${name}`))

    const instance = await pythonModule.instantiate(getCoreModule, imports)
    // With sync transpile (default) instance.run.run() returns
    // undefined synchronously. With TRANSPILE_JSPI=1 the export is
    // WebAssembly.promising-wrapped and returns a Promise that must
    // be awaited. `await undefined` is fine, so this works for both.
    await instance.run.run()
  } catch (e: unknown) {
    if (e instanceof ComponentExitError) {
      exitCode = e.status.code
    } else {
      throw e
    }
  } finally {
    polyfill.destroy()
  }

  return {
    stdout: stdoutCapture.getText(),
    stderr: stderrCapture.getText(),
    exitCode,
  }
}

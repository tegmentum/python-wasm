import {
  registerCorePlugins,
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
  getGlobalFilesystemInstance,
  type MemoryFileSystem,
} from '@tegmentum/wasi-polyfill/plugins/filesystem'
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
]

const PYTHONPATH = '/Lib:/cross-build/wasm32-wasip2/build/lib.wasi-wasm32-3.14'

// eslint-disable-next-line @typescript-eslint/no-explicit-any
let pythonModule: any = null
let stdlibFiles: Map<string, Uint8Array> | null = null
let filesystemPopulated = false

export interface RunResult {
  stdout: string
  stderr: string
  exitCode: number
}

/**
 * Initialize the Python runtime: register plugins, load stdlib, import transpiled module.
 */
export async function initialize(
  onProgress?: (message: string) => void,
): Promise<void> {
  onProgress?.('Registering WASI plugins...')
  await registerCorePlugins()

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
  return createPolicy({
    defaultAllow: true,
    env: { PYTHONHOME: '/', PYTHONPATH, PYTHONUNBUFFERED: '1' },
    args: ['python', '-c', userCode],
    overrides: [
      {
        interface: 'wasi:filesystem/preopens@0.2.0',
        implementation: 'memory',
        options: { preopens: [{ path: '/' }] },
      },
      {
        interface: 'wasi:filesystem/types@0.2.0',
        implementation: 'memory',
      },
    ],
  })
}

/**
 * Populate the in-memory filesystem with stdlib files (first run only).
 */
function populateFilesystem(fs: MemoryFileSystem): void {
  if (filesystemPopulated || !stdlibFiles) return

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

    // Create the file
    const filePath = '/' + path
    const createResult = fs.createFile(filePath, { create: true, truncate: true })
    if (createResult.tag === 'ok') {
      createResult.val.content = content
    }
  }

  filesystemPopulated = true
}

/**
 * Run Python code and return captured output.
 */
// Stub classes/functions for sockets interfaces the jco component declares
// but Python never uses in the browser.  Stubs follow jco resource conventions
// (Symbol.for('cabiDispose') on classes) so the generated runtime is satisfied.
const symbolCabiDispose = Symbol.for('cabiDispose')

function stubResource(name: string) {
  const cls = { [name]: class {} }[name]!
  ;(cls as unknown as Record<symbol, () => void>)[symbolCabiDispose] = () => {}
  return cls
}

function stubFunction(msg: string): (...args: unknown[]) => never {
  return function () {
    throw new Error(msg)
  }
}

const socketStubs: Record<string, Record<string, unknown>> = {
  'wasi:sockets/instance-network': {
    instanceNetwork: stubFunction('sockets not available in browser'),
  },
  'wasi:sockets/ip-name-lookup': {
    ResolveAddressStream: stubResource('ResolveAddressStream'),
    resolveAddresses: stubFunction('sockets not available in browser'),
  },
  'wasi:sockets/network': {
    Network: stubResource('Network'),
  },
  'wasi:sockets/tcp': {
    TcpSocket: stubResource('TcpSocket'),
  },
  'wasi:sockets/tcp-create-socket': {
    createTcpSocket: stubFunction('sockets not available in browser'),
  },
  'wasi:sockets/udp': {
    IncomingDatagramStream: stubResource('IncomingDatagramStream'),
    OutgoingDatagramStream: stubResource('OutgoingDatagramStream'),
    UdpSocket: stubResource('UdpSocket'),
  },
  'wasi:sockets/udp-create-socket': {
    createUdpSocket: stubFunction('sockets not available in browser'),
  },
}

function addSocketsStubs(imports: Record<string, Record<string, unknown>>): void {
  for (const [key, value] of Object.entries(socketStubs)) {
    imports[key] ??= value
  }
}

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

  const policy = createPythonPolicy(code)
  const polyfill = new Polyfill({ policy })

  let exitCode = 0

  try {
    const { imports } = await polyfill.forInterfaces(WASI_INTERFACES, {
      jcoCompat: true,
    })

    // The jco-transpiled component unconditionally destructures all declared
    // imports, including sockets.  The polyfill has no sockets plugins, so
    // provide stubs.  Python in the browser won't call these.
    addSocketsStubs(imports)

    // Populate filesystem on first run (singleton persists across runs)
    const fsInstance = getGlobalFilesystemInstance()
    if (fsInstance && !filesystemPopulated) {
      populateFilesystem(fsInstance.getFileSystem())
    }

    // Provide core module loader for jco instantiation
    const getCoreModule = (name: string) =>
      WebAssembly.compileStreaming(fetch(`/python-component/${name}`))

    const instance = await pythonModule.instantiate(getCoreModule, imports)
    instance.run.run()
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

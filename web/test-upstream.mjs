// Tiny HTTP server for gateway-smoke. Runs in a child process so its
// event loop is independent of the wasm guest's (which suspends the
// host loop during JSPI).
//
//   node web/test-upstream.mjs           # listens on 127.0.0.1:28080

import { createServer } from 'node:http'

const HOST = process.env.UPSTREAM_HOST ?? '127.0.0.1'
const PORT = parseInt(process.env.UPSTREAM_PORT ?? '28080', 10)

const server = createServer((req, res) => {
  console.log(`[upstream] ${req.method} ${req.url}`)
  res.writeHead(200, {
    'Content-Type': 'text/plain',
    'Content-Length': '13',
    'Connection': 'close',
  })
  res.end('hello, wasm!\n')
})

server.listen(PORT, HOST, () => {
  console.log(`upstream listening on http://${HOST}:${PORT}`)
})

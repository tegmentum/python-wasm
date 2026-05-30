// HTTPS sibling of test-upstream.mjs for the gateway-smoke. Requires
// UPSTREAM_CERT and UPSTREAM_KEY env vars pointing at PEM files
// (gateway-smoke.mjs generates a self-signed pair on each run).
//
//   UPSTREAM_CERT=cert.pem UPSTREAM_KEY=key.pem node web/test-upstream-https.mjs

import { createServer } from 'node:https'
import { readFileSync } from 'node:fs'

const HOST = process.env.UPSTREAM_HOST ?? '127.0.0.1'
const PORT = parseInt(process.env.UPSTREAM_PORT ?? '28443', 10)
const CERT = process.env.UPSTREAM_CERT
const KEY  = process.env.UPSTREAM_KEY

if (!CERT || !KEY) {
  console.error('test-upstream-https: set UPSTREAM_CERT and UPSTREAM_KEY')
  process.exit(2)
}

const server = createServer(
  { cert: readFileSync(CERT), key: readFileSync(KEY) },
  (req, res) => {
    console.log(`[upstream-https] ${req.method} ${req.url}`)
    res.writeHead(200, {
      'Content-Type': 'text/plain',
      'Content-Length': '20',
      'Connection': 'close',
    })
    res.end('hello, tls + wasm!\n')
  }
)

server.listen(PORT, HOST, () => {
  console.log(`upstream-https listening on https://${HOST}:${PORT}`)
})

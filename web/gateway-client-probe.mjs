// Direct protocol probe of ws-gateway-server.mjs. Bypasses the polyfill
// to confirm the server works at the wire level: Hello -> HelloAck ->
// Open(example.com:80) -> Data(HTTP GET) -> Data(reply) -> Close.

import { WebSocket } from 'ws'
import {
  HEADER_SIZE,
  MessageType,
  Protocol,
  AddressKind,
  Features,
  decodeHeader,
  createFrame,
  createHelloFrame,
  createOpenFrame,
  createDataFrame,
} from '@tegmentum/wasi-polyfill/plugins/ws-gateway'

const ws = new WebSocket('ws://127.0.0.1:8088/ws')
ws.binaryType = 'arraybuffer'

let recv = new Uint8Array(0)
ws.on('open', () => {
  console.log('[client] open; sending HELLO')
  ws.send(createHelloFrame(Features.HalfClose, 32))
})
ws.on('message', (data, isBinary) => {
  if (!isBinary) return
  const chunk = data instanceof Buffer ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength) : new Uint8Array(data)
  const merged = new Uint8Array(recv.length + chunk.length)
  merged.set(recv); merged.set(chunk, recv.length); recv = merged
  while (recv.length >= HEADER_SIZE) {
    const hdr = decodeHeader(recv); if (!hdr) { console.error('bad header'); process.exit(1) }
    if (recv.length < HEADER_SIZE + hdr.payloadLen) break
    const payload = recv.slice(HEADER_SIZE, HEADER_SIZE + hdr.payloadLen)
    recv = recv.slice(HEADER_SIZE + hdr.payloadLen)
    console.log(`[client] frame type=0x${hdr.type.toString(16)} streamId=${hdr.streamId} payloadLen=${hdr.payloadLen}`)
    if (hdr.type === MessageType.HelloAck) {
      const hostBytes = new TextEncoder().encode('example.com')
      ws.send(createOpenFrame(1, { proto: Protocol.Tcp, addrKind: AddressKind.Hostname, port: 80, addr: hostBytes }))
    } else if (hdr.type === MessageType.OpenOk) {
      const req = new TextEncoder().encode('GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n')
      ws.send(createDataFrame(1, req))
    } else if (hdr.type === MessageType.OpenErr) {
      console.error('open err:', new TextDecoder().decode(payload.slice(3)))
      ws.close(); process.exit(1)
    } else if (hdr.type === MessageType.Data) {
      const text = new TextDecoder().decode(payload).slice(0, 80).replace(/\r/g, '\\r').replace(/\n/g, '\\n')
      console.log(`[client]   data: ${text}...`)
    } else if (hdr.type === MessageType.Close) {
      console.log('[client] close received, exit')
      ws.close(); process.exit(0)
    }
  }
})
ws.on('close', () => console.log('[client] ws closed'))
ws.on('error', (e) => { console.error('[client] ws err:', e); process.exit(1) })
setTimeout(() => { console.error('timeout'); process.exit(1) }, 15000)

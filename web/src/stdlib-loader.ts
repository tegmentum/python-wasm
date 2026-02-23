import { gunzipSync } from 'fflate'

/**
 * Fetch, decompress, and parse stdlib.tar.gz into a map of path → file content.
 */
export async function loadStdlib(url: string): Promise<Map<string, Uint8Array>> {
  const response = await fetch(url)
  if (!response.ok) {
    throw new Error(`Failed to fetch stdlib: ${response.status} ${response.statusText}`)
  }

  const raw = new Uint8Array(await response.arrayBuffer())

  // If the server set Content-Encoding: gzip, the browser will have already
  // decompressed the response.  Detect this by checking the gzip magic number.
  const isGzip = raw[0] === 0x1f && raw[1] === 0x8b
  const tar = isGzip ? gunzipSync(raw) : raw

  return parseTar(tar)
}

/**
 * Minimal tar parser. Reads 512-byte headers and extracts regular files.
 * Handles pax extended headers (type flag 'x') by skipping them.
 */
function parseTar(data: Uint8Array): Map<string, Uint8Array> {
  const files = new Map<string, Uint8Array>()
  let offset = 0

  while (offset + 512 <= data.length) {
    // Check for end-of-archive (two consecutive zero-filled blocks)
    const header = data.subarray(offset, offset + 512)
    if (isZeroBlock(header)) break

    const name = readString(header, 0, 100)
    const typeFlag = String.fromCharCode(header[156])
    const sizeStr = readString(header, 124, 12)
    const size = parseInt(sizeStr, 8) || 0

    // Check for UStar prefix (offset 345, 155 bytes)
    const prefix = readString(header, 345, 155)
    const fullPath = prefix ? `${prefix}/${name}` : name

    offset += 512

    if (typeFlag === 'x' || typeFlag === 'g') {
      // Pax extended header or global pax header — skip the data
      offset += Math.ceil(size / 512) * 512
      continue
    }

    if (typeFlag === '0' || typeFlag === '\0') {
      // Regular file
      if (size > 0 && fullPath) {
        const content = data.slice(offset, offset + size)
        // Normalize path: strip leading ./
        const normalized = fullPath.replace(/^\.\//, '')
        files.set(normalized, content)
      }
      offset += Math.ceil(size / 512) * 512
    } else {
      // Directory or other type — skip data blocks
      offset += Math.ceil(size / 512) * 512
    }
  }

  return files
}

function readString(buf: Uint8Array, offset: number, length: number): string {
  let end = offset
  const max = offset + length
  while (end < max && buf[end] !== 0) end++
  return new TextDecoder().decode(buf.subarray(offset, end))
}

function isZeroBlock(block: Uint8Array): boolean {
  for (let i = 0; i < block.length; i++) {
    if (block[i] !== 0) return false
  }
  return true
}

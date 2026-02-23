import type { OutputStreamLike } from '@tegmentum/wasi-polyfill/plugins/cli'

/**
 * Captures output written to a WASI stream, buffering bytes for later retrieval.
 */
export class CaptureOutputStream implements OutputStreamLike {
  readonly isTTY = false
  private chunks: Uint8Array[] = []

  write(chunk: Uint8Array): Promise<void> {
    this.chunks.push(new Uint8Array(chunk))
    return Promise.resolve()
  }

  flush(): Promise<void> {
    return Promise.resolve()
  }

  getText(): string {
    if (this.chunks.length === 0) return ''
    const totalLength = this.chunks.reduce((sum, c) => sum + c.length, 0)
    const merged = new Uint8Array(totalLength)
    let offset = 0
    for (const chunk of this.chunks) {
      merged.set(chunk, offset)
      offset += chunk.length
    }
    return new TextDecoder().decode(merged)
  }
}

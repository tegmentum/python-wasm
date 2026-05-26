#!/usr/bin/env bash
# Componentize-python plan, Phase 4: generate a composectl plan that composes
# python.wasm with the capability multiplexers it imports, and stages the input
# components into the local composectl CAS.
#
# Inputs (in order; their import_name must match what python.wasm declares):
#   - deps/cpython/cross-build/wasm32-wasip2/python.wasm          (root)
#   - compression-multiplexer/.../compression_multiplexer.wasm
#   - crypto-hash-multiplexer/.../crypto_hash_multiplexer.wasm
#   - hashing-multiplexer/.../hashing_multiplexer.wasm
#
# Outputs:
#   - plans/python-browser.json        (composectl plan, sha256-pinned digests)
#   - .compose/blobs/.../*             (CAS blobs as a side effect of `blob put`)
#
# This is the composectl-emit-driven equivalent of scripts/compose-python-component.sh.
# Phase 4 ships both side-by-side: the wac path is the dev fast path, the
# composectl plan is the production / reproducibility path.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PYW="$PROJECT_DIR/deps/cpython/cross-build/wasm32-wasip2/python.wasm"
PLAN="$PROJECT_DIR/plans/python-browser.json"
MUX_COMPRESSION="${COMPRESSION_MULTIPLEXER_WASM:-$HOME/git/compression-multiplexer/target/wasm32-wasip2/release/compression_multiplexer.wasm}"
MUX_CRYPTO_HASH="${CRYPTO_HASH_MULTIPLEXER_WASM:-$HOME/git/crypto-hash-multiplexer/target/wasm32-wasip2/release/crypto_hash_multiplexer.wasm}"
MUX_HASHING="${HASHING_MULTIPLEXER_WASM:-$HOME/git/hashing-multiplexer/target/wasm32-wasip2/release/hashing_multiplexer.wasm}"
COMPOSECTL="${COMPOSECTL:-$HOME/git/webassembly-component-orchestration/target/release/composectl}"

[ -f "$PYW" ] || { echo "build-composectl-plan: $PYW not found — run 'make build' first." >&2; exit 1; }
for f in "$MUX_COMPRESSION" "$MUX_CRYPTO_HASH" "$MUX_HASHING"; do
    [ -f "$f" ] || { echo "build-composectl-plan: capability not found: $f" >&2; exit 1; }
done
[ -x "$COMPOSECTL" ] || { echo "build-composectl-plan: composectl not built at $COMPOSECTL (cargo build --release in webassembly-component-orchestration)" >&2; exit 1; }

mkdir -p "$PROJECT_DIR/plans"

# Stage blobs in CAS and capture sha256 digests as decimal byte arrays. POSIX
# awk on macOS lacks strtonum, so convert hex -> dec via python.
digest_array() {
    shasum -a 256 "$1" | awk '{print $1}' | python3 -c '
import sys
h = sys.stdin.read().strip()
print("[" + ",".join(str(int(h[i:i+2], 16)) for i in range(0, len(h), 2)) + "]")
'
}

echo "==> staging blobs into composectl CAS"
"$COMPOSECTL" blob put "$PYW"             >/dev/null
"$COMPOSECTL" blob put "$MUX_COMPRESSION" >/dev/null
"$COMPOSECTL" blob put "$MUX_CRYPTO_HASH" >/dev/null
"$COMPOSECTL" blob put "$MUX_HASHING"     >/dev/null

PYW_D=$(digest_array "$PYW")
MC_D=$(digest_array "$MUX_COMPRESSION")
MH_D=$(digest_array "$MUX_CRYPTO_HASH")
MX_D=$(digest_array "$MUX_HASHING")

echo "==> emit plan $PLAN"
cat > "$PLAN" <<EOF
{
  "version": "1",
  "root": "python",
  "components": [
    { "id": "python",          "digest": $PYW_D },
    { "id": "compression",     "digest": $MC_D },
    { "id": "crypto-hash",     "digest": $MH_D },
    { "id": "hashing",         "digest": $MX_D }
  ],
  "bindings": [
    {
      "consumer_id": "python",
      "import_name": "tegmentum:compression-multiplexer/compression-dispatcher@0.1.0",
      "provider_id": "compression",
      "export_name": "tegmentum:compression-multiplexer/compression-dispatcher@0.1.0"
    },
    {
      "consumer_id": "python",
      "import_name": "tegmentum:crypto-hash-multiplexer/hash-dispatcher@0.1.0",
      "provider_id": "crypto-hash",
      "export_name": "tegmentum:crypto-hash-multiplexer/hash-dispatcher@0.1.0"
    },
    {
      "consumer_id": "python",
      "import_name": "tegmentum:hashing-multiplexer/hashing-dispatcher@0.1.0",
      "provider_id": "hashing",
      "export_name": "tegmentum:hashing-multiplexer/hashing-dispatcher@0.1.0"
    }
  ],
  "secrets": [],
  "policy": {
    "determinism": "relaxed",
    "capabilities": [],
    "limits": {}
  }
}
EOF

echo "==> validate"
"$COMPOSECTL" plan validate "$PLAN"

echo "OK: $PLAN written + validated."
echo
echo "Note: 'composectl emit build $PLAN -o ...' produces a composed wasm but"
echo "the current composectl emit path silently fails to instantiate dependency"
echo "components (the import list still shows the dep imports). The wac-based"
echo "path -- scripts/compose-python-component.sh / make python-composed --"
echo "wires everything correctly today. The plan is here for reproducibility"
echo "and as the production target once composectl's emit dep-wiring is fixed"
echo "upstream (webassembly-component-orchestration libs/compose-core/src/emit.rs)."

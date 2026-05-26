#!/usr/bin/env bash
# Componentize-python plan, Tier-1 v86 variant: generate a composectl plan
# that composes python.wasm (with the _v86_posix capability extension wired
# in) against the v86 component that exports `v86:posix/process@0.1.0`.
# The composed artifact is what the `native-v86` backend in
# `tegmentum:py-offload/registry` selects when a Python package's native
# path needs the POSIX escape hatch (see docs/tier1-v86-integration.md).
#
# Shape is the python-browser.json plan + one extra component (`v86`) and
# one extra binding (python <- v86:posix/process). All four existing
# multiplexers (compression, crypto-hash, hashing) are also wired so the
# composed artifact is a strict superset of python-browser at the import
# surface.
#
# Inputs (in order; their import_name must match what python.wasm declares):
#   - deps/cpython/cross-build/wasm32-wasip2/python.wasm          (root)
#   - compression-multiplexer/.../compression_multiplexer.wasm
#   - crypto-hash-multiplexer/.../crypto_hash_multiplexer.wasm
#   - hashing-multiplexer/.../hashing_multiplexer.wasm
#   - v86/.../v86.wasm    (must export v86:posix/process@0.1.0)
#
# Outputs:
#   - plans/python-v86.json
#   - .compose/blobs/.../*  (CAS blobs as a side effect of `blob put`)
#
# Status (2026-05-26). v86's posix.wit is contract-only at v0.1.0 — the
# current v86.wasm exports plain wasi:cli/command, NOT v86:posix/process.
# So this script will fail at validation (or earlier, if the v86 wasm
# isn't present at the expected path). The script is ready to use once
# the v86 component starts exporting the world.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PYW="$PROJECT_DIR/deps/cpython/cross-build/wasm32-wasip2/python.wasm"
PLAN="$PROJECT_DIR/plans/python-v86.json"
MUX_COMPRESSION="${COMPRESSION_MULTIPLEXER_WASM:-$HOME/git/compression-multiplexer/target/wasm32-wasip2/release/compression_multiplexer.wasm}"
MUX_CRYPTO_HASH="${CRYPTO_HASH_MULTIPLEXER_WASM:-$HOME/git/crypto-hash-multiplexer/target/wasm32-wasip2/release/crypto_hash_multiplexer.wasm}"
MUX_HASHING="${HASHING_MULTIPLEXER_WASM:-$HOME/git/hashing-multiplexer/target/wasm32-wasip2/release/hashing_multiplexer.wasm}"
V86_COMPONENT="${V86_COMPONENT_WASM:-$HOME/git/v86/target/wasm32-wasip2/release/v86.wasm}"
COMPOSECTL="${COMPOSECTL:-$HOME/git/webassembly-component-orchestration/target/release/composectl}"

[ -f "$PYW" ] || { echo "$(basename "$0"): $PYW not found — run 'make build' first." >&2; exit 1; }
for f in "$MUX_COMPRESSION" "$MUX_CRYPTO_HASH" "$MUX_HASHING"; do
    [ -f "$f" ] || { echo "$(basename "$0"): capability not found: $f" >&2; exit 1; }
done
if [ ! -f "$V86_COMPONENT" ]; then
    cat >&2 <<EOF
$(basename "$0"): v86 component not found at $V86_COMPONENT.

Set V86_COMPONENT_WASM=<path> to point at a v86 component build that
exports the \`v86:posix/process@0.1.0\` interface (see
~/git/v86/wit/posix.wit). The wasm should declare a world that
includes \`export process\` from that package; until v86's Rust side
implements it, no such build exists and this script can't proceed.

Tracking: the contract is in v86 commit cbf77ae (interface-only at
v0.1.0). The Rust impl is item #5 in the Tier-1 v86 task list — see
docs/tier1-v86-integration.md.
EOF
    exit 1
fi
[ -x "$COMPOSECTL" ] || { echo "$(basename "$0"): composectl not built at $COMPOSECTL (cargo build --release in webassembly-component-orchestration)" >&2; exit 1; }

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
"$COMPOSECTL" blob put "$V86_COMPONENT"   >/dev/null

PYW_D=$(digest_array "$PYW")
MC_D=$(digest_array "$MUX_COMPRESSION")
MH_D=$(digest_array "$MUX_CRYPTO_HASH")
MX_D=$(digest_array "$MUX_HASHING")
V86_D=$(digest_array "$V86_COMPONENT")

echo "==> emit plan $PLAN"
cat > "$PLAN" <<EOF
{
  "version": "1",
  "root": "python",
  "components": [
    { "id": "python",          "digest": $PYW_D },
    { "id": "compression",     "digest": $MC_D },
    { "id": "crypto-hash",     "digest": $MH_D },
    { "id": "hashing",         "digest": $MX_D },
    { "id": "v86",             "digest": $V86_D }
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
    },
    {
      "consumer_id": "python",
      "import_name": "v86:posix/process@0.1.0",
      "provider_id": "v86",
      "export_name": "v86:posix/process@0.1.0"
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
echo "Note: per build-composectl-plan.sh, composectl emit currently fails to"
echo "instantiate dependency components (the wac-based scripts/compose-python-component.sh"
echo "is the working dev path). When that's fixed upstream, this plan is what"
echo "composectl will use to produce the python+v86 composed artifact, whose"
echo "content digest becomes the \`native-v86\` env id in the py-offload registry."

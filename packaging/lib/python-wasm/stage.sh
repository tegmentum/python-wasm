#!/usr/bin/env sh
# python-wasm stage — show what `python-wasm rebuild` would pick up.
set -eu

PYTHON_WASM_HOME="${PYTHON_WASM_HOME:-$HOME/.python-wasm}"
SITE_PACKAGES="$PYTHON_WASM_HOME/site-packages"

if [ ! -d "$SITE_PACKAGES" ]; then
    echo "(no extensions staged; $SITE_PACKAGES does not exist)"
    exit 0
fi

python3 - "$SITE_PACKAGES" <<'PYEOF'
import pathlib, sys, tomllib
sp = pathlib.Path(sys.argv[1])
found = list(sorted(sp.rglob("pyforge-pkg.toml")))
if not found:
    print("(no pyforge extensions staged)")
    sys.exit(0)
print(f"{'NAME':<24} {'PATTERN':<10} {'BRIDGE-SRCDIR':<22}  COMPONENT")
for m in found:
    p = tomllib.loads(m.read_text())
    name = p.get("package", {}).get("name", "?")
    pat = p.get("package", {}).get("pattern", "?")
    bridge = m.parent.name
    component = p.get("wheel", {}).get("component", {}).get("artifact", "(none)")
    print(f"{name:<24} {pat:<10} {bridge:<22}  {component}")
print(f"\n{len(found)} extension(s) — run `python-wasm rebuild` to integrate.")
PYEOF

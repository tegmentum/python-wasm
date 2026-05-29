#!/usr/bin/env bash
# Build a python-wasm extension wheel from a cpython-ext bridge source tree.
#
# Phase B of docs/pip-wheel-wrapper-plan.md / implements
# docs/pyforge-wheel-spec.md.
#
# Usage:
#   scripts/build-extension-wheel.sh \
#       --srcdir cpython-ext/_zlib_cap \
#       --component ~/git/zlib-wasm/build/bin/zlib.component.wasm \
#       --python 3.14 \
#       --output dist/
#
# Runs pyforge-pkg-verify on the bridge source tree first; fails fast
# if the manifest doesn't match disk.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SRCDIR=""
COMPONENT=""
PYTHON_MINOR=""
OUTPUT="$PROJECT_DIR/dist/wheels"

while [ $# -gt 0 ]; do
    case "$1" in
        --srcdir)    SRCDIR="$2"; shift 2 ;;
        --component) COMPONENT="$2"; shift 2 ;;
        --python)    PYTHON_MINOR="$2"; shift 2 ;;
        --output)    OUTPUT="$2"; shift 2 ;;
        -h|--help)   sed -n '3,18p' "$0"; exit 0 ;;
        *)           echo "build-extension-wheel: unknown arg: $1" >&2; exit 2 ;;
    esac
done

[ -n "$SRCDIR" ]       || { echo "build-extension-wheel: --srcdir required" >&2; exit 2; }
[ -n "$COMPONENT" ]    || { echo "build-extension-wheel: --component required" >&2; exit 2; }
[ -n "$PYTHON_MINOR" ] || { echo "build-extension-wheel: --python required" >&2; exit 2; }

# Resolve to absolute paths so the verifier + builder don't get confused
# by cwd changes.
SRCDIR="$(cd "$SRCDIR" && pwd)"
COMPONENT="$(cd "$(dirname "$COMPONENT")" && pwd)/$(basename "$COMPONENT")"
mkdir -p "$OUTPUT"
OUTPUT="$(cd "$OUTPUT" && pwd)"

[ -f "$SRCDIR/pyforge-pkg.toml" ] || {
    echo "build-extension-wheel: $SRCDIR/pyforge-pkg.toml missing" >&2; exit 1; }
[ -f "$COMPONENT" ] || {
    echo "build-extension-wheel: component $COMPONENT not found" >&2; exit 1; }

# Verify the bridge declaration matches the on-disk source. This catches
# wheel-time inconsistencies before they get baked into a published artifact.
# The verifier walks all cpython-ext/* dirs; filter to just this one.
python3 - "$SRCDIR" <<'PYEOF'
import pathlib, sys, tomllib
src = pathlib.Path(sys.argv[1])
manifest = src / "pyforge-pkg.toml"
pkg = tomllib.loads(manifest.read_text())
errors = []
if pkg.get("schema") != "tegmentum:pylon-pyforge/pkg@0.1.0":
    errors.append(f"unknown schema {pkg.get('schema')!r}")
package = pkg.get("package", {})
for k in ("name", "version", "pattern"):
    if not package.get(k): errors.append(f"missing [package].{k}")
ext = pkg.get("extension")
if ext:
    for k in ("c_file", "gen_import_c", "gen_import_obj"):
        rel = ext.get(k)
        if not rel:
            errors.append(f"[extension].{k} missing"); continue
        target = src / rel if k == "c_file" else src / "gen" / rel
        if not target.exists():
            errors.append(f"[extension].{k} -> {target} missing")
for i, provides in enumerate(pkg.get("provides", []) or []):
    shim = provides.get("shim")
    if shim:
        target = (src / shim).resolve()
        if not target.exists():
            errors.append(f"[[provides]][{i}].shim -> {target} missing")
if errors:
    print("verify failed:")
    for e in errors: print(" ", e)
    sys.exit(1)
print(f"verify OK: {src.name}")
PYEOF

# Hand off to the Python builder.
WHEEL_PATH="$(python3 "$SCRIPT_DIR/lib/wheel_builder.py" "$SRCDIR" "$COMPONENT" "$PYTHON_MINOR" "$OUTPUT")"
echo "==> built $(du -h "$WHEEL_PATH" | cut -f1) $WHEEL_PATH"

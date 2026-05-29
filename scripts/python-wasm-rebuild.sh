#!/usr/bin/env bash
# Rebuild ~/.python-wasm/python.composed.wasm including any
# pyforge wheels installed under $PYTHON_WASM_HOME/site-packages/.
#
# Phase C of docs/pip-wheel-wrapper-plan.md.
#
# Discovery: glob $PYTHON_WASM_HOME/site-packages/**/pyforge-pkg.toml.
# Each match is a candidate staged extension. For each, copy its bridge
# dir into cpython-ext/ (with a .staged sentinel marking it as wheel-
# sourced), then call build-from-pkgs.sh with the union of base + staged
# extensions in --include.
#
# Conflict policy: staged wheel wins over the in-tree version with the
# same [package].name. Warn loudly.
#
# Idempotent: cleans up its own .staged markers on exit so re-running
# doesn't leak state into the repo.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EXT_DIR="$PROJECT_DIR/cpython-ext"

PYTHON_WASM_HOME="${PYTHON_WASM_HOME:-$HOME/.python-wasm}"
SITE_PACKAGES="$PYTHON_WASM_HOME/site-packages"

PROFILE="${PROFILE:-default}"

if [ ! -d "$SITE_PACKAGES" ]; then
    echo "python-wasm-rebuild: $SITE_PACKAGES does not exist." >&2
    echo "  pip install --target $SITE_PACKAGES <wheel> first, or just rebuild without staged extensions." >&2
    SITE_PACKAGES=""
fi

# Discover staged extensions and pick out the ones with [extension]
# (Pattern A). Shim-only packages also get picked up but pass straight
# through to install-python-shims rather than wire-cpython-ext.
STAGED_BRIDGES=()
STAGED_NAMES=()
STAGED_PYFORGE_DIRS=()
if [ -n "$SITE_PACKAGES" ]; then
    while IFS=$'\t' read -r srcdir pkg_name has_ext; do
        [ -z "$srcdir" ] && continue
        STAGED_BRIDGES+=("$srcdir")
        STAGED_NAMES+=("$pkg_name")
        STAGED_PYFORGE_DIRS+=("$(dirname "$srcdir")")  # the wheel's site-packages root component
    done < <(python3 - "$SITE_PACKAGES" <<'PYEOF'
import pathlib, sys, tomllib
sp = pathlib.Path(sys.argv[1])
for manifest in sorted(sp.rglob("pyforge-pkg.toml")):
    try:
        pkg = tomllib.loads(manifest.read_text())
    except Exception:
        continue
    name = pkg.get("package", {}).get("name")
    if not name:
        continue
    has_ext = "1" if pkg.get("extension") else "0"
    # srcdir = absolute path to the bridge dir (manifest's parent)
    print(f"{manifest.parent}\t{name}\t{has_ext}")
PYEOF
)
fi

echo "==> staged extensions: ${#STAGED_BRIDGES[@]}"

# Stage each into cpython-ext/ with a .staged sentinel. Detect collisions
# with the base in-tree extensions; last-wins is staged.
declare -a STAGED_CLEANUP=()
for i in "${!STAGED_BRIDGES[@]}"; do
    bridge="${STAGED_BRIDGES[$i]}"
    name="${STAGED_NAMES[$i]}"
    srcdir_name="$(basename "$bridge")"
    target="$EXT_DIR/$srcdir_name"
    component_src="$(dirname "$bridge")/${srcdir_name}_component"

    if [ -d "$target" ] && [ ! -e "$target/.staged" ]; then
        echo "WARN: staged wheel '$name' shadows in-tree cpython-ext/$srcdir_name (last-wins)" >&2
        # Back up the in-tree version so we restore on cleanup.
        mv "$target" "${target}.tracked-backup"
        STAGED_CLEANUP+=("$srcdir_name:restore-tracked")
    else
        STAGED_CLEANUP+=("$srcdir_name:remove")
    fi

    cp -R "$bridge" "$target"
    touch "$target/.staged"
    echo "  + staged: $srcdir_name from $bridge"

    # If the wheel ships a component too, expose it via the env-var path
    # scripts/compose-python-component.sh expects. The wheel's manifest
    # records the relative path under [wheel.component].artifact.
    component_rel="$(python3 -c "
import pathlib, tomllib
p = tomllib.loads(pathlib.Path('$target/pyforge-pkg.toml').read_text())
print(p.get('wheel', {}).get('component', {}).get('artifact', ''))
")"
    if [ -n "$component_rel" ]; then
        component_path="$(dirname "$bridge")/$component_rel"
        if [ -f "$component_path" ]; then
            # Map srcdir -> compose env var. The compose script reads
            # specific names (ZLIB_COMPONENT_WASM, etc.); a wheel-shipped
            # extension can override.
            case "$srcdir_name" in
                _zlib_cap)          export ZLIB_COMPONENT_WASM="$component_path" ;;
                _bz2_cap)           export BZIP2_COMPONENT_WASM="$component_path" ;;
                _lzma_cap)          export LZMA_COMPONENT_WASM="$component_path" ;;
                _zstd_cap)          export ZSTD_COMPONENT_WASM="$component_path" ;;
                _lz4_cap)           export LZ4_COMPONENT_WASM="$component_path" ;;
                _openzl_cap)        export OPENZL_COMPONENT_WASM="$component_path" ;;
                _crypto_hash)       export CRYPTO_HASH_MULTIPLEXER_WASM="$component_path" ;;
                _xxhash)            export HASHING_MULTIPLEXER_WASM="$component_path" ;;
                _ssl)               export OPENSSL_COMPONENT_WASM="$component_path" ;;
                _sqlite_capability) export SQLITE_COMPONENT_WASM="$component_path" ;;
                _kdf_cap)           export PASSWORD_HASH_MULTIPLEXER_WASM="$component_path" ;;
                _v86_posix)         export V86_POSIX_COMPONENT_WASM="$component_path" ;;
                *) echo "  ! no compose-env mapping for $srcdir_name; component will not be plugged" >&2 ;;
            esac
            echo "  + component: $component_path"
        else
            echo "  ! [wheel.component].artifact -> $component_path missing" >&2
        fi
    fi
done

cleanup() {
    for entry in "${STAGED_CLEANUP[@]}"; do
        srcdir_name="${entry%%:*}"
        action="${entry##*:}"
        target="$EXT_DIR/$srcdir_name"
        case "$action" in
            remove)            rm -rf "$target" ;;
            restore-tracked)   rm -rf "$target" && mv "${target}.tracked-backup" "$target" ;;
        esac
    done
}
trap cleanup EXIT

# Rebuild. build-from-pkgs.sh re-runs wire-cpython-ext.sh + make build +
# compose. Default include = all extensions (= base + the staged ones
# we just dropped in).
echo "==> invoking build-from-pkgs.sh"
PROFILE="$PROFILE" bash "$SCRIPT_DIR/build-from-pkgs.sh" 2>&1 | tail -3

# Land the artifact at ~/.python-wasm/python.composed.wasm.
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"
COMPOSED="$PROJECT_DIR/$BUILD_DIR/python.composed.wasm"
TARGET="$PYTHON_WASM_HOME/python.composed.wasm"
mkdir -p "$PYTHON_WASM_HOME"
cp "$COMPOSED" "$TARGET"
echo "==> $(du -h "$TARGET" | cut -f1) $TARGET"

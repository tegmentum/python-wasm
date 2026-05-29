#!/usr/bin/env bash
# Phase 8 of docs/coverage-implementation-plan.md.
#
# Build a custom python.composed.wasm with a subset of cpython-ext
# extensions, declared by package name. Output goes to a separate
# build/<variant>/ directory so the default build isn't disturbed.
#
# Usage:
#   ./scripts/build-from-pkgs.sh                       # all pkgs (= default build)
#   ./scripts/build-from-pkgs.sh --variant slim \      # named variant
#       --include zlib-cap,ssl_capability,sqlite-cap   # opt-in list
#   ./scripts/build-from-pkgs.sh --variant nostsd \    # opposite
#       --exclude lz4-cap,openzl-cap,zstd-cap
#
# --include and --exclude take comma-separated [package].name values
# from the cpython-ext/*/pyforge-pkg.toml manifests. They map directly
# to PYFORGE_PKGS_{INCLUDE,EXCLUDE} env vars that wire-cpython-ext.sh
# reads.
#
# This is the Phase 8 "extension recipe" surface — what a future
# pip-style installer would emit when a user declares their dep set.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

VARIANT=""
INCLUDE=""
EXCLUDE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --variant) VARIANT="$2"; shift 2 ;;
        --include) INCLUDE="$2"; shift 2 ;;
        --exclude) EXCLUDE="$2"; shift 2 ;;
        -h|--help)
            sed -n '3,18p' "$0"; exit 0 ;;
        *)
            echo "build-from-pkgs: unknown arg: $1" >&2; exit 2 ;;
    esac
done

PROFILE="${PROFILE:-default}"
eval "$(bash "$SCRIPT_DIR/load-profile.sh" "$PROFILE")"

if [ -n "$VARIANT" ]; then
    OUT_BUILD_DIR="build/${PROFILE_NAME}-${VARIANT}"
else
    OUT_BUILD_DIR="$BUILD_DIR"
fi
mkdir -p "$PROJECT_DIR/$OUT_BUILD_DIR"

echo "==> variant:  ${VARIANT:-(default — overwrites $BUILD_DIR)}"
echo "==> include:  ${INCLUDE:-(all)}"
echo "==> exclude:  ${EXCLUDE:-(none)}"
echo "==> output:   $PROJECT_DIR/$OUT_BUILD_DIR/python.composed.wasm"
echo

# Force a full rebuild — python.wasm needs to relink without the excluded
# extensions. The Makefile's `make build` rule re-runs wire-cpython-ext.sh
# internally, so the env vars need to be passed through to make so the
# re-wire respects them. The clean of python.wasm + the cap object dirs
# is what actually drops the excluded extensions from the link.
export PYFORGE_PKGS_INCLUDE="$INCLUDE"
export PYFORGE_PKGS_EXCLUDE="$EXCLUDE"

rm -f "$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR/cross-build/$HOST_TRIPLE/python.wasm"
if [ -n "$EXCLUDE" ]; then
    # The cap-extension object dirs (Modules/_<srcdir>/) live under the
    # wasi cross-build tree. Drop the ones for excluded packages so the
    # next link doesn't pull their .o files back in.
    for pkg in ${EXCLUDE//,/ }; do
        srcdir=$(python3 -c "
import pathlib, tomllib
for m in pathlib.Path('$PROJECT_DIR/cpython-ext').glob('*/pyforge-pkg.toml'):
    p = tomllib.loads(m.read_text())
    if p.get('package', {}).get('name') == '$pkg' and p.get('extension'):
        print(p['extension']['srcdir']); break
")
        if [ -n "$srcdir" ]; then
            rm -rf "$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR/cross-build/$HOST_TRIPLE/Modules/$srcdir"
            rm -f  "$PROJECT_DIR/deps/$PYTHON_SOURCE_DIR/Modules/$srcdir"
        fi
    done
fi

(cd "$PROJECT_DIR" && PROFILE="$PROFILE" \
    PYFORGE_PKGS_INCLUDE="$INCLUDE" PYFORGE_PKGS_EXCLUDE="$EXCLUDE" \
    make build >/dev/null)

# Compose. Override BUILD_DIR via env so we land in the variant dir.
BUILD_DIR="$OUT_BUILD_DIR" PROFILE="$PROFILE" \
    bash "$SCRIPT_DIR/compose-python-component.sh" 2>&1 | tail -2

# Restore the source-of-truth wire so subsequent default builds still
# include everything.
PYFORGE_PKGS_INCLUDE="" PYFORGE_PKGS_EXCLUDE="" \
    bash "$SCRIPT_DIR/wire-cpython-ext.sh" >/dev/null

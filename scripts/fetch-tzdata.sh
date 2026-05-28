#!/usr/bin/env bash
# Fetch the IANA tzdata package from PyPI and stage it at
# deps/cpython/Lib/tzdata/ so zoneinfo's PEP 615 fallback finds it.
#
# CPython's zoneinfo module searches `TZPATH` (a list of filesystem
# locations like /usr/share/zoneinfo) first, then falls back to the
# `tzdata` Python package if importable. The default python-wasm
# runtime has no TZPATH dirs (no /usr/share inside the sandbox), so
# tzdata-as-a-package is the only viable source.
#
# We use the official `tzdata` package from PyPI (maintained by Paul
# Ganssle / the python core dev who owns zoneinfo). It re-publishes
# the IANA database as a pure-Python wheel; we just unzip it into Lib.
#
# Idempotent: re-runs are no-ops once the target dir exists.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CPYTHON_DIR="$PROJECT_DIR/deps/cpython"
TARGET="$CPYTHON_DIR/Lib/tzdata"
CACHE_DIR="$PROJECT_DIR/deps/tzdata-cache"

# Pin the tzdata version we ship. Bumping = `pip download tzdata
# --no-deps -d /tmp/...` to find the latest, then update this string.
TZDATA_VERSION="2026.2"
WHEEL="tzdata-${TZDATA_VERSION}-py2.py3-none-any.whl"
PYPI_URL="https://pypi.org/simple/tzdata/"

if [ -d "$TARGET" ] && [ -d "$TARGET/zoneinfo" ]; then
    echo "tzdata already installed at $TARGET (zone count: $(ls "$TARGET/zoneinfo" | wc -l | xargs))"
    exit 0
fi

if [ ! -d "$CPYTHON_DIR/Lib" ]; then
    echo "ERROR: $CPYTHON_DIR/Lib missing — run scripts/fetch-cpython.sh first" >&2
    exit 1
fi

mkdir -p "$CACHE_DIR"

if [ ! -f "$CACHE_DIR/$WHEEL" ]; then
    echo "Fetching $WHEEL..."
    # PyPI's simple index uses content-addressed URLs we can derive from
    # the package name. The actual URL needs a hash dir in the path,
    # so use pip to do the fetch — most consumer machines already have
    # python3 + pip.
    python3 -m pip download --no-deps --dest "$CACHE_DIR" \
        "tzdata==$TZDATA_VERSION" 2>&1 | grep -v "^$" | tail -5
    if [ ! -f "$CACHE_DIR/$WHEEL" ]; then
        echo "ERROR: pip download succeeded but $WHEEL not in $CACHE_DIR" >&2
        ls "$CACHE_DIR"
        exit 1
    fi
fi

echo "Extracting $WHEEL -> $TARGET ..."
# tzdata wheel layout: tzdata/__init__.py + tzdata/zoneinfo/<zone-files>.
# We want the tzdata/ dir landing AT Lib/tzdata/, so unzip into Lib/.
TMP_EXTRACT="$(mktemp -d)"
trap 'rm -rf "$TMP_EXTRACT"' EXIT
unzip -q -o "$CACHE_DIR/$WHEEL" -d "$TMP_EXTRACT"

# Copy just the tzdata package (skip the dist-info metadata)
cp -R "$TMP_EXTRACT/tzdata" "$TARGET"

echo "installed: $TARGET ($(ls "$TARGET/zoneinfo" | wc -l | xargs) zones, $(du -sh "$TARGET" | cut -f1))"
echo "verify: python -c 'import zoneinfo; print(zoneinfo.ZoneInfo(\"UTC\"))'  (via your composed.wasm)"

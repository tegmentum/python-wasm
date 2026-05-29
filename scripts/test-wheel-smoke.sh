#!/usr/bin/env bash
# Phase 1 of docs/coverage-implementation-plan.md.
#
# Smoke-test the top-N pure-Python wheels from PyPI: install each one with
# `pip install --no-deps`, then probe `import <name>`. Captures a pass/fail
# row per package.
#
# Default-OFF: requires NETWORK=1 (PyPI access).
set -uo pipefail   # NOT -e — we want to see failures

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -z "${NETWORK:-}" ] && [ -z "${CI_NETWORK_OK:-}" ]; then
    echo "SKIP: network-gated test. Re-run with NETWORK=1 (or CI_NETWORK_OK=1) to enable." >&2
    exit 0
fi

# Top pure-Python packages by recent PyPI install count + diversity of API
# surface (logging/parsing/network/templating/datetime/yaml). Excluded:
# anything with C extensions (numpy, pydantic-core, cryptography, lxml, …)
# — those are Phase 4 of the coverage plan.
PACKAGES=(
    # Already verified by Phase 1 critical path:
    "certifi:certifi"
    "idna:idna"
    "urllib3:urllib3"
    "charset_normalizer:charset_normalizer"
    "requests:requests"
    # Top-N pure-Python:
    "six:six"
    "python-dateutil:dateutil"
    "click:click"
    "jinja2:jinja2"
    "markupsafe:markupsafe"
    "pyyaml:yaml"
    "attrs:attr"
    "packaging:packaging"
    "typing_extensions:typing_extensions"
    "wheel:wheel"
    "toml:toml"
    "tomli:tomli"
    "rich:rich"
    "pygments:pygments"
    "colorama:colorama"
)

pass=0
fail=0
results=()

for entry in "${PACKAGES[@]}"; do
    pkg="${entry%%:*}"
    mod="${entry##*:}"
    printf '%-22s' "$pkg"

    # install (skip if already imports — keep this script fast on rerun)
    if "$SCRIPT_DIR/run-python.sh" -c "import $mod" >/dev/null 2>&1; then
        printf '  cached       '
    else
        if timeout 180 "$SCRIPT_DIR/run-python.sh" -m pip install \
                --use-deprecated=legacy-certs --no-deps --target /site-packages \
                --no-cache-dir --quiet "$pkg" >/dev/null 2>&1; then
            printf '  installed    '
        else
            echo "FAIL (install)"
            fail=$((fail + 1))
            results+=("FAIL install: $pkg")
            continue
        fi
    fi

    # import probe
    if "$SCRIPT_DIR/run-python.sh" -c "import $mod" >/dev/null 2>&1; then
        echo "OK   import $mod"
        pass=$((pass + 1))
        results+=("OK: $pkg")
    else
        err="$("$SCRIPT_DIR/run-python.sh" -c "import $mod" 2>&1 | tail -1)"
        echo "FAIL import $mod  ($err)"
        fail=$((fail + 1))
        results+=("FAIL import: $pkg ($err)")
    fi
done

echo
echo "===== summary ====="
echo "$pass pass / $((pass + fail)) total"
[ $fail -gt 0 ] && exit 1 || exit 0

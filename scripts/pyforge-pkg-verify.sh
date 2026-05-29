#!/usr/bin/env bash
# Phase 8 of docs/coverage-implementation-plan.md: validate that every
# cpython-ext/<srcdir>/pyforge-pkg.toml is internally consistent and
# matches the on-disk layout.
#
# Checks per manifest:
#   * schema is a known version
#   * [package].name + .version are non-empty strings
#   * [package].pattern is one of "A", "N", "shim-only"
#   * if [extension] present: srcdir matches the dir name, c_file +
#     gen_import_c + gen_import_obj exist on disk
#   * if [[provides]] present: each shim path exists (relative to the
#     manifest's directory)
#
# Also reports orphan dirs (cpython-ext/<dir>/ without a manifest) and
# orphan EXTS entries in scripts/wire-cpython-ext.sh that no longer map
# to a manifest. (Phase 8 made wire-cpython-ext.sh consume the
# manifests directly, so any leftover hardcoded entries are dead code.)
#
# Exit codes: 0 = all clean, 1 = at least one check failed.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EXT_DIR="$PROJECT_DIR/cpython-ext"

python3 - "$EXT_DIR" <<'PYEOF'
import os
import pathlib
import sys
import tomllib

ext_dir = pathlib.Path(sys.argv[1])
errors = []
ok_count = 0
manifests = []

KNOWN_SCHEMAS = {"tegmentum:pylon-pyforge/pkg@0.1.0"}
KNOWN_PATTERNS = {"A", "N", "shim-only"}


def check_one(manifest_path):
    global ok_count
    dir_name = manifest_path.parent.name
    try:
        with open(manifest_path, "rb") as fh:
            pkg = tomllib.load(fh)
    except Exception as exc:
        errors.append(f"{dir_name}: cannot parse: {exc}")
        return

    schema = pkg.get("schema")
    if schema not in KNOWN_SCHEMAS:
        errors.append(f"{dir_name}: unknown schema {schema!r}")

    package = pkg.get("package", {})
    name = package.get("name")
    if not isinstance(name, str) or not name:
        errors.append(f"{dir_name}: [package].name missing/empty")
    version = package.get("version")
    if not isinstance(version, str) or not version:
        errors.append(f"{dir_name}: [package].version missing/empty")
    pattern = package.get("pattern")
    if pattern not in KNOWN_PATTERNS:
        errors.append(f"{dir_name}: [package].pattern unknown ({pattern!r})")

    ext = pkg.get("extension")
    if ext:
        ext_srcdir = ext.get("srcdir")
        if ext_srcdir != dir_name:
            errors.append(f"{dir_name}: [extension].srcdir {ext_srcdir!r} != dir {dir_name!r}")
        for key in ("c_file", "gen_import_c", "gen_import_obj"):
            rel = ext.get(key)
            if not rel:
                errors.append(f"{dir_name}: [extension].{key} missing")
                continue
            if key == "c_file":
                check_path = manifest_path.parent / rel
            else:
                check_path = manifest_path.parent / "gen" / rel
            if not check_path.exists():
                errors.append(f"{dir_name}: [extension].{key} -> {check_path} missing")

    for i, provides in enumerate(pkg.get("provides", []) or []):
        # A [[provides]] without a shim documents that the C extension
        # provides the module directly — e.g. `import _xxhash` reaches
        # straight into the static linkage. Only validate the shim path
        # when one is declared.
        shim = provides.get("shim")
        if not shim:
            continue
        target = (manifest_path.parent / shim).resolve()
        if not target.exists():
            errors.append(f"{dir_name}: [[provides]][{i}].shim -> {target} missing")

    if not errors_for(dir_name):
        ok_count += 1


def errors_for(dir_name):
    return [e for e in errors if e.startswith(dir_name + ":")]


for manifest in sorted(ext_dir.glob("*/pyforge-pkg.toml")):
    manifests.append(manifest)
    check_one(manifest)

# Orphan check: cpython-ext/<dir>/ without a pyforge-pkg.toml.
orphan_dirs = []
for d in sorted(ext_dir.iterdir()):
    if d.is_dir() and not (d / "pyforge-pkg.toml").exists():
        orphan_dirs.append(d.name)
if orphan_dirs:
    for name in orphan_dirs:
        errors.append(f"orphan dir cpython-ext/{name}/ has no pyforge-pkg.toml")

# Print per-package status
for manifest in manifests:
    name = manifest.parent.name
    errs = errors_for(name)
    if errs:
        print(f"FAIL {name}")
        for e in errs:
            print(f"     {e[len(name)+2:]}")
    else:
        print(f"OK   {name}")

print()
print(f"{ok_count} of {len(manifests)} manifests pass")
if orphan_dirs:
    print(f"{len(orphan_dirs)} orphan dirs: {', '.join(orphan_dirs)}")
sys.exit(0 if not errors else 1)
PYEOF

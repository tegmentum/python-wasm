#!/usr/bin/env sh
# python-wasm extensions — list / remove installed pyforge-wheel extensions.
set -eu

PYTHON_WASM_HOME="${PYTHON_WASM_HOME:-$HOME/.python-wasm}"
SITE_PACKAGES="$PYTHON_WASM_HOME/site-packages"

case "${1:-list}" in
    list)
        if [ ! -d "$SITE_PACKAGES" ]; then
            echo "(no extensions installed; $SITE_PACKAGES does not exist)"
            exit 0
        fi
        python3 - "$SITE_PACKAGES" <<'PYEOF'
import pathlib, sys, tomllib
sp = pathlib.Path(sys.argv[1])
rows = []
for manifest in sorted(sp.rglob("pyforge-pkg.toml")):
    try: pkg = tomllib.loads(manifest.read_text())
    except Exception as e: rows.append(("?", "?", "?", manifest.parent.name, f"parse error: {e}")); continue
    p = pkg.get("package", {})
    rows.append((p.get("name", "?"), p.get("version", "?"), p.get("pattern", "?"),
                 manifest.parent.name, p.get("description", "")[:50]))
if not rows:
    print("(no pyforge extensions installed)")
else:
    print(f"{'NAME':<24} {'VERSION':<10} {'PATTERN':<10} {'SRCDIR':<22}  DESCRIPTION")
    for row in rows:
        print(f"{row[0]:<24} {row[1]:<10} {row[2]:<10} {row[3]:<22}  {row[4]}")
PYEOF
        ;;
    remove)
        [ $# -ge 2 ] || { echo "usage: python-wasm extensions remove <pkg-name>" >&2; exit 2; }
        target_name="$2"
        if [ ! -d "$SITE_PACKAGES" ]; then
            echo "(no extensions installed)"
            exit 1
        fi
        # Find the dist-info for this name and pip-uninstall semantics: delete
        # everything RECORD says belongs to the distribution.
        python3 - "$SITE_PACKAGES" "$target_name" <<'PYEOF'
import pathlib, sys
sp, name = pathlib.Path(sys.argv[1]), sys.argv[2]
# Look for <name>-*.dist-info (with hyphens normalized).
norm = name.replace("-", "_")
matches = list(sp.glob(f"{norm}-*.dist-info"))
matches += list(sp.glob(f"{name}-*.dist-info"))
if not matches:
    print(f"python-wasm extensions remove: no installed distribution matches {name!r}")
    sys.exit(1)
for dist_info in matches:
    record_path = dist_info / "RECORD"
    if not record_path.exists():
        print(f"warn: {dist_info} has no RECORD; skipping")
        continue
    for line in record_path.read_text().splitlines():
        rel = line.split(",", 1)[0]
        if not rel: continue
        target = sp / rel
        if target.exists() and target.is_file():
            target.unlink()
    # Empty-prune any dirs the wheel created.
    for sub in sorted({(sp / line.split(",", 1)[0]).parent for line in record_path.read_text().splitlines() if "," in line}, key=lambda p: -len(p.parts)):
        if sub.is_dir() and not any(sub.iterdir()):
            sub.rmdir()
    print(f"removed: {dist_info.name}")
print("\nrun `python-wasm rebuild` to recompose without the removed extension")
PYEOF
        ;;
    *)
        echo "usage: python-wasm extensions [list | remove <pkg-name>]" >&2
        exit 2 ;;
esac

"""
Build a python-wasm extension wheel from a cpython-ext source tree.

Implements docs/pyforge-wheel-spec.md. Pure stdlib (tomllib, zipfile,
hashlib, base64) so it runs in any Python 3.11+ env without
dependencies.

Driven by scripts/build-extension-wheel.sh — that script handles
flag parsing + pyforge-pkg-verify invocation, then calls
wheel_builder.build() with the resolved inputs.
"""

from __future__ import annotations

import base64
import hashlib
import pathlib
import shutil
import sys
import tempfile
import tomllib
import zipfile


SPEC_VERSION = "0.1.0"
GENERATOR = f"python-wasm-build/{SPEC_VERSION}"

# Normalize PEP 503 distribution name to a wheel filename component.
# Per PEP 427, `-` is forbidden in filenames; we replace with `_` so
# `pyforge-pkg` -> `pyforge_pkg` etc.
def _filename_safe(name: str) -> str:
    return name.replace("-", "_").replace(".", "_")


def _sha256_b64(data: bytes) -> str:
    """PEP 376 RECORD hash format: urlsafe base64 of sha256, no padding."""
    return base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()


def _read_manifest(bridge_dir: pathlib.Path) -> dict:
    manifest = bridge_dir / "pyforge-pkg.toml"
    if not manifest.exists():
        raise FileNotFoundError(f"{bridge_dir}/pyforge-pkg.toml missing")
    with open(manifest, "rb") as fh:
        return tomllib.load(fh)


def _ensure_wheel_component_block(pkg: dict, component_path: pathlib.Path,
                                  bridge_dirname: str) -> dict:
    """Inject [wheel.component] if it's not already in the manifest.

    The in-tree pyforge-pkg.toml files in this repo predate the wheel
    spec and don't carry the [wheel.component] block. The wheel needs
    it so python-wasm rebuild can find the component artifact inside
    the installed wheel.
    """
    wheel = pkg.setdefault("wheel", {})
    component = wheel.setdefault("component", {})
    if "artifact" not in component:
        component["artifact"] = f"{bridge_dirname}_component/{component_path.name}"
    return pkg


def _emit_manifest(out_path: pathlib.Path, pkg: dict) -> None:
    """Re-emit the manifest with the [wheel.component] block included.

    Tomllib is read-only; serialize by hand. The format is small and the
    block ordering matters less than fidelity, so just emit each top-level
    key in order.
    """
    lines: list[str] = []
    lines.append(f'schema = "{pkg["schema"]}"')
    lines.append("")

    def emit_table(name: str, table: dict) -> None:
        lines.append(f"[{name}]")
        for k, v in table.items():
            if isinstance(v, str):
                lines.append(f'{k} = "{v}"')
            elif isinstance(v, bool):
                lines.append(f"{k} = {str(v).lower()}")
            elif isinstance(v, (int, float)):
                lines.append(f"{k} = {v}")
            elif isinstance(v, list):
                lines.append(f"{k} = {v!r}")
        lines.append("")

    if "package" in pkg:
        emit_table("package", pkg["package"])
    if "extension" in pkg:
        emit_table("extension", pkg["extension"])

    # capabilities.required is an array-of-tables
    for cap in pkg.get("capabilities", {}).get("required", []) or []:
        lines.append("[[capabilities.required]]")
        for k, v in cap.items():
            if isinstance(v, str):
                lines.append(f'{k} = "{v}"')
            else:
                lines.append(f"{k} = {v!r}")
        lines.append("")

    # provides is an array-of-tables; preserve all fields
    for prov in pkg.get("provides", []) or []:
        lines.append("[[provides]]")
        for k, v in prov.items():
            if isinstance(v, str):
                lines.append(f'{k} = "{v}"')
            elif isinstance(v, list):
                # `offload` is a list of inline tables; serialize via TOML inline syntax
                items = ", ".join(
                    "{ " + ", ".join(
                        f'{ik} = "{iv}"' if isinstance(iv, str) else f"{ik} = {iv!r}"
                        for ik, iv in entry.items()
                    ) + " }"
                    for entry in v
                )
                lines.append(f"{k} = [{items}]")
            else:
                lines.append(f"{k} = {v!r}")
        lines.append("")

    if "wheel" in pkg and "component" in pkg["wheel"]:
        emit_table("wheel.component", pkg["wheel"]["component"])

    if "gating" in pkg:
        emit_table("gating", pkg["gating"])

    out_path.write_text("\n".join(lines))


def _build_metadata(pkg: dict, python_minor: str) -> str:
    """Generate PEP-621 METADATA from pyforge-pkg.toml's [package] block.

    python_minor is e.g. "3.14"; encoded as Requires-Python.
    """
    package = pkg.get("package", {})
    name = package.get("name", "unknown")
    version = package.get("version", "0.0.0")
    summary = package.get("description", "")
    major, minor = python_minor.split(".")
    next_minor = f"{major}.{int(minor)+1}"

    lines = [
        "Metadata-Version: 2.1",
        f"Name: {name}",
        f"Version: {version}",
        f"Summary: {summary}",
        "Platform: wasm32-wasip2",
        f"Requires-Python: >={python_minor},<{next_minor}",
    ]
    if author := package.get("author"):
        lines.append(f"Author: {author}")
    if email := package.get("author_email"):
        lines.append(f"Author-email: {email}")
    if license_ := package.get("license"):
        lines.append(f"License: {license_}")
    return "\n".join(lines) + "\n"


def _build_wheel_metadata(tag: str) -> str:
    return (
        "Wheel-Version: 1.0\n"
        f"Generator: {GENERATOR}\n"
        "Root-Is-Purelib: false\n"
        f"Tag: {tag}\n"
    )


def _build_record(stage_dir: pathlib.Path, record_relpath: pathlib.Path) -> str:
    """PEP 376 RECORD: per-file SHA-256 + size, plus a final unhashed RECORD line."""
    lines: list[str] = []
    record_abs = stage_dir / record_relpath
    for path in sorted(stage_dir.rglob("*")):
        if not path.is_file() or path == record_abs:
            continue
        rel = path.relative_to(stage_dir).as_posix()
        data = path.read_bytes()
        lines.append(f"{rel},sha256={_sha256_b64(data)},{len(data)}")
    lines.append(f"{record_relpath.as_posix()},,")
    return "\n".join(lines) + "\n"


def build(
    *,
    bridge_dir: pathlib.Path,
    component_path: pathlib.Path,
    python_minor: str,
    output_dir: pathlib.Path,
    pyshim_extras: dict[str, pathlib.Path] | None = None,
) -> pathlib.Path:
    """Build the wheel; return the output path.

    bridge_dir: cpython-ext/_<name>/ source tree (the bridge).
    component_path: path to the .wasm capability component the bridge imports.
    python_minor: e.g. "3.14". One Python minor per wheel.
    output_dir: where to drop the .whl.
    pyshim_extras: optional {module-name -> path} for Lib/ overlays that
        the wheel should also ship as a regular Python package (Pattern A
        extensions with a stdlib shim).
    """
    pkg = _read_manifest(bridge_dir)
    package = pkg.get("package", {})
    name = package.get("name")
    version = package.get("version")
    if not name or not version:
        raise ValueError(f"{bridge_dir}/pyforge-pkg.toml missing [package].name or .version")

    py_tag = f"cp{python_minor.replace('.', '')}"  # cp314
    tag = f"{py_tag}-{py_tag}-wasm32_wasip2"
    wheel_name = f"{_filename_safe(name)}-{version}-{tag}.whl"
    bridge_dirname = bridge_dir.name  # e.g. "_zlib_cap"

    with tempfile.TemporaryDirectory(prefix="pyforge-wheel-") as tmp:
        stage = pathlib.Path(tmp)

        # 1. Copy bridge sources, excluding cruft (.compiled .o files are
        # legitimate, but skip __pycache__ and editor backups).
        target_bridge = stage / bridge_dirname
        shutil.copytree(
            bridge_dir, target_bridge,
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".*.swp"),
        )

        # 2. Re-emit the manifest with the [wheel.component] block guaranteed.
        pkg_with_wheel = _ensure_wheel_component_block(pkg, component_path, bridge_dirname)
        _emit_manifest(target_bridge / "pyforge-pkg.toml", pkg_with_wheel)

        # 3. Copy the capability component.
        component_dir = stage / f"{bridge_dirname}_component"
        component_dir.mkdir()
        shutil.copy2(component_path, component_dir / component_path.name)

        # 4. Optional Python shim packages (Lib/ overlays).
        for shim_pkg, shim_src in (pyshim_extras or {}).items():
            shutil.copytree(shim_src, stage / shim_pkg)

        # 5. dist-info: METADATA, WHEEL, then RECORD last so RECORD covers
        # the previous two.
        dist_info_name = f"{_filename_safe(name)}-{version}.dist-info"
        dist_info = stage / dist_info_name
        dist_info.mkdir()
        (dist_info / "METADATA").write_text(_build_metadata(pkg, python_minor))
        (dist_info / "WHEEL").write_text(_build_wheel_metadata(tag))
        record_relpath = pathlib.Path(dist_info_name) / "RECORD"
        (dist_info / "RECORD").write_text(_build_record(stage, record_relpath))

        # 6. Zip into a .whl.
        output_dir.mkdir(parents=True, exist_ok=True)
        output = output_dir / wheel_name
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as wheel:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    wheel.write(path, path.relative_to(stage).as_posix())

    return output


if __name__ == "__main__":
    # Smoke-test entry point — full invocation is via scripts/build-extension-wheel.sh.
    if len(sys.argv) < 5:
        print("usage: wheel_builder.py <bridge_dir> <component.wasm> <python_minor> <output_dir>",
              file=sys.stderr)
        sys.exit(2)
    out = build(
        bridge_dir=pathlib.Path(sys.argv[1]).resolve(),
        component_path=pathlib.Path(sys.argv[2]).resolve(),
        python_minor=sys.argv[3],
        output_dir=pathlib.Path(sys.argv[4]).resolve(),
    )
    print(out)

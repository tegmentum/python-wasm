#!/usr/bin/env bash
# load-profile.sh: parse a profile TOML and emit env-style KEY=VALUE lines.
#
# Usage:
#   bash scripts/load-profile.sh 3.14-current
#   bash scripts/load-profile.sh path/to/profile.toml
#
# Both forms resolve to a TOML file under profiles/. Output is the resolved
# variables consumers (Makefile, other scripts) need:
#
#   PROFILE_NAME=<basename>
#   PYTHON_VERSION=3.14.3
#   PYTHON_SOURCE_DIR=cpython-3.14
#   WASI_SDK_VERSION=33.0
#   WASI_SDK_DIR=wasi-sdk-33.0-arm64-macos
#   HOST_TRIPLE=wasm32-wasip2
#   STATIC_OPENSSL=0|1
#   STATIC_ZLIB=0|1
#   WITH_V86_POSIX=0|1
#   BUILD_DIR=build/<profile>
#   CAP_COMPRESSION_MULTIPLEXER=<path>
#   CAP_CRYPTO_HASH_MULTIPLEXER=<path>
#   ...
#
# Consumers source this output as `eval "$(bash scripts/load-profile.sh
# 3.14-current)"` (bash) or with Make's $(shell ...) (Makefile).
#
# String values that contain `${HOME}` etc. are expanded against the
# current shell environment so cap paths in profile TOML can stay portable.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PROFILE_ARG="${1:-}"
if [ -z "$PROFILE_ARG" ]; then
    echo "load-profile: usage: $0 <profile-name-or-path>" >&2
    exit 2
fi

# Resolve the profile arg to an absolute TOML path.
if [ -f "$PROFILE_ARG" ]; then
    PROFILE_FILE="$PROFILE_ARG"
else
    PROFILE_FILE="$PROJECT_DIR/profiles/${PROFILE_ARG}.toml"
fi
if [ ! -f "$PROFILE_FILE" ]; then
    echo "load-profile: profile not found: $PROFILE_FILE" >&2
    exit 1
fi

# Derive PROFILE_NAME (the basename without .toml).
PROFILE_NAME="$(basename "$PROFILE_FILE" .toml)"

# Use the host Python to parse the TOML (Python 3.11+ has tomllib in stdlib;
# we already require python3 elsewhere in the build).
exec python3 - "$PROFILE_FILE" "$PROFILE_NAME" <<'PYEOF'
import os
import shlex
import sys
import tomllib

profile_file, profile_name = sys.argv[1], sys.argv[2]
with open(profile_file, "rb") as fh:
    profile = tomllib.load(fh)


def env_expand(value: str) -> str:
    """Expand $HOME / ${HOME} / $USER etc. against the current process env."""
    return os.path.expandvars(value)


def emit(key: str, value) -> None:
    if isinstance(value, bool):
        value = "1" if value else "0"
    elif isinstance(value, (int, float)):
        value = str(value)
    elif isinstance(value, str):
        value = env_expand(value)
    else:
        raise TypeError(f"unsupported profile value for {key}: {value!r}")
    print(f"{key}={shlex.quote(value)}")


emit("PROFILE_NAME", profile_name)

python_cfg = profile.get("python", {})
emit("PYTHON_VERSION", python_cfg.get("version", ""))
emit("PYTHON_SOURCE_DIR", python_cfg.get("source_dir", "cpython"))

toolchain = profile.get("toolchain", {})
emit("WASI_SDK_VERSION", toolchain.get("wasi_sdk_version", ""))
emit("WASI_SDK_DIR", toolchain.get("wasi_sdk_dir", ""))
emit("HOST_TRIPLE", toolchain.get("host_triple", "wasm32-wasip2"))

build = profile.get("build", {})
emit("STATIC_OPENSSL", build.get("static_openssl", False))
emit("STATIC_ZLIB", build.get("static_zlib", False))
emit("WITH_V86_POSIX", build.get("with_v86_posix", True))

caps = profile.get("capabilities", {})
# Map TOML key (snake_case) -> the env var the compose script expects.
cap_var_map = {
    "zlib_component": "ZLIB_COMPONENT_WASM",
    "bzip2_component": "BZIP2_COMPONENT_WASM",
    "lzma_component": "LZMA_COMPONENT_WASM",
    "zstd_component": "ZSTD_COMPONENT_WASM",
    "lz4_component": "LZ4_COMPONENT_WASM",
    "openzl_component": "OPENZL_COMPONENT_WASM",
    "crypto_hash_multiplexer": "CRYPTO_HASH_MULTIPLEXER_WASM",
    "hashing_multiplexer": "HASHING_MULTIPLEXER_WASM",
    "openssl_component": "OPENSSL_COMPONENT_WASM",
    "sqlite_component": "SQLITE_COMPONENT_WASM",
    "password_hash_multiplexer": "PASSWORD_HASH_MULTIPLEXER_WASM",
    "v86_posix_component": "V86_POSIX_COMPONENT_WASM",
}
for toml_key, env_var in cap_var_map.items():
    if toml_key in caps:
        emit(env_var, caps[toml_key])

output = profile.get("output", {})
emit("BUILD_DIR", output.get("build_dir", f"build/{profile_name}"))
PYEOF

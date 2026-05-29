#!/usr/bin/env sh
# python-wasm installer.
#
# Usage:
#   curl -fsSL https://python-wasm.dev/install.sh | sh
#   curl -fsSL https://python-wasm.dev/install.sh | sh -s -- --version 0.1.0
#   curl -fsSL https://python-wasm.dev/install.sh | sh -s -- --prefix ~/python-wasm
#   curl -fsSL https://python-wasm.dev/install.sh | sh -s -- --no-modify-path
#
# Same shape as rustup / uv / bun installers. POSIX sh, no bashisms.
set -eu

# --- defaults ---------------------------------------------------------------

VERSION="${PYTHON_WASM_VERSION:-latest}"
PREFIX="${PYTHON_WASM_PREFIX:-$HOME/.python-wasm}"
BIN_DIR="${PYTHON_WASM_BIN:-$HOME/.local/bin}"
MODIFY_PATH=1
RELEASE_BASE="${PYTHON_WASM_RELEASE_BASE:-https://github.com/tegmentum/python-wasm/releases}"
USER_AGENT="python-wasm-install/0.1.0"

# --- arg parse --------------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        --version)         VERSION="$2"; shift 2 ;;
        --prefix)          PREFIX="$2"; shift 2 ;;
        --bin-dir)         BIN_DIR="$2"; shift 2 ;;
        --no-modify-path)  MODIFY_PATH=0; shift ;;
        --release-base)    RELEASE_BASE="$2"; shift 2 ;;
        -h|--help)
            cat <<EOF
Usage: install.sh [OPTIONS]

OPTIONS:
  --version <v>        version to install (default: latest)
  --prefix <dir>       install root (default: ~/.python-wasm)
  --bin-dir <dir>      where to symlink python-wasm (default: ~/.local/bin)
  --no-modify-path     don't touch ~/.profile / ~/.bashrc / ~/.zshrc
  --release-base <url> alternative release URL base
EOF
            exit 0 ;;
        *) echo "install.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

# --- platform detection -----------------------------------------------------

uname_s="$(uname -s)"
uname_m="$(uname -m)"

case "$uname_s" in
    Darwin) os="darwin" ;;
    Linux)  os="linux"  ;;
    *)      echo "install.sh: unsupported OS: $uname_s" >&2; exit 1 ;;
esac

case "$uname_m" in
    arm64|aarch64)   arch="arm64"  ;;
    x86_64|amd64)    arch="x86_64" ;;
    *)               echo "install.sh: unsupported architecture: $uname_m" >&2; exit 1 ;;
esac

PLATFORM="$os-$arch"

# --- preflight --------------------------------------------------------------

# wasmtime is required by the launcher; warn early instead of installing
# a broken tool.
if ! command -v wasmtime >/dev/null 2>&1; then
    cat <<EOF
WARNING: wasmtime is not on your PATH. python-wasm needs it to run.

Install with one of:
  brew install wasmtime
  curl https://wasmtime.dev/install.sh -sSf | bash
  cargo install wasmtime-cli

Continuing the python-wasm install anyway; you'll get a clear error
on the first invocation if wasmtime is still missing.

EOF
fi

# Resolve the latest version if requested.
if [ "$VERSION" = "latest" ]; then
    LATEST_URL="$RELEASE_BASE/latest/download/VERSION"
    VERSION="$(curl -fsSL -H "User-Agent: $USER_AGENT" "$LATEST_URL" 2>/dev/null || true)"
    [ -n "$VERSION" ] || { echo "install.sh: could not resolve latest version from $LATEST_URL" >&2; exit 1; }
fi

TARBALL="python-wasm-$VERSION-$PLATFORM.tar.gz"
DOWNLOAD_URL="$RELEASE_BASE/download/v$VERSION/$TARBALL"

echo "==> installing python-wasm $VERSION ($PLATFORM)"
echo "    download: $DOWNLOAD_URL"
echo "    prefix:   $PREFIX"
echo "    bin-dir:  $BIN_DIR"

# --- download + extract ----------------------------------------------------

mkdir -p "$PREFIX" "$BIN_DIR"
tmp="$(mktemp -d 2>/dev/null || mktemp -d -t pwasm-install)"
trap 'rm -rf "$tmp"' EXIT

curl -fsSL -H "User-Agent: $USER_AGENT" -o "$tmp/$TARBALL" "$DOWNLOAD_URL" || {
    echo "install.sh: download failed from $DOWNLOAD_URL" >&2; exit 1
}

# Optional checksum verification when SHASUMS.txt is published alongside.
if curl -fsSL -H "User-Agent: $USER_AGENT" "$RELEASE_BASE/download/v$VERSION/checksums.txt" -o "$tmp/checksums.txt" 2>/dev/null; then
    expected="$(grep " $TARBALL\$" "$tmp/checksums.txt" | awk '{print $1}' || true)"
    if [ -n "$expected" ]; then
        actual="$(shasum -a 256 "$tmp/$TARBALL" | awk '{print $1}')"
        if [ "$expected" != "$actual" ]; then
            echo "install.sh: checksum mismatch (expected $expected, got $actual)" >&2
            exit 1
        fi
        echo "==> checksum verified"
    fi
fi

# Replace any previous install of the same prefix.
target_root="$PREFIX/python-wasm-$VERSION-$PLATFORM"
if [ -e "$target_root" ]; then
    echo "==> removing previous $target_root"
    rm -rf "$target_root"
fi
tar xzf "$tmp/$TARBALL" -C "$PREFIX"

# Symlink the active install for stable PATH semantics.
ln -sfn "$target_root" "$PREFIX/active"
ln -sfn "$PREFIX/active/bin/python-wasm" "$BIN_DIR/python-wasm"

echo "==> installed: $target_root"
echo "==> symlinked: $BIN_DIR/python-wasm -> $PREFIX/active/bin/python-wasm"

# --- PATH sniff -------------------------------------------------------------

case ":$PATH:" in
    *":$BIN_DIR:"*) on_path=1 ;;
    *)              on_path=0 ;;
esac

if [ $on_path -eq 0 ] && [ $MODIFY_PATH -eq 1 ]; then
    # Append a line to a couple of likely rc files (idempotent — skip if
    # already present).
    line="export PATH=\"\$PATH:$BIN_DIR\""
    for rc in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc"; do
        [ -f "$rc" ] || continue
        if ! grep -q "$BIN_DIR" "$rc"; then
            printf '\n# python-wasm\n%s\n' "$line" >> "$rc"
            echo "==> appended PATH update to $rc"
        fi
    done
elif [ $on_path -eq 0 ]; then
    echo ""
    echo "$BIN_DIR is not on your PATH. Add it manually:"
    echo "  echo 'export PATH=\"\$PATH:$BIN_DIR\"' >> ~/.profile"
fi

echo ""
echo "Run \`python-wasm --help\` to get started."

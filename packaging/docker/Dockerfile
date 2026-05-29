#syntax=docker/dockerfile:1.6

# python-wasm Docker image.
#
# Bundles wasmtime + a release tarball so `docker run` is the only
# install step. Multi-stage build keeps the runtime image lean.
#
# Build args:
#   PYTHON_WASM_VERSION   release version (e.g. 0.1.0)
#   WASMTIME_VERSION      wasmtime version to install
#   TARGET_ARCH           docker arch ("x86_64" or "arm64")
#
# Build (single-arch):
#   docker build \
#     --build-arg PYTHON_WASM_VERSION=0.1.0 \
#     --build-arg TARGET_ARCH=$(uname -m | sed 's/x86_64/x86_64/;s/aarch64/arm64/;s/arm64/arm64/') \
#     -f packaging/docker/Dockerfile \
#     -t python-wasm:0.1.0 .
#
# Multi-arch (buildx):
#   docker buildx build --platform linux/amd64,linux/arm64 \
#     --build-arg PYTHON_WASM_VERSION=0.1.0 \
#     -f packaging/docker/Dockerfile -t ghcr.io/tegmentum/python-wasm:0.1.0 \
#     --push .

ARG PYTHON_WASM_VERSION=0.1.0
ARG WASMTIME_VERSION=27.0.0
ARG TARGET_ARCH=x86_64

# --- stage 1: fetch wasmtime ---------------------------------------------
FROM debian:bookworm-slim AS wasmtime-stage
ARG WASMTIME_VERSION
ARG TARGET_ARCH

RUN apt-get update && \
    apt-get install -y --no-install-recommends curl xz-utils ca-certificates && \
    rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    case "$TARGET_ARCH" in \
        x86_64) wasmtime_arch="x86_64-linux" ;; \
        arm64)  wasmtime_arch="aarch64-linux" ;; \
        *) echo "unsupported TARGET_ARCH=$TARGET_ARCH" >&2; exit 1 ;; \
    esac; \
    curl -fsSL "https://github.com/bytecodealliance/wasmtime/releases/download/v${WASMTIME_VERSION}/wasmtime-v${WASMTIME_VERSION}-${wasmtime_arch}.tar.xz" \
        | tar xJ -C /tmp; \
    mv "/tmp/wasmtime-v${WASMTIME_VERSION}-${wasmtime_arch}/wasmtime" /usr/local/bin/wasmtime; \
    chmod +x /usr/local/bin/wasmtime

# --- stage 2: stage the release tarball -----------------------------------
FROM debian:bookworm-slim AS release-stage
ARG PYTHON_WASM_VERSION
ARG TARGET_ARCH

# The tarball is expected to live at dist/release/ in the build context.
# CI assembles the matching one before invoking docker build.
COPY dist/release/python-wasm-${PYTHON_WASM_VERSION}-linux-${TARGET_ARCH}.tar.gz /tmp/python-wasm.tar.gz

RUN tar xzf /tmp/python-wasm.tar.gz -C /opt && \
    mv /opt/python-wasm-${PYTHON_WASM_VERSION}-linux-${TARGET_ARCH} /opt/python-wasm && \
    rm /tmp/python-wasm.tar.gz

# --- stage 3: minimal runtime --------------------------------------------
FROM debian:bookworm-slim
ARG PYTHON_WASM_VERSION

LABEL org.opencontainers.image.title="python-wasm"
LABEL org.opencontainers.image.description="CPython 3.14 interpreter on WebAssembly (WASI Preview 2)"
LABEL org.opencontainers.image.source="https://github.com/tegmentum/python-wasm"
LABEL org.opencontainers.image.licenses="PSF-2.0"
LABEL org.opencontainers.image.version="${PYTHON_WASM_VERSION}"

RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates && \
    rm -rf /var/lib/apt/lists/*

COPY --from=wasmtime-stage /usr/local/bin/wasmtime /usr/local/bin/wasmtime
COPY --from=release-stage /opt/python-wasm /opt/python-wasm

RUN ln -s /opt/python-wasm/bin/python-wasm /usr/local/bin/python-wasm && \
    mkdir -p /root/.python-wasm/site-packages
ENV PYTHON_WASM_HOME=/root/.python-wasm

ENTRYPOINT ["python-wasm"]
CMD ["--help"]

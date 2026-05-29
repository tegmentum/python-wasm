# `python-wasm` Docker image

Self-contained image: bundles wasmtime + the python-wasm release
tarball. `docker run` is the only install step.

## Build

Single-arch (matches the host):

```bash
# From the python-wasm repo root, with dist/release/ populated:
docker build \
    --build-arg PYTHON_WASM_VERSION=0.1.0 \
    --build-arg TARGET_ARCH=$(uname -m | sed 's/x86_64/x86_64/;s/aarch64/arm64/;s/arm64/arm64/') \
    -f packaging/docker/Dockerfile \
    -t python-wasm:0.1.0 .
```

Multi-arch (buildx, requires `docker buildx create --use` first):

```bash
docker buildx build \
    --platform linux/amd64,linux/arm64 \
    --build-arg PYTHON_WASM_VERSION=0.1.0 \
    -f packaging/docker/Dockerfile \
    -t ghcr.io/tegmentum/python-wasm:0.1.0 \
    --push .
```

The build assumes
`dist/release/python-wasm-${VERSION}-linux-${TARGET_ARCH}.tar.gz`
exists in the context. CI assembles those tarballs in a separate
step.

## Run

```bash
# Hello world
docker run --rm python-wasm:0.1.0 -c "print(1+1)"

# A script (mount cwd)
docker run --rm -v $PWD:/work -w /work python-wasm:0.1.0 ./script.py

# Persistent user state (rebuild output, installed wheels)
docker run --rm -v $HOME/.python-wasm:/root/.python-wasm python-wasm:0.1.0 -c "..."
```

## Publish

GHCR is the recommended registry:

```bash
docker buildx build \
    --platform linux/amd64,linux/arm64 \
    --build-arg PYTHON_WASM_VERSION=0.1.0 \
    -f packaging/docker/Dockerfile \
    -t ghcr.io/tegmentum/python-wasm:0.1.0 \
    -t ghcr.io/tegmentum/python-wasm:latest \
    --push .
```

One-time setup: create a GHCR token for the Tegmentum org and
`docker login ghcr.io -u <user> --password-stdin <<<"$TOKEN"`. CI
should use a `GITHUB_TOKEN`-derived token instead.

## Image size

The runtime image is ~70 MiB:

- Debian bookworm-slim base: ~30 MiB
- wasmtime: ~30 MiB
- python-wasm tarball contents: ~22 MiB
- ca-certificates: ~2 MiB

Considered alternatives:

- `gcr.io/distroless/static`: would shave ~25 MiB but loses
  `apt-get install ca-certificates` simplicity. The certs are
  needed for TLS to PyPI / external APIs from inside python-wasm.
- `alpine`: musl mismatches with wasmtime's glibc binaries. Skip.

## See also

- [`distribution-channels-plan.md`](../../docs/distribution-channels-plan.md)
  §D₁e — this image's role across channels.
- [`scripts/package-release.sh`](../../scripts/package-release.sh) — what produces
  the tarball the Dockerfile consumes.

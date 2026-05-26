PROJECT_DIR := $(shell pwd)
DEPS_DIR := $(PROJECT_DIR)/deps
CPYTHON_DIR := $(DEPS_DIR)/cpython
WASI_SDK_DIR := $(DEPS_DIR)/wasi-sdk-33.0-arm64-macos
OPENSSL_PREFIX := $(DEPS_DIR)/openssl-prefix
HOST_TRIPLE := wasm32-wasip2
PYTHON_WASM := $(CPYTHON_DIR)/cross-build/$(HOST_TRIPLE)/python.wasm

.PHONY: all fetch-deps build run test clean distclean \
       web-deps web-stdlib web-transpile web-dev web-build web-clean \
       python-component-verify python-composed install-python-shims \
       test-compression-extension test-hash-extensions \
       test-ssl-capability test-ssl-network composectl-plan

all: fetch-deps build

fetch-deps:
	bash scripts/fetch-sdk.sh
	bash scripts/fetch-cpython.sh

# Componentize-python plan, Phase 3d: by default the build NO LONGER static-
# links OpenSSL. The capability path (_ssl_capability + openssl-component +
# ssl_capability.py) covers `import ssl` and `urllib.request.urlopen` via
# composition (see docs/phase-3-tls.md). Without --with-openssl, CPython
# auto-disables the static _ssl and _hashlib modules — both are superseded
# by capabilities (_crypto_hash for hashlib; _ssl_capability for ssl).
#
# To opt back into the static path during the soak period: STATIC_OPENSSL=1 make build
STATIC_OPENSSL ?=
ifeq ($(STATIC_OPENSSL),1)
    WITH_OPENSSL_FLAG := -- --with-openssl=$(OPENSSL_PREFIX)
    OPENSSL_STEP     := bash scripts/build-openssl.sh
else
    WITH_OPENSSL_FLAG :=
    OPENSSL_STEP     := @echo "build: static OpenSSL disabled (capability path is the default; set STATIC_OPENSSL=1 to re-enable)"
endif

build: fetch-deps
	bash scripts/build-zlib.sh
	$(OPENSSL_STEP)
	cd $(CPYTHON_DIR) && python3 Tools/wasm/wasi build \
		--host-triple $(HOST_TRIPLE) \
		--wasi-sdk $(WASI_SDK_DIR) \
		$(WITH_OPENSSL_FLAG)

run:
	@bash scripts/run-python.sh $(ARGS)

test:
	bash tests/test_wasi_cli.sh

clean:
	cd $(CPYTHON_DIR) && python3 Tools/wasm/wasi clean 2>/dev/null || true

distclean:
	rm -rf $(DEPS_DIR)

# --- Web demo targets ---

web-deps:
	cd $(PROJECT_DIR)/web && npm install

web-stdlib: build
	bash scripts/bundle-stdlib.sh

# Phase 4: web-transpile now consumes the COMPOSED python component (capabilities
# wired in), not the raw python.wasm. The composed wasm has zero non-wasi:*
# imports, so jco/wasi-polyfill can instantiate it as-is in the browser.
web-transpile: python-composed web-deps
	bash scripts/transpile-component.sh

web-dev: web-deps web-stdlib web-transpile
	cd $(PROJECT_DIR)/web && npx vite

web-build: web-deps web-stdlib web-transpile
	cd $(PROJECT_DIR)/web && npx vite build

web-clean:
	rm -rf $(PROJECT_DIR)/web/public/python-component
	rm -f $(PROJECT_DIR)/web/public/stdlib.tar.gz
	rm -rf $(PROJECT_DIR)/web/dist
	rm -rf $(PROJECT_DIR)/web/node_modules

# Componentize-python plan, Phase 0: verify python.wasm is a valid wasi-p2
# component (exports wasi:cli/run, imports only wasi:*). The wasi-sdk build
# already produces a component natively, so this is a regression guard rather
# than a transform — if a future toolchain change drops back to a core module
# this gate fails and we'll know.
python-component-verify: build
	@bash scripts/verify-python-component.sh

# Componentize-python plan, Phase 1: compose python.wasm with the
# compression-multiplexer capability component. Produces build/python.composed.wasm.
# Also installs Python-side shims (ssl_capability, etc.) so the composed wasm
# can pick them up from /Lib at run time.
python-composed: build install-python-shims
	@bash scripts/compose-python-component.sh

# Componentize-python plan, Phase 3b.6: install Python-side shim modules into
# deps/cpython/Lib/ so the composed wasm can `import ssl_capability` at runtime.
# Idempotent — re-copies on every invocation so iteration on the shim is
# pick-up-on-next-run.
install-python-shims:
	@cp $(PROJECT_DIR)/cpython-ext/_ssl/ssl_capability.py \
	    $(PROJECT_DIR)/deps/cpython/Lib/ssl_capability.py
	@echo "installed: deps/cpython/Lib/ssl_capability.py"

# Componentize-python plan, Phase 1: end-to-end smoke test of the composed
# component + _compression extension + multiplexer.
test-compression-extension: python-composed
	@bash scripts/test-compression-extension.sh

# Componentize-python plan, Phase 2: end-to-end smoke test of the composed
# component + _crypto_hash + _xxhash extensions against canonical vectors for
# all 9 crypto + 5 verifiable non-crypto algorithms.
test-hash-extensions: python-composed
	@bash scripts/test-hash-extensions.sh

# Componentize-python plan, Phase 3b: end-to-end smoke of _ssl_capability.
# Exercises 3b.1 (scaffold), 3b.2 (MemoryBIO with stdlib parity), and 3b.3
# (_SSLContext + _SSLSocket type wiring + config knobs, no network).
test-ssl-capability: python-composed
	@bash scripts/test-ssl-capability.sh

# Componentize-python plan, Phase 3c.1: NETWORK-GATED end-to-end TLS smoke.
# Does a real HTTPS request through _SSLSocket -> openssl-component ->
# wasi:sockets/tcp. Default-OFF; opt-in with NETWORK=1. This is the gating
# decision-point #2 from docs/phase-3-tls.md ("real handshake works?").
test-ssl-network: python-composed
	@NETWORK=1 bash scripts/test-ssl-network.sh

# Componentize-python plan, Phase 4: generate the composectl plan that pins
# python.wasm + capability multiplexers by CAS digest. Reproducibility target;
# wac (python-composed) is the dev fast-path until composectl's emit dep-wiring
# is fixed upstream.
composectl-plan: build
	@bash scripts/build-composectl-plan.sh

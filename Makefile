PROJECT_DIR := $(shell pwd)
DEPS_DIR := $(PROJECT_DIR)/deps
CPYTHON_DIR := $(DEPS_DIR)/cpython
WASI_SDK_DIR := $(DEPS_DIR)/wasi-sdk-33.0-arm64-macos
OPENSSL_PREFIX := $(DEPS_DIR)/openssl-prefix
HOST_TRIPLE := wasm32-wasip2
PYTHON_WASM := $(CPYTHON_DIR)/cross-build/$(HOST_TRIPLE)/python.wasm

.PHONY: all fetch-deps build run test clean distclean \
       web-deps web-stdlib web-transpile web-dev web-build web-clean \
       python-component-verify

all: fetch-deps build

fetch-deps:
	bash scripts/fetch-sdk.sh
	bash scripts/fetch-cpython.sh

build: fetch-deps
	bash scripts/build-zlib.sh
	bash scripts/build-openssl.sh
	cd $(CPYTHON_DIR) && python3 Tools/wasm/wasi build \
		--host-triple $(HOST_TRIPLE) \
		--wasi-sdk $(WASI_SDK_DIR) \
		-- --with-openssl=$(OPENSSL_PREFIX)

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

web-transpile: build web-deps
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

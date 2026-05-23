PROJECT_DIR := $(shell pwd)
DEPS_DIR := $(PROJECT_DIR)/deps
CPYTHON_DIR := $(DEPS_DIR)/cpython
WASI_SDK_DIR := $(DEPS_DIR)/wasi-sdk-33.0-arm64-macos
HOST_TRIPLE := wasm32-wasip2
PYTHON_WASM := $(CPYTHON_DIR)/cross-build/$(HOST_TRIPLE)/python.wasm

.PHONY: all fetch-deps build run test clean distclean \
       web-deps web-stdlib web-transpile web-dev web-build web-clean

all: fetch-deps build

fetch-deps:
	bash scripts/fetch-sdk.sh
	bash scripts/fetch-cpython.sh

build: fetch-deps
	cd $(CPYTHON_DIR) && python3 Tools/wasm/wasi build \
		--host-triple $(HOST_TRIPLE) \
		--wasi-sdk $(WASI_SDK_DIR)

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

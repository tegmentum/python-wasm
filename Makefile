PROJECT_DIR := $(shell pwd)
DEPS_DIR := $(PROJECT_DIR)/deps

# Build profile — names a TOML file under profiles/. The profile captures
# every option that the build depends on: which CPython source tree, which
# wasi-sdk, which static-vs-cap toggles, which cap artifacts get plugged
# in at compose time, and which build/<dir> the outputs land in. Multiple
# profiles coexist on disk under build/<profile>/ so simultaneous version
# builds don't clobber each other. See docs/build-profiles.md.
PROFILE ?= default

# Resolve profile -> KEY=VALUE pairs once at Make-load time and turn each
# pair into a Make variable. The shell command below emits one
# "KEY=VALUE" line per resolved field, which we splice into a single
# $(eval ...) so the variables become first-class Make symbols.
PROFILE_VARS := $(shell bash $(PROJECT_DIR)/scripts/load-profile.sh $(PROFILE))
$(foreach line,$(PROFILE_VARS),$(eval $(line)))

CPYTHON_DIR := $(DEPS_DIR)/$(PYTHON_SOURCE_DIR)
WASI_SDK_DIR := $(DEPS_DIR)/$(WASI_SDK_DIR)
OPENSSL_PREFIX := $(DEPS_DIR)/openssl-prefix
PYTHON_WASM := $(CPYTHON_DIR)/cross-build/$(HOST_TRIPLE)/python.wasm
PROFILE_BUILD_DIR := $(PROJECT_DIR)/$(BUILD_DIR)
COMPOSED_WASM := $(PROFILE_BUILD_DIR)/python.composed.wasm

# Export profile-derived env vars so child scripts inherit them without
# re-parsing the TOML.
export PROFILE
export PYTHON_SOURCE_DIR PYTHON_VERSION HOST_TRIPLE
export STATIC_OPENSSL STATIC_ZLIB WITH_V86_POSIX
export ZLIB_COMPONENT_WASM BZIP2_COMPONENT_WASM LZMA_COMPONENT_WASM ZSTD_COMPONENT_WASM
export LZ4_COMPONENT_WASM OPENZL_COMPONENT_WASM
export CRYPTO_HASH_MULTIPLEXER_WASM
export HASHING_MULTIPLEXER_WASM OPENSSL_COMPONENT_WASM
export SQLITE_COMPONENT_WASM PASSWORD_HASH_MULTIPLEXER_WASM
export V86_POSIX_COMPONENT_WASM
export PROFILE_BUILD_DIR COMPOSED_WASM

.PHONY: all fetch-deps build run test clean distclean \
       web-deps web-stdlib web-transpile web-dev web-build web-clean \
       python-component-verify python-composed install-python-shims \
       test-compression-extension test-hash-extensions \
       test-ssl-capability test-ssl-network composectl-plan \
       show-profile

all: fetch-deps build

# Print resolved profile variables — useful when debugging "what's this
# PROFILE actually using?"
show-profile:
	@echo "PROFILE=$(PROFILE)"
	@echo "  PYTHON_VERSION=$(PYTHON_VERSION)"
	@echo "  PYTHON_SOURCE_DIR=$(PYTHON_SOURCE_DIR)"
	@echo "  CPYTHON_DIR=$(CPYTHON_DIR)"
	@echo "  WASI_SDK_DIR=$(WASI_SDK_DIR)"
	@echo "  HOST_TRIPLE=$(HOST_TRIPLE)"
	@echo "  STATIC_OPENSSL=$(STATIC_OPENSSL)  STATIC_ZLIB=$(STATIC_ZLIB)  WITH_V86_POSIX=$(WITH_V86_POSIX)"
	@echo "  BUILD_DIR=$(BUILD_DIR)"
	@echo "  COMPOSED_WASM=$(COMPOSED_WASM)"

fetch-deps:
	bash scripts/fetch-sdk.sh
	bash scripts/fetch-cpython.sh $(PROFILE)
	bash scripts/fetch-tzdata.sh

# Static OpenSSL / zlib toggles come from the profile now. When the
# profile sets static_openssl=true (or static_zlib=true), the build also
# runs the corresponding pre-build script to populate openssl-prefix or
# zlib-build. Default profile keeps both off (capability path).
ifeq ($(STATIC_OPENSSL),1)
    WITH_OPENSSL_FLAG := -- --with-openssl=$(OPENSSL_PREFIX)
    OPENSSL_STEP     := bash scripts/build-openssl.sh
else
    WITH_OPENSSL_FLAG :=
    OPENSSL_STEP     := @echo "build: static OpenSSL disabled (profile $(PROFILE); cap path is the default)"
endif

ifeq ($(STATIC_ZLIB),1)
    ZLIB_STEP        := bash scripts/build-zlib.sh
else
    ZLIB_STEP        := @echo "build: static zlib disabled (profile $(PROFILE); cap path is the default)"
endif

build: fetch-deps
	$(ZLIB_STEP)
	$(OPENSSL_STEP)
	# Wire every cpython-ext/ capability extension into the cpython build
	# tree (symlinks + Setup.local). Idempotent. The deps/cpython tree is
	# gitignored so these have to be (re-)applied locally on each clone.
	bash scripts/wire-cpython-ext.sh
	# CPython's WASI-build entrypoint moved between minor versions:
	#   3.13:  Tools/wasm/wasi.py        (single-file module)
	#   3.14+: Tools/wasm/wasi/__main__.py (package)
	# Detect which shape this profile's tree has and invoke accordingly.
	cd $(CPYTHON_DIR) && \
	    if [ -f Tools/wasm/wasi.py ]; then \
	        python3 Tools/wasm/wasi.py build \
	            --host-triple $(HOST_TRIPLE) \
	            --wasi-sdk $(WASI_SDK_DIR) \
	            $(WITH_OPENSSL_FLAG); \
	    else \
	        python3 Tools/wasm/wasi build \
	            --host-triple $(HOST_TRIPLE) \
	            --wasi-sdk $(WASI_SDK_DIR) \
	            $(WITH_OPENSSL_FLAG); \
	    fi

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
#
# The bz2/lzma/zstd/zlib shims SHADOW the stdlib copies of the same name.
# They route through `_compress_cap` (the capability extension) instead of
# the missing-on-wasi `_bz2`/`_lzma`/`_zstd`/`_zlib` C extensions. Tier A
# net effect: `import bz2` / `import lzma` / `from compression import zstd`
# now WORK in python-wasm (previously `ImportError`); `zlib` continues to
# work but via the capability path (no `-lz` link dep).
install-python-shims:
	@cp $(PROJECT_DIR)/cpython-ext/_ssl/ssl_capability.py \
	    $(CPYTHON_DIR)/Lib/ssl_capability.py
	@echo "installed: $(PYTHON_SOURCE_DIR)/Lib/ssl_capability.py"
	@cp $(PROJECT_DIR)/cpython-ext/_ssl/ssl.py \
	    $(CPYTHON_DIR)/Lib/ssl.py
	@echo "installed: $(PYTHON_SOURCE_DIR)/Lib/ssl.py  (Phase 5.2: cap-route ssl through _ssl_capability)"
	@cp $(PROJECT_DIR)/cpython-ext/_compression/bz2.py \
	    $(CPYTHON_DIR)/Lib/bz2.py
	@echo "installed: $(PYTHON_SOURCE_DIR)/Lib/bz2.py  (Tier A: routes to _compress_cap.bzip2_*)"
	@cp $(PROJECT_DIR)/cpython-ext/_compression/lzma.py \
	    $(CPYTHON_DIR)/Lib/lzma.py
	@echo "installed: $(PYTHON_SOURCE_DIR)/Lib/lzma.py  (Tier A: routes to _compress_cap.lzma_* / FORMAT_XZ)"
	@if [ -d "$(CPYTHON_DIR)/Lib/compression/zstd" ]; then \
	    cp $(PROJECT_DIR)/cpython-ext/_compression/zstd.py \
	        $(CPYTHON_DIR)/Lib/compression/zstd/__init__.py && \
	    echo "installed: $(PYTHON_SOURCE_DIR)/Lib/compression/zstd/__init__.py  (Tier A: routes to _compress_cap.zstd_*)"; \
	else \
	    echo "skipped: $(PYTHON_SOURCE_DIR)/Lib/compression/zstd/  (not a stdlib module in this Python version — added in 3.14)"; \
	fi
	@cp $(PROJECT_DIR)/cpython-ext/_compression/zlib.py \
	    $(CPYTHON_DIR)/Lib/zlib.py
	@echo "installed: $(PYTHON_SOURCE_DIR)/Lib/zlib.py  (Tier A: routes to _compress_cap.deflate_* + C-speed crc32/adler32)"
	@cp $(PROJECT_DIR)/cpython-ext/_crypto_hash/_hashlib.py \
	    $(CPYTHON_DIR)/Lib/_hashlib.py
	@echo "installed: $(PYTHON_SOURCE_DIR)/Lib/_hashlib.py  (Phase 5.1 redesign: pure-Python pbkdf2_hmac)"
	@cp $(PROJECT_DIR)/cpython-ext/_sqlite_capability/sqlite3.py \
	    $(CPYTHON_DIR)/Lib/sqlite3/__init__.py
	@echo "installed: $(PYTHON_SOURCE_DIR)/Lib/sqlite3/__init__.py  (Tier B: routes to _sqlite_cap)"
	@cp $(PROJECT_DIR)/cpython-ext/_mmap_shim/mmap.py \
	    $(CPYTHON_DIR)/Lib/mmap.py
	@echo "installed: $(PYTHON_SOURCE_DIR)/Lib/mmap.py  (Blocked-3: pure-Python mmap on bytearray + file I/O)"
	@cp $(PROJECT_DIR)/cpython-ext/_threading_shim/threading.py \
	    $(CPYTHON_DIR)/Lib/threading.py
	@echo "installed: $(PYTHON_SOURCE_DIR)/Lib/threading.py  (LOW-2: single-threaded shim)"
	@cp $(PROJECT_DIR)/cpython-ext/_ctypes_shim/__init__.py \
	    $(CPYTHON_DIR)/Lib/ctypes/__init__.py
	@cp $(PROJECT_DIR)/cpython-ext/_ctypes_shim/util.py \
	    $(CPYTHON_DIR)/Lib/ctypes/util.py
	@echo "installed: $(PYTHON_SOURCE_DIR)/Lib/ctypes/  (LOW-1: stub for native ABI absence)"
	@cp $(PROJECT_DIR)/cpython-ext/_posix_user_shim/sitecustomize.py \
	    $(CPYTHON_DIR)/Lib/sitecustomize.py
	@echo "installed: $(PYTHON_SOURCE_DIR)/Lib/sitecustomize.py  (Phase 1: WASI user-id stubs for pip/platformdirs)"
	@if [ "$(WITH_V86_POSIX)" = "1" ]; then \
	    cp $(PROJECT_DIR)/cpython-ext/_v86_posix/subprocess.py \
	        $(CPYTHON_DIR)/Lib/subprocess.py && \
	    echo "installed: $(PYTHON_SOURCE_DIR)/Lib/subprocess.py  (Tier C: routes Popen/run through _v86_posix.spawn)"; \
	else \
	    rm -f $(CPYTHON_DIR)/Lib/subprocess.py && \
	    echo "skipped: subprocess.py shim (WITH_V86_POSIX=0; using stdlib subprocess from Lib/)"; \
	fi

# Test targets run against an already-built artifact at $(COMPOSED_WASM).
# Run `make python-composed PROFILE=<name>` once (or just `make python-composed`
# for the default profile) to produce it. The test targets don't re-trigger
# build because CPython's wasi.py isn't idempotent across rebuilds on all
# minor versions (notably 3.13).
#
# Componentize-python plan, Phase 1: end-to-end smoke test of the composed
# component + _compression extension + multiplexer.
test-compression-extension:
	@bash scripts/test-compression-extension.sh

# Componentize-python plan, Phase 2: end-to-end smoke test of the composed
# component + _crypto_hash + _xxhash extensions against canonical vectors for
# all 9 crypto + 5 verifiable non-crypto algorithms.
test-hash-extensions:
	@bash scripts/test-hash-extensions.sh

# Componentize-python plan, Phase 3b: end-to-end smoke of _ssl_capability.
# Exercises 3b.1 (scaffold), 3b.2 (MemoryBIO with stdlib parity), and 3b.3
# (_SSLContext + _SSLSocket type wiring + config knobs, no network).
test-ssl-capability:
	@bash scripts/test-ssl-capability.sh

# Componentize-python plan, Phase 3c.1: NETWORK-GATED end-to-end TLS smoke.
# Does a real HTTPS request through _SSLSocket -> openssl-component ->
# wasi:sockets/tcp. Default-OFF; opt-in with NETWORK=1. This is the gating
# decision-point #2 from docs/phase-3-tls.md ("real handshake works?").
test-ssl-network:
	@NETWORK=1 bash scripts/test-ssl-network.sh

# Componentize-python plan, Tier 1 v86: end-to-end smoke of _v86_posix +
# v86-posix-stub through the composed python.wasm. Asserts the module +
# exception hierarchy + spawn → GuestNotReadyError contract (the stub's
# only behavior — see crates/v86-posix-stub in the v86 repo).
.PHONY: test-v86-posix-extension
test-v86-posix-extension:
	@bash scripts/test-v86-posix-extension.sh

# Componentize-python plan, Phase 4: generate the composectl plan that pins
# python.wasm + capability multiplexers by CAS digest. Reproducibility target;
# wac (python-composed) is the dev fast-path until composectl's emit dep-wiring
# is fixed upstream.
composectl-plan: build
	@bash scripts/build-composectl-plan.sh

# Tier-1 v86 composition (plans/python-v86.json + the real-host round-trip
# test) lives in the v86 repo — it pins v86 component digests and exercises
# v86's posix-helper.sh, so it's owned there. See
#   ~/git/v86/scripts/build-python-v86-composectl-plan.sh
#   ~/git/v86/scripts/test-v86-posix-roundtrip.sh
# Both read PYTHON_WASM_REPO (default: ~/git/python-wasm) and re-run this
# repo's compose-python-component.sh with V86_POSIX_COMPONENT_WASM pinned
# to v86's artifact, so they work transparently across the two checkouts.

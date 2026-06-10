# shortcog top-level dev convenience.

SRC_DIR        := core
BUILD_DIR      := core/build
TSAN_BUILD_DIR := core/build-tsan
FIXTURE_DIR    := $(SRC_DIR)/tests/fixtures/data
FIXTURE_SCRIPT := $(SRC_DIR)/tests/fixtures/make_fixtures.py
PYTHON         ?= python
PREFIX         ?= /usr/local

PY_DIR     ?= bindings/python
PY_LIB_DIR := $(PY_DIR)/shortcog/_lib

# Homebrew prefix on macOS so find_package picks up GDAL / zstd.
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW_PREFIX),)
PREFIX_FLAG := -DCMAKE_PREFIX_PATH=$(BREW_PREFIX)
endif

CMAKE_BUILD_TYPE ?= Release
CMAKE_FLAGS      ?=

CMAKE_OPTS := \
	-G Ninja \
	-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
	-DSHORTCOG_BUILD_TESTS=ON \
	$(PREFIX_FLAG) \
	$(CMAKE_FLAGS)

.PHONY: all build configure fixtures test test-tsan python install clean clean-fixtures help

# `make` does the lot: clean, build, run the tests, then run them under TSan.
all:
	$(MAKE) clean
	$(MAKE) test
	$(MAKE) test-tsan

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S $(SRC_DIR) -B $(BUILD_DIR) $(CMAKE_OPTS)

configure:
	cmake -S $(SRC_DIR) -B $(BUILD_DIR) $(CMAKE_OPTS)

build: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR)

$(FIXTURE_DIR)/manifest.json: $(FIXTURE_SCRIPT)
	$(PYTHON) $(FIXTURE_SCRIPT) --out $(FIXTURE_DIR)

fixtures: $(FIXTURE_DIR)/manifest.json

# Generate the corpus, run ctest, then always remove data/ — even if a test
# fails. The trap fires on exit and set -e propagates the failing status.
test: build
	@set -e; \
	trap 'rm -rf $(FIXTURE_DIR)' EXIT; \
	$(PYTHON) $(FIXTURE_SCRIPT) --out $(FIXTURE_DIR); \
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# Same run on a throwaway build compiled with ThreadSanitizer. halt_on_error
# makes a detected race return non-zero so ctest fails on it.
test-tsan:
	cmake -S $(SRC_DIR) -B $(TSAN_BUILD_DIR) -G Ninja \
	  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	  -DSHORTCOG_BUILD_TESTS=ON \
	  -DSHORTCOG_SANITIZE=thread \
	  $(PREFIX_FLAG)
	cmake --build $(TSAN_BUILD_DIR)
	@set -e; \
	trap 'rm -rf $(FIXTURE_DIR)' EXIT; \
	$(PYTHON) $(FIXTURE_SCRIPT) --out $(FIXTURE_DIR); \
	TSAN_OPTIONS=halt_on_error=1 ctest --test-dir $(TSAN_BUILD_DIR) --output-on-failure

# Build the shared lib, drop it next to the binding, check it loads, then run
# pytest if the binding ships tests.
python: build
	@mkdir -p $(PY_LIB_DIR)
	@rm -f $(PY_LIB_DIR)/libshortcog* $(PY_LIB_DIR)/shortcog*.dll
	@lib=$$(find $(BUILD_DIR) \( -name 'libshortcog*.dylib' -o -name 'libshortcog*.so*' -o -name 'shortcog*.dll' \) | head -1); \
	  [ -n "$$lib" ] || { echo "no built libshortcog under $(BUILD_DIR)"; exit 1; }; \
	  cp -a "$$(dirname "$$lib")"/libshortcog* $(PY_LIB_DIR)/ 2>/dev/null || true; \
	  cp -a "$$(dirname "$$lib")"/shortcog*.dll $(PY_LIB_DIR)/ 2>/dev/null || true
	@$(PYTHON) -c 'import numpy, cffi' 2>/dev/null || { echo "missing deps: pip install numpy cffi"; exit 1; }
	cd $(PY_DIR) && $(PYTHON) -c "import shortcog; print('loaded shortcog', shortcog.__version__)"
	@if [ -d $(PY_DIR)/tests ] && $(PYTHON) -c 'import pytest' 2>/dev/null; then \
	  cd $(PY_DIR) && $(PYTHON) -m pytest -q; rc=$$?; \
	  [ $$rc -eq 0 ] || [ $$rc -eq 5 ] || exit $$rc; \
	else \
	  echo "no python tests (or pytest missing); skipping"; \
	fi

install: build
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)

clean:
	rm -rf $(BUILD_DIR) $(TSAN_BUILD_DIR)
	rm -f $(PY_LIB_DIR)/libshortcog* $(PY_LIB_DIR)/shortcog*.dll

clean-fixtures:
	rm -rf $(FIXTURE_DIR)

help:
	@echo "Targets:"
	@echo "  (default)       clean + build + test + test-tsan"
	@echo "  build           incremental build"
	@echo "  configure       force CMake reconfigure"
	@echo "  fixtures        generate test fixture corpus (kept)"
	@echo "  test            generate fixtures, run ctest, remove them"
	@echo "  test-tsan       same, on a ThreadSanitizer build"
	@echo "  python          build lib, copy into the binding, smoke-load, pytest"
	@echo "  install         cmake --install into PREFIX"
	@echo "  clean           remove $(BUILD_DIR) and $(TSAN_BUILD_DIR)"
	@echo "  clean-fixtures  remove $(FIXTURE_DIR)"
	@echo ""
	@echo "Overrides:"
	@echo "  CMAKE_BUILD_TYPE=Debug    PYTHON=python3.12    PREFIX=/opt/local"
	@echo "  CMAKE_FLAGS=\"-DFOO=ON\"    PY_DIR=bindings/python"
# shortcog top-level dev convenience.

VERSION := $(shell tr -d '[:space:]' < VERSION)

SRC_DIR        := core
BUILD_DIR      := core/build
TSAN_BUILD_DIR := core/build-tsan
FIXTURE_DIR    := $(SRC_DIR)/tests/fixtures/data
FIXTURE_SCRIPT := $(SRC_DIR)/tests/fixtures/make_fixtures.py
PYTHON         ?= python
PREFIX         ?= /usr/local

# Bindings. Python exists; R is planned (terra, system GDAL, no vendoring).
PY_DIR     ?= bindings/python
R_DIR      ?= bindings/r
PY_LIB_DIR := $(PY_DIR)/shortcog/_lib

ifeq ($(shell uname -s),Darwin)
LIB_NAME := libshortcog.dylib
else ifeq ($(OS),Windows_NT)
LIB_NAME := shortcog.dll
else
LIB_NAME := libshortcog.so
endif

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

.PHONY: all build configure lib stage-lib sync fixtures test test-tsan \
        python r install clean clean-fixtures help

# The full flow: wipe, build + test, the same under TSan, then run every
# binding that exists. sync runs as a dependency of the binding targets.
all:
	$(MAKE) clean
	$(MAKE) sync
	$(MAKE) test
	$(MAKE) test-tsan
	$(MAKE) python
	$(MAKE) r
	@echo "shortcog $(VERSION): all green"

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S $(SRC_DIR) -B $(BUILD_DIR) $(CMAKE_OPTS)

configure:
	cmake -S $(SRC_DIR) -B $(BUILD_DIR) $(CMAKE_OPTS)

build: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR)

# Just the shared lib, staged next to the Python binding. Single CI entry.
lib: build
	@mkdir -p $(PY_LIB_DIR)
	@rm -f $(PY_LIB_DIR)/libshortcog* $(PY_LIB_DIR)/shortcog*.dll
	@lib=$$(find $(BUILD_DIR) \( -name 'libshortcog*.dylib' -o -name 'libshortcog*.so*' -o -name 'shortcog*.dll' \) | head -1); \
	  [ -n "$$lib" ] || { echo "no built libshortcog under $(BUILD_DIR)"; exit 1; }; \
	  cp -a "$$(dirname "$$lib")"/libshortcog* $(PY_LIB_DIR)/ 2>/dev/null || true; \
	  cp -a "$$(dirname "$$lib")"/shortcog*.dll $(PY_LIB_DIR)/ 2>/dev/null || true; \
	  echo "staged lib into $(PY_LIB_DIR)"

# Copy the lib to STAGE_DIR for upload-artifact, no glob logic in the YAML.
STAGE_DIR ?= staged
stage-lib: lib
	@mkdir -p $(STAGE_DIR)
	@cp -a $(PY_LIB_DIR)/libshortcog* $(STAGE_DIR)/ 2>/dev/null || true
	@cp -a $(PY_LIB_DIR)/shortcog*.dll $(STAGE_DIR)/ 2>/dev/null || true
	@ls -1 $(STAGE_DIR)

# Python and CMake read VERSION at build time, so only a hardcoded manifest
# needs rewriting. That is R's DESCRIPTION, written here when bindings/r/
# exists. Today this is a no-op guard that fails on a malformed VERSION.
sync:
	@printf '%s' "$(VERSION)" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$$' \
	  || { echo "VERSION '$(VERSION)' is not X.Y.Z[-prerelease]"; exit 1; }
	@if [ -f $(R_DIR)/DESCRIPTION ]; then \
	  sed -i.bak -E 's/^Version:.*/Version: $(VERSION)/' $(R_DIR)/DESCRIPTION; \
	  rm -f $(R_DIR)/DESCRIPTION.bak; \
	  v=$$(grep -E '^Version:' $(R_DIR)/DESCRIPTION | sed 's/Version:[[:space:]]*//'); \
	  [ "$$v" = "$(VERSION)" ] || { echo "sync check: DESCRIPTION=$$v != $(VERSION)"; exit 1; }; \
	  echo "sync OK $(VERSION) (python dynamic, R DESCRIPTION written)"; \
	else \
	  echo "sync OK $(VERSION) (python dynamic, no R binding yet)"; \
	fi

# Generate the corpus and keep it. CI generates once and shares it.
fixtures:
	$(PYTHON) $(FIXTURE_SCRIPT) --out $(FIXTURE_DIR)

# Run ctest. Generates fixtures if absent; removes them on exit unless
# KEEP_FIXTURES is set (CI reuses the shared corpus).
test: build
	@set -e; \
	if [ -z "$(KEEP_FIXTURES)" ]; then trap 'rm -rf $(FIXTURE_DIR)' EXIT; fi; \
	[ -f $(FIXTURE_DIR)/manifest.json ] || $(PYTHON) $(FIXTURE_SCRIPT) --out $(FIXTURE_DIR); \
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# Same on a ThreadSanitizer build.
test-tsan:
	cmake -S $(SRC_DIR) -B $(TSAN_BUILD_DIR) -G Ninja \
	  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	  -DSHORTCOG_BUILD_TESTS=ON \
	  -DSHORTCOG_SANITIZE=thread \
	  $(PREFIX_FLAG)
	cmake --build $(TSAN_BUILD_DIR)
	@set -e; \
	if [ -z "$(KEEP_FIXTURES)" ]; then trap 'rm -rf $(FIXTURE_DIR)' EXIT; fi; \
	[ -f $(FIXTURE_DIR)/manifest.json ] || $(PYTHON) $(FIXTURE_SCRIPT) --out $(FIXTURE_DIR); \
	TSAN_OPTIONS=halt_on_error=1 ctest --test-dir $(TSAN_BUILD_DIR) --output-on-failure

# Build the lib, smoke-load the binding, run pytest if it ships tests.
python: lib
	@$(PYTHON) -c 'import numpy, cffi' 2>/dev/null || { echo "missing deps: pip install numpy cffi"; exit 1; }
	cd $(PY_DIR) && $(PYTHON) -c "import shortcog; print('loaded shortcog', shortcog.__version__)"
	@if [ -d $(PY_DIR)/tests ] && $(PYTHON) -c 'import pytest' 2>/dev/null; then \
	  cd $(PY_DIR) && $(PYTHON) -m pytest -q; rc=$$?; \
	  [ $$rc -eq 0 ] || [ $$rc -eq 5 ] || exit $$rc; \
	else \
	  echo "no python tests (or pytest missing); skipping"; \
	fi

# R binding (terra, system GDAL). Skips cleanly until bindings/r/ exists.
r:
	@if [ ! -d $(R_DIR) ]; then \
	  echo "no R binding yet ($(R_DIR) absent); skipping"; \
	else \
	  command -v R >/dev/null || { echo "missing R"; exit 1; }; \
	  ( cd $(R_DIR) && Rscript -e 'if (requireNamespace("roxygen2", quietly=TRUE)) roxygen2::roxygenise()' ); \
	  R CMD INSTALL $(R_DIR); \
	  R CMD build $(R_DIR); \
	fi

install: build
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)

clean:
	rm -rf $(BUILD_DIR) $(TSAN_BUILD_DIR) $(STAGE_DIR)
	rm -f $(PY_LIB_DIR)/libshortcog* $(PY_LIB_DIR)/shortcog*.dll

clean-fixtures:
	rm -rf $(FIXTURE_DIR)

help:
	@echo "Targets:"
	@echo "  all             clean + sync + test + test-tsan + python + r"
	@echo "  build           incremental build"
	@echo "  configure       force CMake reconfigure"
	@echo "  lib             build the shared lib, stage it into the binding (CI entry)"
	@echo "  stage-lib       copy the lib into STAGE_DIR for upload-artifact"
	@echo "  sync            validate VERSION; write R DESCRIPTION if present"
	@echo "  fixtures        generate the corpus and keep it"
	@echo "  test            run ctest (KEEP_FIXTURES=1 keeps the corpus)"
	@echo "  test-tsan       same, on a ThreadSanitizer build"
	@echo "  python          build lib, smoke-load the binding, pytest"
	@echo "  r               build the R binding (skips until $(R_DIR) exists)"
	@echo "  install         cmake --install into PREFIX"
	@echo "  clean           remove build dirs and staged libs"
	@echo "  clean-fixtures  remove $(FIXTURE_DIR)"
	@echo ""
	@echo "Overrides:"
	@echo "  CMAKE_BUILD_TYPE=Debug   PYTHON=python3.12   PREFIX=/opt/local"
	@echo "  KEEP_FIXTURES=1   STAGE_DIR=out   PY_DIR=bindings/python   R_DIR=bindings/r"
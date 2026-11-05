BUILD_DIR ?= build/release
PREFIX ?= $(HOME)/.local
CMAKE ?= cmake
CMAKE_FLAGS ?=
JOBS ?= $(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN)

.PHONY: all release install clean test deps

all: release

release:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON $(CMAKE_FLAGS)
	$(CMAKE) --build $(BUILD_DIR) --parallel $(JOBS)

install: release
	$(CMAKE) --install $(BUILD_DIR) --prefix $(PREFIX)

test: release
	$(BUILD_DIR)/docs "how to read a parquet file" 1 >/dev/null
	$(BUILD_DIR)/docs "how to read a csv file" 1 >/dev/null
	$(BUILD_DIR)/docs "varchar" 1 >/dev/null
	$(BUILD_DIR)/docs --raw "parket" 1 | grep -q "Parquet"
	$(BUILD_DIR)/docs --raw "parket" 1 | grep -q "corrected to: parquet"
	$(BUILD_DIR)/docs --snippets --raw "varchar" 1 | grep -q "VARCHAR"
	# Snippets must keep the page-level ranking: result 2 is the Text Types page.
	$(BUILD_DIR)/docs --snippets --raw "varchar" 2 | awk '/(2\/2)/ { found = 1 } found' | grep -q "Text Types"
	# Semantic ranking may place a closely related page first (here the
	# full-text search extension), so check that the type docs rank near the top.
	$(BUILD_DIR)/docs --raw "varkar" 2 | grep -q "Text Types"
	$(BUILD_DIR)/docs --raw "varchar" 2 | grep -q "Text Types"

deps:
	git submodule update --init --recursive

clean:
	rm -rf build

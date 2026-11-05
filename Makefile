BUILD_DIR ?= build/release
PREFIX ?= $(HOME)/.local
CMAKE ?= cmake

.PHONY: all release install clean test test-semantic deps

all: release

release:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build $(BUILD_DIR) --parallel 8

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

# A semantic query prepares the embedding index at startup (with a progress
# line), so the first run can take a couple of minutes. Later runs load the
# cached index. This verifies that the natural-language query reaches Parquet
# content.
test-semantic: release
	$(BUILD_DIR)/docs --raw "how to read a parquet file?" 1 | grep -q "Parquet"

deps:
	git submodule update --init --recursive
	scripts/fetch_models.sh

clean:
	rm -rf build

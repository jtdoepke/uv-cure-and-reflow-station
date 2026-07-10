# Convenience wrappers. PlatformIO remains the source of truth for builds/tests.
.PHONY: help format format-check lint check hooks compiledb tidy test build
.DEFAULT_GOAL := help

help:      ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) | \
		awk 'BEGIN{FS=":.*## "}{printf "  \033[36m%-13s\033[0m %s\n", $$1, $$2}'

# Hand-written C++ (tracked), excluding the vendored LVGL config. clang-format/clang-tidy are
# provided by mise (mise.toml); run `mise install` and activate mise so they're on PATH.
CXX_FILES := $(shell git ls-files '*.c' '*.cpp' '*.h' '*.hpp' ':!:include/lv_conf.h')

format:    ## Auto-format C++ in place
	clang-format -i $(CXX_FILES)

format-check: ## Check C++ formatting without modifying files
	clang-format --dry-run --Werror $(CXX_FILES)

lint:      ## Run all pre-commit hooks (read-only check)
	pre-commit run --all-files

check: lint  ## Alias for `lint`

hooks:     ## Install the pre-commit git hook (one-time)
	pre-commit install

compiledb: ## Regenerate compile_commands.json for clangd/IDE (esp32dev firmware env)
	pio run -e esp32dev -t compiledb
	python3 tools/clangd-inject-sysincludes.py compile_commands.json

# Host-buildable library logic (the firmware glue in src/ and include/LGFX_CYD2USB.hpp needs
# the ESP32 Xtensa toolchain, which clang-tidy can't target — that code is linted by clangd in
# the editor instead, via .clangd). tidy regenerates its own host (native_ui) compile DB each
# run so it's always fresh, uses it from .pio/tidy, then restores the esp32dev DB for clangd.
tidy: ## Local static analysis of host-buildable lib logic (advisory; not a CI gate)
	pio run -e native_ui -t compiledb
	@mkdir -p .pio/tidy && mv -f compile_commands.json .pio/tidy/compile_commands.json
	clang-tidy -p .pio/tidy $$(git ls-files 'lib/**/*.cpp')
	@$(MAKE) --no-print-directory compiledb

test:      ## Host test suites (no board)
	pio test -e native_logic -e native_ui

build:     ## Firmware compile-check
	pio run -e esp32dev

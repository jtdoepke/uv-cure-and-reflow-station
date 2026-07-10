# Convenience wrappers. PlatformIO remains the source of truth for builds/tests.
.PHONY: help format format-check lint check hooks compiledb tidy test build sim sim-shot
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

# Two passes: (1) host-buildable library logic via a fresh native_ui compile DB in .pio/tidy;
# (2) the firmware glue (src/*.cpp, and include/LGFX_CYD2USB.hpp via its TU) via a sanitized
# copy of the esp32dev DB in .pio/tidy-esp32 — tools/tidy-sanitize-compiledb.py applies the
# same Xtensa fixups .clangd describes (clang-tidy doesn't read .clangd). The esp32dev DB is
# restored at the root for clangd in between.
tidy: ## Local static analysis of lib logic + firmware glue (advisory; not a CI gate)
	pio run -e native_ui -t compiledb
	@mkdir -p .pio/tidy && mv -f compile_commands.json .pio/tidy/compile_commands.json
	clang-tidy -p .pio/tidy $$(git ls-files 'lib/**/*.cpp')
	@$(MAKE) --no-print-directory compiledb
	python3 tools/tidy-sanitize-compiledb.py compile_commands.json .pio/tidy-esp32
	clang-tidy -p .pio/tidy-esp32 $$(git ls-files 'src/*.cpp')

test:      ## Host test suites (no board)
	pio test -e native_logic -e native_ui

build:     ## Firmware compile-check
	pio run -e esp32dev

# Headless UI screenshot loop (see the ui-development skill). SIM_OUT sets the PNG path;
# ARGS is the action script, e.g. make sim-shot ARGS="click 160 120 wait 300".
SIM_OUT ?= .pio/sim/ui.png

sim:       ## Build the host UI simulator (env native_sim)
	pio run -e native_sim

sim-shot: sim  ## Render the UI to a PNG: make sim-shot ARGS="click 160 120 wait 300"
	@mkdir -p $(dir $(SIM_OUT))
	.pio/build/native_sim/program --out $(SIM_OUT) $(ARGS)

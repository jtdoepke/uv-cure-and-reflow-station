# Convenience wrappers. PlatformIO remains the source of truth for builds/tests.
.PHONY: help format format-check lint check hooks compiledb tidy test build sim sim-shot \
	dev-flash dev-status dev-shot dev-touch
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

compiledb: ## Regenerate compile_commands.json for clangd/IDE (esp32dev_cyd firmware env)
	pio run -e esp32dev_cyd -t compiledb
	python3 tools/clangd-inject-sysincludes.py compile_commands.json

# Two passes: (1) host-buildable library logic via a fresh native_ui_cyd compile DB in .pio/tidy;
# (2) the firmware glue (src_cyd/*.cpp, and include/LGFX_CYD2432S028.hpp via its TU) via a sanitized
# copy of the esp32dev_cyd DB in .pio/tidy-esp32 — tools/tidy-sanitize-compiledb.py applies the
# same Xtensa fixups .clangd describes (clang-tidy doesn't read .clangd). The esp32dev_cyd DB is
# restored at the root for clangd in between.
tidy: ## Local static analysis of lib logic + firmware glue (advisory; not a CI gate)
	pio run -e native_ui_cyd -t compiledb
	@mkdir -p .pio/tidy && mv -f compile_commands.json .pio/tidy/compile_commands.json
	clang-tidy -p .pio/tidy $$(git ls-files 'lib/**/*.cpp')
	@$(MAKE) --no-print-directory compiledb
	python3 tools/tidy-sanitize-compiledb.py compile_commands.json .pio/tidy-esp32
	clang-tidy -p .pio/tidy-esp32 $$(git ls-files 'src_cyd/*.cpp')

test:      ## Host test suites (no board)
# native_ui_cyd_35 runs the same UI suites against the 3.5" panel's geometry — the suites are
# geometry-independent, so a layout that only works at 320x240 fails here rather than on glass.
	pio test -e native_logic_cyd -e native_ui_cyd -e native_ui_cyd_35 -e native_control

build:     ## Firmware compile-check (both MCUs + both bench envs)
# The bench envs are #if-guarded code paths, so they rot unless something compiles them.
# Unlike esp32dev_cyd_uidev they need no secrets.h, so there is no reason to leave them out.
	pio run -e esp32dev_cyd -e esp32dev_control -e esp32dev_cyd_bench -e esp32dev_control_bench

# Headless UI screenshot loop (see the ui-development skill). SIM_OUT sets the PNG path;
# ARGS is the action script, e.g. make sim-shot ARGS="click 160 120 wait 300".
SIM_OUT ?= .pio/sim/ui.png

sim:       ## Build the host UI simulator (env native_sim)
	pio run -e native_sim

sim-shot: sim  ## Render the UI to a PNG: make sim-shot ARGS="click 160 120 wait 300"
	@mkdir -p $(dir $(SIM_OUT))
	.pio/build/native_sim/program --out $(SIM_OUT) $(ARGS)

# On-device UI dev loop (esp32dev_cyd_uidev env; board on Micro-USB, WiFi creds in
# include/secrets.h — see the ui-development skill).
DEV_OUT ?= .pio/sim/device.png

dev-flash: ## Flash firmware + UI dev tools, then print the device IP
	pio run -e esp32dev_cyd_uidev -t upload

dev-status: ## Query device IP/status over serial (no flash)
	pio run -e esp32dev_cyd_uidev -t status

dev-shot:  ## Screenshot the physical display: make dev-shot IP=192.168.x.x
	tools/cyd-shot.sh $(IP) $(DEV_OUT)

dev-touch: ## Inject a touch on the device: make dev-touch IP=192.168.x.x X=160 Y=120
	curl -sf "http://$(IP)/api/touch/simulate?x=$(X)&y=$(Y)" && echo

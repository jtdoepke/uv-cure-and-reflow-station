# Convenience wrappers. PlatformIO remains the source of truth for builds/tests.
.PHONY: help format format-check lint check hooks compiledb tidy test build sim sim-shot \
	perf perf-baseline perf-diff perf-device fuzz fuzz-corpus fuzz-seed \
	dev-flash dev-status dev-shot dev-touch touch-calib
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

compiledb: ## Regenerate compile_commands.json for clangd/IDE (esp32dev_cyd35 firmware env)
	pio run -e esp32dev_cyd35 -t compiledb
	python3 tools/clangd-inject-sysincludes.py compile_commands.json

# Two passes: (1) host-buildable library logic via a fresh native_ui_cyd compile DB in .pio/tidy;
# (2) the firmware glue (src_cyd/*.cpp, and include/LGFX_CYD2432S028.hpp via its TU) via a sanitized
# copy of the esp32dev_cyd35 DB in .pio/tidy-esp32 — tools/tidy-sanitize-compiledb.py applies the
# same Xtensa fixups .clangd describes (clang-tidy doesn't read .clangd). The esp32dev_cyd35 DB is
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

build:     ## Firmware compile-check (both MCUs + both bench envs + the on-target suite)
# The bench envs are #if-guarded code paths, so they rot unless something compiles them.
# Unlike esp32dev_cyd35_uidev they need no secrets.h, so there is no reason to leave them out.
	pio run -e esp32dev_cyd -e esp32dev_cyd35 -e esp32dev_control \
		-e esp32dev_cyd_bench -e esp32dev_cyd35_bench -e esp32dev_control_bench \
		-e esp32dev_control_sim \
		-e touch_calib_cyd -e touch_calib_cyd35
# The embedded suites need a board to RUN, but not to build — and building them here is the only
# thing standing between them and bit-rot. It was broken for exactly this reason: nothing built it.
	pio test -e embedded --without-testing --without-uploading
	pio test -e embedded_cyd35 --without-testing --without-uploading

# ---------- Fuzzing (host-only, libFuzzer; clang comes from mise — see fuzz/README.md) ----------
# Auto-discovery is the extensibility hinge: every fuzz/fuzz_<name>.cpp is a harness with env
# fuzz_<name> and corpus fuzz/corpus/<name>. Dropping a new one needs no edit here.
FUZZ_ENVS   := $(patsubst fuzz/fuzz_%.cpp,fuzz_%,$(wildcard fuzz/fuzz_*.cpp))
FUZZ_TIME   ?= 60                                    # seconds per harness for `make fuzz`
FUZZ_TARGET ?=                                       # e.g. FUZZ_TARGET=frontdoor runs only that one
FUZZ_RUN    := $(if $(FUZZ_TARGET),fuzz_$(FUZZ_TARGET),$(FUZZ_ENVS))

fuzz:      ## Build + run each libFuzzer harness: make fuzz [FUZZ_TARGET=frontdoor] [FUZZ_TIME=60]
# New coverage-expanding inputs are written to a scratch dir under .pio (git-ignored) so the
# committed seed corpus stays pristine; the committed corpus is passed read-only as a second dir.
	@for e in $(FUZZ_RUN); do echo "== $$e =="; pio run -e $$e >/dev/null; \
		mkdir -p .pio/fuzz/$${e#fuzz_}; \
		.pio/build/$$e/program .pio/fuzz/$${e#fuzz_} fuzz/corpus/$${e#fuzz_} \
			-max_len=1200 -max_total_time=$(FUZZ_TIME) -print_final_stats=1 || exit 1; done

fuzz-corpus: ## Fast regression: load+run every committed corpus once, no mutation (must exit 0)
	@for e in $(FUZZ_RUN); do echo "== $$e =="; pio run -e $$e >/dev/null; \
		.pio/build/$$e/program fuzz/corpus/$${e#fuzz_} -runs=0 || exit 1; done

fuzz-seed: ## Regenerate the committed seed corpus (fuzz/corpus/**) from the real encoder
	pio run -e fuzz_seedgen >/dev/null
	.pio/build/fuzz_seedgen/program

# Headless UI screenshot loop (see the ui-development skill). SIM_OUT sets the PNG path;
# ARGS is the action script, e.g. make sim-shot ARGS="click 160 120 wait 300".
# SIM_PANEL=35 renders the 3.5" 320x480 portrait panel instead of the 2.8" 320x240 landscape —
# the two geometries are different layouts (theme tokens scale, flows flip), so a screenshot is
# only evidence about the panel it was rendered for.
SIM_OUT ?= .pio/sim/ui.png
SIM_ENV = $(if $(filter 35,$(SIM_PANEL)),native_sim_35,native_sim)

sim:       ## Build the host UI simulator: make sim [SIM_PANEL=35]
	pio run -e $(SIM_ENV)

sim-shot: sim  ## Render the UI to a PNG: make sim-shot [SIM_PANEL=35] ARGS="click 160 120"
	@mkdir -p $(dir $(SIM_OUT))
	.pio/build/$(SIM_ENV)/program --out $(SIM_OUT) $(ARGS)

# Host UI performance harness (perf/perf_main.cpp). SIM_PANEL=35 picks the 3.5" geometry, same
# idiom as the sim. One PROCESS PER SCENARIO on purpose: lv_mem_monitor's max_used is a monotonic
# high-water mark with no public reset, so scenarios sharing a process would contaminate each
# other's peak.
#
# The numbers to trust across machines are the task COUNTS (exact, deterministic); the
# microseconds only mean something as a before/after on one machine. Ground truth is the device
# probe, not this.
PERF_ENV = $(if $(filter 35,$(SIM_PANEL)),native_perf_35,native_perf)
PERF_PANEL = $(if $(filter 35,$(SIM_PANEL)),35,28)
PERF_ITERS ?= 50
# The leak scenario reports one row per rebuild, and its signal is the SLOPE — a dozen rebuilds
# show it as clearly as fifty and keep the table readable.
PERF_LEAK_ITERS ?= 12
PERF_SCENARIOS = home settings list stepper keypad press
PERF_OUT ?= .pio/perf/$(PERF_PANEL).tsv
PERF_BASELINE = perf/baseline/$(PERF_PANEL).tsv

perf:      ## Build + run the host perf harness: make perf [SIM_PANEL=35]
	@pio run -e $(PERF_ENV) >/dev/null
	@mkdir -p $(dir $(PERF_OUT))
	@printf '# git=%s\tdirty=%s\tpanel=%s\n' \
		"$$(git rev-parse --short HEAD)" \
		"$$(git diff --quiet && echo 0 || echo 1)" "$(PERF_PANEL)" > $(PERF_OUT)
	@for s in $(PERF_SCENARIOS); do \
		.pio/build/$(PERF_ENV)/program --scenario $$s --iters $(PERF_ITERS) \
			| grep -v '^scenario	metric' >> $(PERF_OUT); \
	done
	@.pio/build/$(PERF_ENV)/program --scenario leak --iters $(PERF_LEAK_ITERS) \
		| grep -v '^scenario	metric' >> $(PERF_OUT)
	@column -t -s'	' $(PERF_OUT)
	@echo "WROTE $(PERF_OUT)"

perf-baseline: perf  ## Re-record the committed host baseline: make perf-baseline [SIM_PANEL=35]
	@mkdir -p $(dir $(PERF_BASELINE))
	@cp $(PERF_OUT) $(PERF_BASELINE)
	@echo "BASELINE $(PERF_BASELINE) — review the git diff before committing"

perf-diff: perf  ## Diff the current run against the committed baseline [SIM_PANEL=35]
	@tools/perf-diff.sh $(PERF_BASELINE) $(PERF_OUT)

# On-glass perf probe (esp32dev_cyd35_perf). Flashes, then drives the serial workload and prints
# the CPU-render vs SPI-flush split. When one board is plugged in it's auto-selected; with several
# attached, tools/resolve-port.sh makes you pass PORT=/dev/ttyUSBn (both CYDs are CH340, so only
# the MAC tells them apart). The _perf_sb twin (DISP_DOUBLE_BUFFER=0) gives the ground-truth SPI
# wall time.
PERF_DEVICE_ENV ?= esp32dev_cyd35_perf
PERF_DEVICE_CMD ?= a

perf-device: ## Flash + run the on-glass perf probe: make perf-device [PORT=/dev/ttyUSBn] [PERF_DEVICE_CMD=a]
	@port=$$(tools/resolve-port.sh $(PORT)) || exit 1; \
	pio run -e $(PERF_DEVICE_ENV) -t upload --upload-port $$port; \
	echo "-- driving probe (cmd=$(PERF_DEVICE_CMD)) --"; \
	tools/perf-device.py $$port $(PERF_DEVICE_CMD)

# On-device UI dev loop (board on Micro-USB, WiFi creds in include/secrets.h — see the
# ui-development skill). DEV_ENV picks the board, mirroring SIM_PANEL/CALIB_ENV; the default is
# the default board. `make dev-shot` only works under DEV_ENV=esp32dev_cyd_uidev — the 3.5"
# panel's SDO is unwired, so its endpoint returns 501 rather than a black PNG.
# One board plugged in is auto-selected; with several attached, tools/resolve-port.sh makes you
# pass PORT=/dev/ttyUSBn (both CYDs are CH340, so only the MAC tells them apart).
DEV_ENV ?= esp32dev_cyd35_uidev
DEV_OUT ?= .pio/sim/device.png

dev-flash: ## Flash firmware + UI dev tools, print the IP: make dev-flash [DEV_ENV=…] [PORT=/dev/ttyUSBn]
	@port=$$(tools/resolve-port.sh $(PORT)) || exit 1; \
	pio run -e $(DEV_ENV) -t upload --upload-port $$port

dev-status: ## Query device IP/status over serial (no flash)
	pio run -e $(DEV_ENV) -t status

dev-shot:  ## Screenshot the physical display: make dev-shot IP=192.168.x.x
	tools/cyd-shot.sh $(IP) $(DEV_OUT)

dev-touch: ## Inject a touch on the device: make dev-touch IP=192.168.x.x X=160 Y=120
	curl -sf "http://$(IP)/api/touch/simulate?x=$(X)&y=$(Y)" && echo

# Touch calibration rig (tools/touch_calibrate). Tap the 15 targets; paste the printed CALIB
# values into that board's LGFX header. CALIB_ENV picks the board.
CALIB_ENV ?= touch_calib_cyd35

touch-calib: ## Calibrate touch on a board: make touch-calib [CALIB_ENV=touch_calib_cyd] [PORT=/dev/ttyUSBn]
	@port=$$(tools/resolve-port.sh $(PORT)) || exit 1; \
	pio run -e $(CALIB_ENV) -t upload --upload-port $$port; \
	echo "Tap the 15 targets, then read the CALIB lines:"; \
	pio device monitor -e $(CALIB_ENV) --port $$port

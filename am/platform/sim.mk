# SPDX-License-Identifier: MPL-2.0
# Run a headless benchmark on refsim-soc, the cycle-exact model of the
# proto-soc chip (proto-core sim/soc, --chip: 100 MHz, 32-bit SDRAM, the
# chip's caches), instead of a board. The image is the PLATFORM=fpga one (same
# AM runtime and flags; the model stops at "AM exit:" before the Boot ROM
# return), so its cycle counts compare with board runs.
PLATFORM_MAKEFILE := am/platform/sim.mk
# SIM_MEDIA=1 (set by `run`, in BUILD_DIR-media): the interactive build. Its
# AM drives mgba's simulation media device (AM_SIM_MEDIA, as PLATFORM=verilator),
# which refsim-soc-media serves with an SDL window, audio and keys while it
# runs the program in fast-forward (no cycle model). Otherwise the headless
# timed image, identical to PLATFORM=fpga.
SIM_MEDIA ?= 0
ifeq ($(SIM_MEDIA),1)
AM_RUNNER_FLAGS := -DAM_SIM_MEDIA -DAM_HAS_WSRAM
else
AM_RUNNER_FLAGS := -DAM_FPGA -DAM_HAS_WSRAM
endif
# The core checkout whose sim/soc builds refsim-soc (its SoC model is
# sim/src/protosoc.c of the same tree). Required, no default.
ifeq ($(strip $(SIM_CORE)),)
$(error PLATFORM=sim requires SIM_CORE=/path/to/proto-core/quad-issue-rvv)
endif
# A leading ~ is expanded here: fish (and quoted arguments) pass it literally.
SIM_CORE_DIR := $(abspath $(patsubst ~%,$(HOME)%,$(strip $(SIM_CORE))))
ifeq ($(wildcard $(SIM_CORE_DIR)/sim/soc/Makefile),)
$(error SIM_CORE=$(SIM_CORE): no $(SIM_CORE_DIR)/sim/soc/Makefile)
endif
REFSIM := $(SIM_CORE_DIR)/sim/soc/refsim-soc
# `make PLATFORM=sim run` needs nothing else: the independent core (gba-next)
# on Dragon Ball with its recorded input, the RV32 translator and the RVV PPU
# kernels, built as for the board. The standard measurement is
# GBN_WARMUP=4500 GBN_FRAMES=120 (SIM_HOST=gpu08: about 13 minutes there).
RISCV_ISA ?= rv32im_zicsr_zifencei_zicbom_zve32x_zvl128b
RISCV_C_ISA ?= rv32im_zicsr_zifencei_zicbom
NEWLIB_ISA ?= rv32im_zicsr_zifencei_zicbom
GBN_RV32 ?= 1
GBN_PPU_RVV ?= 4
GBN_INPUT ?= am/test/data/gba-next-dragonball-play.input
GBN_WARMUP ?= 100
GBN_FRAMES ?= 30
# GBN_CAPTURE=1 (pictures and sound instead of timing) needs the validated PCM.
GBN_VALIDATE ?= $(if $(filter 1,$(GBN_CAPTURE)),1,0)
ifeq ($(SIM_MEDIA),1)
HEADLESS ?= 0
AUDIO ?= 1
FRAMES ?= 0
else
HEADLESS ?= 1
AUDIO ?= 0
BENCHMARK ?= 1
FRAMES ?= 120
WARMUP ?= 30
endif
include am/platform/rv32.mk
ifeq ($(SIM_MEDIA),1)
include am/platform/rtl-media.mk
REFSIM_MEDIA := $(SIM_CORE_DIR)/sim/soc/refsim-soc-media
else ifneq ($(HEADLESS):$(AUDIO):$(BENCHMARK),1:0:1)
$(error PLATFORM=sim requires HEADLESS=1 AUDIO=0 BENCHMARK=1 (the window is `run`))
endif

# Where the model runs. Empty SIM_HOST runs it here; SIM_HOST=gpu08 copies the
# ELF to SIM_REMOTE_ROOT/build/sim-runs on that host and runs the model of the
# mirror of SIM_CORE there, in its tool image (am/tools/run_sim.py).
SIM_HOST ?=
SIM_REMOTE_ROOT ?= proto-core/quad-issue-rvv-wide-peak
# The host CPU of a remote run; empty picks one per run (am/tools/run_sim.py).
SIM_CPU ?=
# Extra refsim-soc options, e.g. SIM_ARGS="--ic-kb 128".
SIM_ARGS ?=
# 1: a per-function profile of the window (am/tools/sim_profile.py).
SIM_PROFILE ?= 0
# A capture.bin of another GBN_CAPTURE=1 run to compare pictures and sound with.
SIM_REFERENCE ?=
SIM_TOOL = $(PYTHON) -u am/tools/run_sim.py --refsim "$(REFSIM)" \
	$(if $(SIM_HOST),--host "$(SIM_HOST)" --remote-root "$(SIM_REMOTE_ROOT)" $(if $(SIM_CPU),--cpu $(SIM_CPU))) \
	$(if $(filter 1,$(SIM_PROFILE)),--profile) $(if $(SIM_REFERENCE),--reference "$(abspath $(SIM_REFERENCE))")
SIM_TAIL = $(if $(strip $(SIM_ARGS)),-- $(SIM_ARGS))

all: $(TARGET)

# Build (or bring up to date) the model from SIM_CORE before a local run.
.PHONY: sim-model sim-model-media
sim-model:
	$(if $(SIM_HOST),,$(MAKE) -C "$(SIM_CORE_DIR)/sim/soc" refsim-soc)
ifeq ($(SIM_MEDIA),1)
sim-model-media: $(RTL_NATIVE)
	$(MAKE) -C "$(SIM_CORE_DIR)/sim/soc" refsim-soc-media AM_MEDIA_ROOT="$(abspath .)" AM_MEDIA_NATIVE="$(RTL_NATIVE)"
endif

.PHONY: run run-mgba
ifeq ($(SIM_MEDIA),1)
# The gba-next player in a window: Makefile run-gba-next-player.
run: run-gba-next-player
else
# Like `make PLATFORM=spike run`: Dragon Ball in a window, picture, sound and
# keys, built apart in BUILD_DIR-media. The timed benchmark is run-gba-next-bench.
run:
	+$(MAKE) PLATFORM=sim SIM_MEDIA=1 BUILD_DIR="$(BUILD_DIR)-media" run
endif
# The mGBA player benchmark (FRAMES/WARMUP), its window from its start line to
# its frame report. Slow: the model runs the interpreter cycle by cycle.
run-mgba: all sim-model
	$(SIM_TOOL) --elf "$(TARGET)" --out "$(BUILD_DIR)/sim-mgba" \
		--begin "Benchmark start:" --end "Frames:" $(SIM_TAIL)

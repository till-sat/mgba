# SPDX-License-Identifier: MPL-2.0
# Spike with the embedded integer vector profile and the target core's VLEN=128.
# Zve32x alone makes Spike use VLEN=32; keep the width explicit for board comparisons.
SPIKE_PREFIX ?= /home/tillsat/tools/spike-zve32x
RISCV_ISA ?= rv32im_zicsr_zifencei_zicbom_zve32x_zvl128b
include am/platform/spike.mk
PLATFORM_MAKEFILE += am/platform/spike_zve32x.mk

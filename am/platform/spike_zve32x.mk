# SPDX-License-Identifier: MPL-2.0
# Spike with the embedded integer vector profile.
SPIKE_PREFIX ?= /home/tillsat/tools/spike-zve32x
RISCV_ISA ?= rv32im_zicsr_zifencei_zicbom_zve32x
include am/platform/spike.mk
PLATFORM_MAKEFILE += am/platform/spike_zve32x.mk

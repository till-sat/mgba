# SPDX-License-Identifier: MPL-2.0
# Included by either proto-core SoC simulator when AM_MEDIA_ROOT is supplied.
AM_MEDIA_DIR := $(AM_MEDIA_ROOT)/am/sim
AM_MEDIA_SRCS := $(AM_MEDIA_DIR)/axi_media_bridge.sv $(AM_MEDIA_DIR)/rtl-media.cc
AM_MEDIA_DEPS := $(AM_MEDIA_SRCS) $(wildcard $(AM_MEDIA_DIR)/*.[hcv]* $(AM_MEDIA_DIR)/*.mk) $(AM_MEDIA_NATIVE)
AM_MEDIA_CFLAGS := -DAM_SIM_MEDIA -I$(AM_MEDIA_DIR) -I$(AM_MEDIA_ROOT)/am/include $(shell pkg-config --cflags sdl2)
AM_MEDIA_LIBS := $(AM_MEDIA_NATIVE) $(shell pkg-config --libs sdl2)
VFLAGS += +define+AM_SIM_MEDIA -I$(AM_MEDIA_DIR) -MAKEFLAGS "OPT_FAST=-O3"

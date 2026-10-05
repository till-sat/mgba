# SPDX-License-Identifier: MPL-2.0
# Build the mGBA player as a standard Abstract Machine application.
#
# Usage:
#   AM_HOME=/path/to/abstract-machine make -C ysyx image \
#       ARCH=riscv32-ysyxsoc
#   AM_HOME=/path/to/abstract-machine make -C ysyx run \
#       ARCH=riscv32-ysyxsoc

ROOT := $(abspath $(CURDIR)/..)
NAME := mgba
ARCH ?= riscv32-ysyxsoc
ROM ?= $(ROOT)/cinema/gba/obj/2d-wrap/test.gba
FRAMES ?= 2
RUNNER_THREADED ?= 0
FRAME_CONFIG := src/frame-config.h
ROM_CONFIG := src/rom-path.stamp

# Keep the core source selection synchronized with the standalone player's
# bare-metal source list. Files requiring POSIX VFS, threads, or floating-point
# resampling remain outside this image.
CORE_SOURCES := $(shell awk '/^CORE_SOURCES :=/ { active = 1; next } active && NF == 0 { exit } active { gsub(/\\/, ""); print $$1 }' $(ROOT)/Makefile)
CORE_SOURCES := $(filter-out \
  src/core/directories.c \
  src/gb/% \
  src/sm83/% \
  src/util/vfs/vfs-dirent.c \
  src/util/vfs/vfs-fd.c \
  src/util/audio-resampler.c \
  src/util/interpolator.c, $(CORE_SOURCES))
CORE_SOURCES += src/gb/audio.c

SRCS := $(addprefix ../,$(CORE_SOURCES)) \
        src/main.c src/player.c src/compat.c src/rom.S

INC_PATH += $(ROOT)/include $(ROOT)/src
CFLAGS += -D_GNU_SOURCE -DAM_BAREMETAL -DNDEBUG \
          -DHAVE_STRLCPY \
          -include $(ROOT)/include/mgba/flags.h \
          -include $(CURDIR)/$(FRAME_CONFIG) \
          -include $(CURDIR)/include/ysyx-config.h \
          -Wno-unused-parameter -Wno-sign-compare -Wno-format -Wno-nonnull -Wno-nonnull-compare

ifeq ($(RUNNER_THREADED),1)
CFLAGS += -DMGBA_RUNNER_THREADED
endif

.PHONY: FORCE test
FORCE:

$(FRAME_CONFIG): FORCE
	@{ echo '/* generated: make FRAMES=$(FRAMES) RUNNER_THREADED=$(RUNNER_THREADED) */'; echo '#define AM_FRAME_LIMIT $(FRAMES)'; } > $@.tmp
	@cmp -s $@.tmp $@ 2>/dev/null || cp $@.tmp $@
	@rm -f $@.tmp

$(ROM_CONFIG): FORCE
	@printf '%s\n' '$(abspath $(ROM))' > $@.tmp
	@cmp -s $@.tmp $@ 2>/dev/null || cp $@.tmp $@
	@rm -f $@.tmp

src/rom.S: $(ROM) $(ROM_CONFIG)
	@mkdir -p $(@D)
	@{ \
		echo '.section .rodata.rom,"a",@progbits'; \
		echo '.balign 4'; \
		echo '.globl _rom_start'; \
		echo '.globl _rom_end'; \
		echo '_rom_start:'; \
		echo '.incbin "$(abspath $(ROM))"'; \
		echo '_rom_end:'; \
	} > $@

include $(AM_HOME)/Makefile

$(OBJS): $(FRAME_CONFIG)

test: run

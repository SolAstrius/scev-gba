# scev-cores/game-boy-advance — GBA on RVVM bare-metal.
#
# Vendors gdkGBA (gdkchan, Unlicense / public domain) under
# vendor/gdkGBA/ for the GBA core. ~6000 lines of C across nine
# files, no libc deps inside the core beyond a handful of mallocs
# at boot. Consumes rvvm-hal (vendor/rvvm-hal as a git submodule
# pinned to v0.8.0) for device drivers — same as game-boy.
#
# Stage: bringing the port up incrementally. This Makefile compiles
# the core's .c files with a small shim layer (malloc / printf) and
# our scev glue under src/. The frontend pieces (main.c, sdl.c) of
# gdkGBA are NOT compiled — replaced by our src/main.c + the
# rvvm-hal device drivers.
#
# Build: `make`        produces firmware.bin
# Run:   `make run ROM=roms/foo.gba BIOS=roms/gba_bios.bin`
#                      boots under RVVM with -bochs_display -hda_test
# Clean: `make clean`

HAL      := vendor/rvvm-hal
GDKGBA   := vendor/gdkGBA
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

RVVM     ?= $(shell command -v rvvm 2>/dev/null || \
                    echo /home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64)

# Include order: src/ first (own headers + libc stubs), HAL headers,
# then the gdkGBA tree last (its headers expect to be found in the
# include path as bare names — `#include "arm.h"` etc).
CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable \
            -Wno-unused-function -Wno-unused-variable \
            -fcommon \
            -Isrc/stub-libc -Isrc -I$(HAL)/include -I$(GDKGBA)

# -fcommon is needed because gdkGBA's headers declare globals
# WITHOUT `extern` (e.g. `uint8_t *bios;` in arm_mem.h). Pre-C23
# compilers folded these tentative definitions into a single
# common symbol; newer zig-cc defaults to -fno-common, which
# turns each TU's include into its own definition and the
# linker rejects duplicates. -fcommon restores legacy behaviour
# without patching the vendored tree.

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld

SCEV_OBJS   := build/main.o build/gdkgba_shim.o
GDKGBA_OBJS := build/arm.o build/arm_mem.o build/dma.o build/io.o \
               build/sound.o build/timer.o build/video.o
OBJS        := $(SCEV_OBJS) $(GDKGBA_OBJS)

all: firmware.bin

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# gdkGBA core compilation. Same flags as our own sources; we suppress
# a few warnings the upstream tree triggers (it's pre-MISRA C, has
# casts and shadowing we don't want to fix in a vendored copy).
GDKGBA_CFLAGS := $(CFLAGS) -Wno-shadow -Wno-sign-compare \
                 -Wno-implicit-fallthrough -Wno-pointer-sign \
                 -Wno-unused-result

build/%.o: $(GDKGBA)/%.c
	@mkdir -p build
	$(CC) $(GDKGBA_CFLAGS) -c -o $@ $<

$(HAL)/libhal.a:
	$(MAKE) -C $(HAL)

# gdkGBA patches. Applied as a one-shot to the submodule because
# our scev_* extensions (arm_skip_bios, IRQ-fire counters, IWRAM
# write tracker) need to live INSIDE vendor/gdkGBA/ — they touch
# static state that has no public accessor. Marker file means
# don't re-apply on every build. To clear: rm vendor/gdkGBA/.patched
$(GDKGBA)/.patched: patches/01-scev-skip-bios-and-irq-counters.patch
	cd $(GDKGBA) && git apply ../../patches/01-scev-skip-bios-and-irq-counters.patch
	@touch $@
	@echo "patches: applied scev extensions to $(GDKGBA)"

$(GDKGBA_OBJS): $(GDKGBA)/.patched

firmware.elf: $(OBJS) $(HAL)/libhal.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

ROM ?= roms/test.gba

# Save staging — same shape as game-boy. GBA save sizes vary by cart
# type (SRAM 32 KiB, Flash 64/128 KiB, EEPROM 512 B / 8 KiB), so we
# default to a 128 KiB blob that fits the largest. The firmware will
# read only the bytes the cart asks for.
define ENSURE_SAVE
	@if [ -n "$(SAVE)" ] && [ ! -f "$(SAVE)" ]; then \
	    mkdir -p $$(dirname "$(SAVE)"); \
	    dd if=/dev/zero of="$(SAVE)" bs=1024 count=128 status=none; \
	    echo "save: created empty $(SAVE) (128 KB)"; \
	fi
endef

# BIOS handling. gdkGBA needs a real GBA BIOS (16 KiB) — it doesn't
# implement HLE. Pass BIOS=roms/gba_bios.bin via the environment;
# we attach it as NVMe controller 1. ROM is controller 0; SAVE is
# controller 2 if provided.
BIOS ?= roms/gba_bios.bin

# Sector-pad helper. RVVM's nvme reports `floor(file_size / 512)`
# LBAs, which truncates ROMs that aren't a multiple of 512 bytes —
# tested with the 1300-byte hello.gba homebrew, where the trailing
# 276 bytes (font glyph table) got lost and corrupted the rendered
# text. Run this against any ROM before `make run` to round it up
# to an LBA boundary in place. Real commercial GBA carts are 4-32
# MiB and naturally aligned, so this is mostly a homebrew concern.
define ENSURE_SECTOR_ALIGNED
	@orig=$$(stat -c %s "$(1)"); \
	pad=$$(( (orig + 511) & ~511 )); \
	if [ $$pad -ne $$orig ]; then \
	    dd if=/dev/zero bs=1 count=$$((pad - orig)) >> "$(1)" status=none; \
	    echo "rom: padded $(1) $$orig → $$pad bytes"; \
	fi
endef

# NVMe slot order matters: cart on 0, BIOS on 1, save on 2 (if any).
# Mirrors how the firmware will discover them via nvme_init_nth().
NVME_FLAGS := -nvme $(ROM)
ifneq ($(wildcard $(BIOS)),)
NVME_FLAGS += -nvme $(BIOS)
endif
ifneq ($(SAVE),)
NVME_FLAGS += -nvme $(SAVE)
endif

run: firmware.bin
	@test -f "$(ROM)"  || { echo "missing $(ROM); set ROM=path/to/cart.gba"; exit 1; }
	@test -f "$(BIOS)" || { echo "missing $(BIOS); GBA needs a real BIOS at roms/gba_bios.bin"; exit 1; }
	$(call ENSURE_SECTOR_ALIGNED,$(ROM))
	$(call ENSURE_SECTOR_ALIGNED,$(BIOS))
	$(ENSURE_SAVE)
	$(RVVM) firmware.bin -bochs_display -nonet -hda_test $(NVME_FLAGS)

run-headless: firmware.bin
	@test -f "$(ROM)"  || { echo "missing $(ROM); set ROM=path/to/cart.gba"; exit 1; }
	@test -f "$(BIOS)" || { echo "missing $(BIOS); GBA needs a real BIOS at roms/gba_bios.bin"; exit 1; }
	$(call ENSURE_SECTOR_ALIGNED,$(ROM))
	$(call ENSURE_SECTOR_ALIGNED,$(BIOS))
	$(ENSURE_SAVE)
	$(RVVM) firmware.bin -nogui -nonet -hda_test $(NVME_FLAGS)

# `make run-noaudio` — quicker boot, no HDA. Useful while iterating
# on the video / shim path without ALSA in the loop.
run-noaudio: firmware.bin
	@test -f "$(ROM)"  || { echo "missing $(ROM); set ROM=path/to/cart.gba"; exit 1; }
	@test -f "$(BIOS)" || { echo "missing $(BIOS); GBA needs a real BIOS at roms/gba_bios.bin"; exit 1; }
	$(call ENSURE_SECTOR_ALIGNED,$(ROM))
	$(call ENSURE_SECTOR_ALIGNED,$(BIOS))
	$(RVVM) firmware.bin -bochs_display -nonet $(NVME_FLAGS)

clean:
	rm -rf build firmware.elf firmware.bin
	$(MAKE) -C $(HAL) clean

.PHONY: all run run-headless run-noaudio clean

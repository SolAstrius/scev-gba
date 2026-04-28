# scev-cores/game-boy-advance — GBA on RVVM bare-metal.
#
# Vendors mGBA (mgba-emu, MPL-2.0) under vendor/mgba/ for the GBA
# core. mGBA's HLE BIOS removes the legal-BIOS hassle binjgb-ish
# emulators have, and the file-level MPL doesn't propagate into our
# scev glue. Consumes rvvm-hal (vendor/rvvm-hal as a git submodule
# pinned to v0.8.0) for device drivers — same as game-boy.
#
# Build: `make`        produces firmware.bin
# Run:   `make run ROM=roms/foo.gba`
#                      boots under RVVM with -bochs_display -hda_test
# Clean: `make clean`
#
# Stage of the port (see README): scaffolding only. main.c boots,
# brings up uart/fdt/pci/i2c/time/gfx/hid, paints a banner via
# gfx_text, and halts. mGBA integration follows.

HAL      := vendor/rvvm-hal
MGBA     := vendor/mgba
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

RVVM     ?= $(shell command -v rvvm 2>/dev/null || \
                    echo /home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64)

# Include order: src/ first (for our own headers + libc stubs), then
# HAL headers. mGBA's include path will be added once the integration
# step pulls its sources in — for now the scaffold only links against
# libhal.a + our own glue.
CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable \
            -Wno-unused-function -Wno-unused-variable \
            -Isrc/stub-libc -Isrc -I$(HAL)/include

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld

SCEV_OBJS := build/main.o
OBJS      := $(SCEV_OBJS)

all: firmware.bin

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

$(HAL)/libhal.a:
	$(MAKE) -C $(HAL)

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

run: firmware.bin
	@test -f "$(ROM)" || { echo "missing $(ROM); set ROM=path/to/cart.gba"; exit 1; }
	$(ENSURE_SAVE)
	$(RVVM) firmware.bin -bochs_display -nonet -hda_test -nvme $(ROM) $(if $(SAVE),-nvme $(SAVE))

run-headless: firmware.bin
	@test -f "$(ROM)" || { echo "missing $(ROM); set ROM=path/to/cart.gba"; exit 1; }
	$(ENSURE_SAVE)
	$(RVVM) firmware.bin -nogui -nonet -hda_test -nvme $(ROM) $(if $(SAVE),-nvme $(SAVE))

# `make run-noaudio` — quicker boot, no HDA. Useful while iterating
# on the video / shim path without ALSA in the loop.
run-noaudio: firmware.bin
	@test -f "$(ROM)" || { echo "missing $(ROM); set ROM=path/to/cart.gba"; exit 1; }
	$(RVVM) firmware.bin -bochs_display -nonet -nvme $(ROM)

clean:
	rm -rf build firmware.elf firmware.bin
	$(MAKE) -C $(HAL) clean

.PHONY: all run run-headless run-noaudio clean

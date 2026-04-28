# scev-cores/game-boy-advance

Game Boy Advance (AGB / ARM7TDMI @ 16.78 MHz) on RVVM bare-metal.

**Status: real GBA carts boot.** Pokemon FireRed and Zelda:
Minish Cap reach their title screens / intros. Skip-BIOS workaround
in place (the BIOS splash itself still hangs — see KNOWN_ISSUES).
Audio + saves not yet wired.

## Why mGBA

- **Accurate** — the gold-standard open-source GBA emulator.
- **MPL-2.0**, file-level weak copyleft. Doesn't propagate into our
  scev glue; the vendored mGBA source remains MPL.
- **HLE BIOS** (`gba/hle-bios.[cs]`) — no proprietary BIOS file
  required for boot.
- C-based, modular layout (`arm/` CPU, `gba/` system, `core/`
  frontend abstraction we shim). Comparable to binjgb's port shape.

## Layout (target — mostly empty in scaffold stage)

```
src/main.c           scev boot + frame loop
src/mgba_shim.c      libc shim: bump alloc, printf→uart,
                     pthread/mutex stubs, VFS over FileData blobs
src/stub-libc/       <assert.h>, <stdio.h>, <pthread.h>, etc
vendor/rvvm-hal/     submodule pinned to v0.8.0 (audio_pcm stereo)
vendor/mgba/         submodule, selected subdirs only
roms/                user-supplied .gba carts (gitignored)
saves/               generated, gitignored
```

## Build & run

```
nix develop          # zig + llvm-objcopy + alsa runtime libs
make                 # produces firmware.bin
make run ROM=roms/your.gba
```

`make run` boots RVVM with `-bochs_display -hda_test
-nvme <ROM>`. Optional second NVMe slot (`SAVE=path`) backs cart
ext-RAM (SRAM / Flash / EEPROM, autodetected from cart magic).

## Display / audio plan

- **Display**: 240×160 native, ×4 = 960×640 host. Same scale as GB.
- **Audio**: stereo 32.768 kHz native (the GBA's APU rate). Streams
  through the rvvm-hal v0.8.0 stereo `audio_pcm` path; RVVM's HDA
  worker downmixes L+R for ALSA.

## License

Scaffold sources under MIT (see `LICENSE` once added). Vendored
`mgba/` retains MPL-2.0 file-level. Vendored `rvvm-hal/` is MIT.

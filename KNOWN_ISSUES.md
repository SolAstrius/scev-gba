# Known issues

## BIOS splash hangs (Timer-1 driven)

The BIOS's "GAME BOY" splash IRQ handler (installed at IWRAM
`0x03007FFC` = `0x300` for the splash phase) only acts on Timer-1
IRQs:

```
0x300: MOV r3, #0x04000000
0x304: LDR r2, [r3, #0x200]    ; IE | (IF<<16)
0x308: AND r2, r2, r2 LSR #16  ; r2 = IE & IF
0x30C: ANDS r1, r2, #0x80      ; test SIO bit (multiboot)
0x310: LDRNE pc, [pc, #0x7A0]  ; if SIO → handler @ 0x2D70
0x318: LDREQ pc, [pc, #0x79C]  ; else      → handler @ 0x210C
```

We provide VBlank IRQs but the BIOS expects Timer-1 too — gdkGBA's
`tmr[1]` stays uninitialized at `ctrl=0`, so Timer 1 IRQs never fire,
so the splash counter never advances.

**Workaround**: skip the BIOS splash via `arm_skip_bios()` in
`vendor/gdkGBA/arm.c` (SCEV addition). Sets registers per GBATEK §3.2
BIOS-reset-defaults, jumps directly to `0x08000000`. Loses the boot
animation; everything after that works normally.

## Audio not yet wired

gdkGBA's `sound.c` mix path is unconnected. Audio playback is silent.
Wiring up `audio_pcm channels=2` on top of gdkGBA's mix output is
straightforward (mirror the game-boy core's `push_audio` shape, plus
HPF since GBA's APU has the same u8-zero-silence convention as DMG).

## Saves not yet wired

gdkGBA's `eeprom`/`sram`/`flash` regions are allocated but no NVMe
backing. Cart-type detection lives in gdkGBA's MBC code based on a
magic-string scan of the ROM (`"SRAM_V"`, `"FLASH_V"`, `"EEPROM_V"`).

## Status

What works:
- Real commercial GBA carts boot and run after `arm_skip_bios()`.
  Pokemon FireRed (16 MiB) and Zelda: Minish Cap (16 MiB) confirmed
  reaching their title screens / intros.
- Small homebrew (jsmolka's `ppu/hello.gba`, `ppu/shades.gba`) renders
  static frames from cart code.
- Frame loop at 60 Hz, ~10 ms run + 600 µs blit, plenty of slack.
- Full diagnostic dump in prof line: PC, CPSR mode/I bit, halt state,
  IE/IF/IME, DISPCNT/DISPSTAT, BIOS IntrWait completion var, IRQ
  counters.

What's pending:
- BIOS splash boot path (Timer 1 plumbing through gdkGBA)
- Audio mix → audio_pcm
- Save / battery RAM via NVMe disk 2
- HID → key_input wiring is in but untested with games that read it

## Earlier rabbit hole, for the record

Before the rvvm-hal v0.8.1 fix, `nvme_read` of any single transfer
larger than ~2 MiB silently corrupted memory: its `setup_prp` wrote
PRP entries linearly into a 4 KiB buffer with no chain support, and
overflowed past the buffer's end into adjacent BSS. For 16 MiB carts
that put PRP-entry bytes (4 KiB-aligned page addresses) over the
first ~28 KiB of `cart_buf`, and DMA'd cart data to where the PRP
entries pointed (random spots inside `cart_buf` and elsewhere).
Visible as: emulator's PC walking sequentially through zero-padded
ROM end at ~13 MiB/sec. Identical pattern across FireRed and Minish
Cap because both got the same corruption. Fixed in HAL v0.8.1 with
proper PRP chaining + internal chunking for transfers > 32 MiB. See
the rvvm-hal commit `8ec4e0b` for the gory details.

# Known issues

## BIOS splash hangs on most carts (Timer-1 driven path)

The BIOS's "GAME BOY" splash IRQ handler (installed at IWRAM
`0x03007FFC` = `0x300` for the splash phase) **only acts on
Timer-1 IRQs** — see decoded instructions:

```
0x300: MOV r3, #0x04000000
0x304: LDR r2, [r3, #0x200]    ; IE | (IF<<16)
0x308: AND r2, r2, r2 LSR #16  ; r2 = IE & IF
0x30C: ANDS r1, r2, #0x80      ; test SIO bit (multiboot)
0x310: LDRNE pc, [pc, #0x7A0]  ; if SIO → handler @ 0x2D70
0x318: LDREQ pc, [pc, #0x79C]  ; else      → handler @ 0x210C
```

We provide VBlank IRQs (`trigger=58/sec`, `armint=54/sec`), but the
BIOS splash needs Timer-1 firing too. gdkGBA's `timer.c` driving
hasn't been verified against BIOS expectations; meanwhile our
`tmr[1].ctrl=0x0000` shows Timer 1 was never enabled.

**Workaround**: skip the BIOS splash entirely. `arm_skip_bios()`
in `vendor/gdkGBA/arm.c` (SCEV addition) sets registers per
GBATEK §3.2 BIOS-reset-defaults and jumps directly to cart entry
at `0x08000000`. Loses the boot animation; gains "everything past
that".

## FireRed (and likely other commercial carts) walks ROM linearly

With skip-BIOS, simple homebrew test ROMs work cleanly:

- `ppu/hello.gba` (jsmolka) → PC settles at `0x08000168` (cart idle loop), `DISPCNT=0x404` (mode 4 + BG2)
- `ppu/shades.gba` → PC settles at `0x08000164`, `DISPCNT=0x100`

But Pokemon FireRed goes off the rails — PC walks linearly through
ROM at ~13 MB/sec (one prof window apart):

```
pc=0x08AB71FC → 0x0956E3FC → 0x0A0166CC → 0x0A6844CC → 0x0ACF22CC ...
```

Each address is in cart-ROM space but always increasing. Diagnosis:
gdkGBA's CPU is treating uninitialised post-cart memory as code
(NOP-stream — PC just increments). Root cause is one of gdkGBA's
ARM/Thumb instructions misbehaving early in FireRed's init
sequence, leaving PC pointing past the cart's intended branch.

gdkGBA describes itself as "in early stages of development" in its
README — this is consistent with that. Commercial titles with rich
init sequences exercise paths that simple homebrew doesn't.

**Where to dig next**:

1. Diff CPU instruction coverage between hello.gba's first ~50
   instructions (works) and FireRed's first ~50 (fails). FireRed
   does `MOV cpsr_c, IRQ`, sets up multiple banked stacks, etc.
   `MSR cpsr_c, *` and the bank-switch path is a likely suspect.
2. Single-step trace from `0x08000000` for the first ~200 instr
   (add a TRACE flag to `arm_step`/`t16_step`) and compare against
   a known-good emulator's trace.
3. Or: vendor mGBA after all (it's more accurate but ~30× the
   port effort — see git history for the mGBA scaffolding I
   tried first).

## What works in this firmware today

- gdkGBA core builds + runs freestanding with the shim
- BIOS loads, cart loads, framebuffer at 240×160 ×4 = 960×640
- `arm_skip_bios()` boots straight to cart, no splash
- Small homebrew test ROMs (hello, shades) execute and stabilise
  in their cart idle loops with valid display state
- Audio is **not yet wired** — the `sound.c` mix path is unconnected
- Saves are **not yet wired** — no NVMe disk 2 detection
- HID is wired but unused (cart code doesn't read `key_input`
  on the homebrew ROMs we've tested)

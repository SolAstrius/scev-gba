/* scev-cores/game-boy-advance — GBA on RVVM bare-metal.
 *
 * Vendors gdkchan/gdkGBA (Unlicense / public domain) for the GBA
 * core. ~6000 lines of C across nine .c files; the frontend pieces
 * (main.c, sdl.c) are NOT compiled — replaced here. We provide
 * malloc/free via bump allocator (gdkgba_shim.c) and a small
 * SDL_LockTexture stub that no-ops so the global `screen` pointer
 * we set below survives across frame calls.
 *
 * Boot:
 *   1. Standard rvvm-hal init: UART, FDT, PCI/I2C/time, HID, gfx.
 *   2. Allocate framebuffer (240×160 ×4 BGRA), point gdkGBA's
 *      `screen` global at it.
 *   3. NVMe disk 0 → cart ROM (up to 32 MiB).
 *      NVMe disk 1 → GBA BIOS (16 KiB exactly, real or replacement).
 *      NVMe disk 2 → save (optional, type detected at runtime).
 *   4. arm_init() (allocates GBA memory regions via shim malloc).
 *   5. memcpy ROM + BIOS into the allocated buffers.
 *   6. arm_reset(), then frame loop:
 *        poll HID → key_input.w → run_frame() → blit FB to bochs
 *        VRAM ×4 → wfi-pace at 60 Hz.
 *
 * Audio is NOT yet wired (see TODO at the bottom). Stage: black
 * screen / playable-graphics target before audio. */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "pci.h"
#include "i2c.h"
#include "hid.h"
#include "nvme.h"
#include "gfx.h"
#include "rvvm.h"

#include "arm.h"
#include "arm_mem.h"
#include "io.h"
#include "video.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

extern char __bss_start[], __bss_end[];

void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);

extern size_t gdkgba_shim_used_bytes(void);
extern size_t gdkgba_shim_pool_bytes(void);

/* ===== Display ============================================== */

#define GBA_W      240
#define GBA_H      160
#define GBA_SCALE  4
#define DISPLAY_W  (GBA_W * GBA_SCALE)
#define DISPLAY_H  (GBA_H * GBA_SCALE)

/* gdkGBA writes BGRA8888 into `screen`. Allocate the framebuffer
 * here (rather than letting the bump pool do it) so its address is
 * known at compile time and the size doesn't eat into the shim
 * pool's 40 MiB reserve. */
__attribute__((aligned(16)))
static uint8_t fb_bgra[GBA_W * GBA_H * 4];

/* ===== Cart / BIOS staging ================================= */

#define CART_ROM_MAX   (32u * 1024u * 1024u)
#define BIOS_BYTES     16384u

__attribute__((aligned(NVME_PAGE_SIZE)))
static uint8_t cart_buf[CART_ROM_MAX];

__attribute__((aligned(NVME_PAGE_SIZE)))
static uint8_t bios_buf[BIOS_BYTES];

/* ===== Globals ============================================== */

extern void *screen;            /* video.c — frame target */

static gfx_t          g;
static hid_keyboard_t kb;
static uint16_t       host_key_mask = 0xFFFF;  /* gdkGBA active-low,
                                                  10 buttons in bits 0..9 */

/* ===== HID → GBA buttons ==================================== */
/*
 * gdkGBA's key_input is active-low: bit cleared = button pressed.
 * Default = 0xFFFF (no buttons held). Mapping mirrors gdkGBA's
 * own SDL frontend (Z=A, X=B etc shifted to a more natural layout
 * that matches what game-boy/main.c uses):
 *
 *   D-pad arrows  → BTN_U/D/L/R
 *   Z             → A
 *   X             → B
 *   A             → L (shoulder)
 *   S             → R (shoulder)
 *   Enter         → Start
 *   Right Shift   → Select
 */
static void on_key(uint8_t usage, bool pressed, void *ctx) {
    (void)ctx;
    uint16_t bit = 0;
    switch (usage) {
    case 0x52: bit = BTN_U;   break;   /* Up */
    case 0x51: bit = BTN_D;   break;   /* Down */
    case 0x50: bit = BTN_L;   break;   /* Left */
    case 0x4F: bit = BTN_R;   break;   /* Right */
    case 0x1D: bit = BTN_A;   break;   /* Z */
    case 0x1B: bit = BTN_B;   break;   /* X */
    case 0x04: bit = BTN_LT;  break;   /* A */
    case 0x16: bit = BTN_RT;  break;   /* S */
    case 0x28: bit = BTN_STA; break;   /* Enter */
    case 0xE5: bit = BTN_SEL; break;   /* RShift */
    default: return;
    }
    if (pressed) host_key_mask &= ~bit;
    else         host_key_mask |=  bit;
}

/* ===== gfx blit (×4 BGRA → vram XRGB) ====================== */
/*
 * gdkGBA writes 32-bit BGRA per pixel (B in low byte). Bochs/RVVM's
 * gfx surface is XRGB8888 (R in high byte) when GFX_FMT_XRGB8888.
 * Same R↔B swap pattern as game-boy/main.c. Hot loop, so each
 * source pixel is unrolled into 16 destination stores (×4 scale). */
static void blit_frame(uint32_t x_off, uint32_t y_off) {
    uint32_t *vram   = g.vram;
    uint32_t  stride = g.stride_px;
    bool      need_swap = (g.format == GFX_FMT_XRGB8888);

    for (uint32_t y = 0; y < GBA_H; y++) {
        uint32_t  base = (y_off + y * GBA_SCALE) * stride + x_off;
        uint32_t *r0   = &vram[base];
        uint32_t *r1   = &vram[base + stride];
        uint32_t *r2   = &vram[base + 2 * stride];
        uint32_t *r3   = &vram[base + 3 * stride];
        const uint32_t *src = (const uint32_t *)&fb_bgra[y * GBA_W * 4];

        for (uint32_t x = 0; x < GBA_W; x++) {
            uint32_t c = src[x] & 0x00FFFFFFu;
            if (need_swap) {
                /* BGRA8888 in src → XRGB8888 in dst.
                 * src layout: B in bits 0:7, G 8:15, R 16:23.
                 * dst wants:  R in bits 16:23 already (matches!).
                 * Wait — gdkGBA's texture format is BGRA8888 from
                 * SDL's perspective, which on little-endian = bytes
                 * [B,G,R,A] = uint32 0xAARRGGBB. So R is already in
                 * bits 16:23. No swap needed for XRGB8888 dst.
                 * Setting need_swap=false here just to be safe — */
                uint32_t r = (c >> 16) & 0xFF;
                uint32_t g_ = (c >>  8) & 0xFF;
                uint32_t b = (c >>  0) & 0xFF;
                c = (r << 16) | (g_ << 8) | b;
            }
            uint32_t dx = x * GBA_SCALE;
            r0[dx] = r0[dx+1] = r0[dx+2] = r0[dx+3] = c;
            r1[dx] = r1[dx+1] = r1[dx+2] = r1[dx+3] = c;
            r2[dx] = r2[dx+1] = r2[dx+2] = r2[dx+3] = c;
            r3[dx] = r3[dx+1] = r3[dx+2] = r3[dx+3] = c;
        }
    }
}

/* ===== Cart loader ========================================== */
/*
 * Round up to the next power of 2 — gdkGBA's cart_rom_mask wants
 * (size_pow2 - 1) so reads past the cart's end mirror cleanly per
 * GBA hardware behaviour. Mirrors gdkGBA/main.c::to_pow2. */
static uint32_t to_pow2(uint32_t v) {
    v--;
    v |= v >>  1; v |= v >>  2; v |= v >>  4;
    v |= v >>  8; v |= v >> 16;
    return v + 1;
}

static uintptr_t fdt_addr_of(const fdt_t *fdt, const char *compat,
                             uintptr_t fallback) {
    uint32_t off = fdt_find_compatible(fdt, compat);
    if (off == UINT32_MAX) return fallback;
    uint64_t addr = 0;
    if (!fdt_node_reg64(fdt, off, 0, &addr, NULL)) return fallback;
    return (uintptr_t)addr;
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\nscev-cores/game-boy-advance — GBA on RVVM (gdkGBA)\n");
    uart_printf("hartid=%u  fdt=%p  bss=%u bytes\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

    /* FDT-driven driver re-init. */
    fdt_t fdt;
    bool fdt_ok = fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr);
    if (fdt_ok) {
        uart_init(fdt_addr_of(&fdt, "ns16550a",              RVVM_UART_BASE));
        pci_init(fdt_addr_of(&fdt, "pci-host-ecam-generic",  RVVM_PCI_ECAM_BASE));
        i2c_init(fdt_addr_of(&fdt, "opencores,i2c-ocores",   RVVM_I2C_OC_BASE));
        uint32_t cpus = fdt_find_node_named(&fdt, "cpus");
        uint32_t hz = 0;
        if (cpus != UINT32_MAX) fdt_node_prop_u32(&fdt, cpus, "timebase-frequency", &hz);
        time_init(fdt_addr_of(&fdt, "sifive,clint0", RVVM_CLINT_BASE), hz);
    } else {
        pci_init(0);
        i2c_init(RVVM_I2C_OC_BASE);
        time_init(RVVM_CLINT_BASE, 0);
    }
    hid_kb_init(&kb, RVVM_I2C_HID_KEYBOARD);

    /* Display. 240×160 ×4 = 960×640. */
    bool have_gfx = gfx_init_fdt(&g, &fdt, DISPLAY_W, DISPLAY_H);
    if (have_gfx) gfx_fill(&g, 0x00000000);
    else          uart_puts("gfx: no display backend; running blind\n");

    /* === BIOS === NVMe disk 1, 16 KiB exactly. */
    static nvme_t bios_disk;
    if (!nvme_init_nth(&bios_disk, 1)) {
        uart_puts("FATAL: GBA BIOS disk not attached "
                  "(start RVVM with -nvme roms/cart.gba -nvme roms/gba_bios.bin).\n");
        for (;;) __asm__ volatile ("wfi");
    }
    uint32_t bios_lbas = (BIOS_BYTES + NVME_LBA_SIZE - 1) / NVME_LBA_SIZE;
    if (bios_disk.num_lbas < bios_lbas) {
        uart_printf("FATAL: BIOS disk only %u LBAs, need %u\n",
                    (uint64_t)bios_disk.num_lbas, (uint64_t)bios_lbas);
        for (;;) __asm__ volatile ("wfi");
    }
    if (nvme_read(&bios_disk, 0, bios_buf, bios_lbas) != bios_lbas) {
        uart_puts("FATAL: BIOS read failed\n");
        for (;;) __asm__ volatile ("wfi");
    }
    uart_printf("bios: loaded %u bytes from NVMe disk 1\n",
                (uint64_t)BIOS_BYTES);

    /* === Cart ROM === NVMe disk 0, up to 32 MiB. */
    static nvme_t cart_disk;
    if (!nvme_init_nth(&cart_disk, 0)) {
        uart_puts("FATAL: cart disk not attached\n");
        for (;;) __asm__ volatile ("wfi");
    }
    uint64_t disk_bytes = (uint64_t)cart_disk.num_lbas * NVME_LBA_SIZE;
    if (disk_bytes > CART_ROM_MAX) disk_bytes = CART_ROM_MAX;
    uint32_t cart_lbas = (uint32_t)(disk_bytes / NVME_LBA_SIZE);
    if (nvme_read(&cart_disk, 0, cart_buf, cart_lbas) != cart_lbas) {
        uart_puts("FATAL: cart read failed\n");
        for (;;) __asm__ volatile ("wfi");
    }
    uart_printf("cart: loaded %u bytes from NVMe disk 0 (%u LBAs)\n",
                disk_bytes, (uint64_t)cart_lbas);

    /* === gdkGBA bring-up === arm_init allocates 32+ MiB internally
     * via our bump allocator. */
    arm_init();
    uart_printf("gdkGBA: shim pool used %u / %u KiB after arm_init\n",
                (uint64_t)(gdkgba_shim_used_bytes() >> 10),
                (uint64_t)(gdkgba_shim_pool_bytes() >> 10));

    /* Stamp BIOS + ROM into the gdkGBA-allocated buffers. */
    memcpy(bios, bios_buf, BIOS_BYTES);
    memcpy(rom,  cart_buf, (size_t)disk_bytes);
    cart_rom_size = (int64_t)disk_bytes;
    cart_rom_mask = to_pow2((uint32_t)disk_bytes) - 1u;

    /* Point video.c's frame target at our static framebuffer. The
     * SDL_LockTexture stub no-ops, so this stays valid across all
     * subsequent run_frame() calls. */
    screen = fb_bgra;

    arm_reset();
    uart_puts("Running.\n\n");

    uint32_t x_off = (have_gfx && g.width  > DISPLAY_W) ? (g.width  - DISPLAY_W) / 2 : 0;
    uint32_t y_off = (have_gfx && g.height > DISPLAY_H) ? (g.height - DISPLAY_H) / 2 : 0;

    /* Pace at 60 Hz wall-clock (GBA's vblank rate is 59.7275 Hz —
     * close enough for now, we can refine later). */
    const uint64_t ticks_per_frame = RVVM_TIME_HZ / 60;
    uint64_t deadline = time_now() + ticks_per_frame;

    uint32_t prof_iters = 0;
    uint64_t prof_run = 0, prof_blit = 0, prof_pace = 0;
    uint64_t prof_window = time_now();

    for (;;) {
        uint64_t t0 = time_now();
        hid_kb_poll(&kb, on_key, NULL);
        key_input.w = host_key_mask;

        run_frame();
        uint64_t t1 = time_now();
        prof_run += t1 - t0;

        if (have_gfx) blit_frame(x_off, y_off);
        uint64_t t2 = time_now();
        prof_blit += t2 - t1;

        time_busy_until(deadline);
        uint64_t t3 = time_now();
        prof_pace += t3 - t2;
        deadline += ticks_per_frame;

        if (++prof_iters >= 60) {
            uint64_t window = t3 - prof_window;
            #define US(t) ((uint64_t)((t) / 10))
            uart_printf("[prof] iters=%u wall=%ums | run=%uus blit=%uus pace=%uus\n",
                        (uint64_t)prof_iters,
                        US(window) / 1000,
                        US(prof_run)  / prof_iters,
                        US(prof_blit) / prof_iters,
                        US(prof_pace) / prof_iters);
            #undef US
            prof_iters = 0;
            prof_run = prof_blit = prof_pace = 0;
            prof_window = t3;
        }
    }

    /* TODO: audio. gdkGBA's sound.c writes into a stereo S16
     * mixing buffer; we'd plumb that through audio_pcm channels=2
     * once the video path is verified. */
}

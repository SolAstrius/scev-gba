/* scev-cores/game-boy-advance — GBA on RVVM bare-metal.
 *
 * SCAFFOLD STAGE — this main.c brings up the same boot path as the
 * other cores (UART → FDT → PCI/I2C/time → HID → gfx) and prints a
 * banner. mGBA integration is the next step: vendor the core,
 * wire the shim, plug emulator_run_frame into the loop.
 *
 * Boot sequence (target shape, mirrors game-boy/main.c):
 *   1. Standard rvvm-hal init: UART, FDT, PCI, I2C, time, HID, gfx.
 *   2. Audio: HDA + audio_pcm channels=2 for the GBA's stereo APU.
 *   3. NVMe: read the cart ROM (up to 32 MB) into a static buffer.
 *   4. mGBA instantiation. Use HLE BIOS so no proprietary BIOS
 *      file ships with the firmware.
 *   5. Frame loop: poll HID → mCoreRunFrame → blit framebuffer →
 *                  push audio → wfi-pace.
 *
 * Display: 240×160 at native res, scaled ×4 to 960×640 for the
 * Bochs surface. Audio: stereo 32.768 kHz native (matches the
 * GBA's APU; mGBA can resample if we ask for 44.1 kHz instead). */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "pci.h"
#include "i2c.h"
#include "hid.h"
#include "gfx.h"
#include "rvvm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

extern char __bss_start[], __bss_end[];

/* Scaffold target dimensions — mGBA's GBA core renders at 240×160
 * native. We bump ×4 to fill the host window; same scale game-boy
 * uses. */
#define GBA_W      240
#define GBA_H      160
#define GBA_SCALE  4
#define DISPLAY_W  (GBA_W * GBA_SCALE)
#define DISPLAY_H  (GBA_H * GBA_SCALE)

static gfx_t          g;
static hid_keyboard_t kb;

static void on_key(uint8_t usage, bool pressed, void *ctx) {
    (void)ctx;
    /* Trace every HID event for now — we have no emulator state
     * to feed yet, but we want to confirm the GUI window's keyboard
     * is reaching the firmware. Remove once we wire mGBA's joypad
     * surface in. */
    uart_printf("hid: usage=%x %s\n",
                (uint64_t)usage, pressed ? "DOWN" : "up");
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
    uart_puts("\nscev-cores/game-boy-advance — GBA on RVVM (SCAFFOLD)\n");
    uart_printf("hartid=%u  fdt=%p  bss=%u bytes\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

    /* FDT discovery + driver re-init with discovered addresses.
     * Fallback path mirrors game-boy/main.c. */
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

    /* Graphics. 240×160 ×4 → 960×640. Paint a slate background so
     * the scaffold-stage firmware shows it's alive even before mGBA
     * is wired in. */
    bool have_gfx = gfx_init_fdt(&g, &fdt, DISPLAY_W, DISPLAY_H);
    if (have_gfx) {
        gfx_fill(&g, 0x00102030);
        uart_printf("gfx: %ux%u backend up (format=%u stride=%u)\n",
                    (uint64_t)g.width, (uint64_t)g.height,
                    (uint64_t)g.format, (uint64_t)g.stride_px);
    } else {
        uart_puts("gfx: no display backend; running blind\n");
    }

    uart_puts("\nscaffold stage: mGBA core not yet integrated.\n"
              "Halting in HID-poll loop — press keys on the GUI window\n"
              "to confirm input reaches the firmware.\n\n");

    /* Idle loop. 60 Hz pacing so we can observe HID poll without
     * pinning a host CPU core. */
    const uint64_t ticks_per_frame = RVVM_TIME_HZ / 60;
    uint64_t deadline = time_now() + ticks_per_frame;
    for (;;) {
        hid_kb_poll(&kb, on_key, NULL);
        time_busy_until(deadline);
        deadline += ticks_per_frame;
    }
}

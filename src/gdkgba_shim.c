/* gdkgba_shim — bump allocator + tracer plumbing for gdkGBA.
 *
 * Most of gdkGBA's libc needs are now satisfied by picolibc (vendored
 * in rvvm-hal): mem/str helpers, exit/abort path, assert. Only the
 * things picolibc can't reasonably provide for our use case live here:
 *
 *   §1  Bump allocator (malloc/calloc/realloc/free) — gdkGBA's
 *       arm_init() requests ~32 MiB of memory regions at boot
 *       (BIOS, WRAM, IWRAM, PRAM, VRAM, OAM, ROM, EEPROM, SRAM,
 *       FLASH) and never frees. Our bump pool is 40 MiB; picolibc's
 *       sbrk-fed nano-malloc would have to expand a HAL heap that
 *       defaults to 16 MiB and would also pay per-block header
 *       overhead. Keeping our own malloc/free means the link prefers
 *       these over picolibc's libc.a equivalents.
 *
 *   §2  exit() override — picolibc's exit walks __init_array /
 *       __fini_array via linker symbols we don't define. Skip
 *       straight to _exit() (wfi loop in
 *       rvvm-hal/src/picolibc_hooks.c).
 *
 *   §3  Instruction tracer globals — main.c arms by writing
 *       scev_trace_remaining; the patched arm_step / t16_step in
 *       vendor/gdkGBA/ call scev_trace_cb. */

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>     /* _exit */

#include "uart.h"

/* ====================================================================
 * §1. Bump allocator.
 *
 * Sized to fit gdkGBA's arm_init() footprint with comfortable slack:
 *   bios    16 KiB
 *   wram   256 KiB
 *   iwram   32 KiB
 *   pram     1 KiB
 *   vram    96 KiB
 *   oam      1 KiB
 *   rom   32768 KiB    ← dominates
 *   eeprom   8 KiB
 *   sram    64 KiB
 *   flash  128 KiB
 *   ─────────────
 *   total ≈ 33152 KiB ≈ 32.4 MiB.
 *
 * Round to 40 MiB for headroom + per-alloc 16-byte alignment loss.
 * The HAL link.ld bumps LENGTH to 256 MiB, so 40 MiB is fine.
 * ==================================================================== */
#define BUMP_POOL_BYTES   (40u * 1024u * 1024u)

__attribute__((aligned(4096)))
static uint8_t bump_pool[BUMP_POOL_BYTES];
static size_t  bump_used = 0;

static void *bump_alloc(size_t n, int zero) {
    /* 16-byte alignment — safe for any vector load gdkGBA might
     * emit and for the cache-line alignment of the GBA memory
     * regions. */
    size_t aligned_used = (bump_used + 15u) & ~(size_t)15u;
    if (n == 0) n = 1;
    if (aligned_used + n > BUMP_POOL_BYTES) {
        uart_printf("gdkgba_shim: bump exhausted (used=%u + want=%u > pool=%u)\n",
                    (uint64_t)bump_used, (uint64_t)n,
                    (uint64_t)BUMP_POOL_BYTES);
        return NULL;
    }
    uint8_t *p = &bump_pool[aligned_used];
    bump_used = aligned_used + n;
    if (zero) {
        for (size_t i = 0; i < n; i++) p[i] = 0;
    }
    return p;
}

void *malloc(size_t n)              { return bump_alloc(n, 0); }
void *calloc(size_t count, size_t n){ return bump_alloc(count * n, 1); }
void  free(void *p)                 { (void)p; /* leak — bump pool */ }

extern void *memcpy(void *, const void *, size_t);
void *realloc(void *old_ptr, size_t new_size) {
    void *fresh = bump_alloc(new_size, 0);
    if (!fresh) return NULL;
    if (old_ptr) memcpy(fresh, old_ptr, new_size);
    return fresh;
}

/* Diagnostics — main.c can use these for boot-time logs. */
size_t gdkgba_shim_used_bytes(void) { return bump_used; }
size_t gdkgba_shim_pool_bytes(void) { return BUMP_POOL_BYTES; }

/* ====================================================================
 * §2. exit() override
 * ==================================================================== */
__attribute__((noreturn))
void exit(int status) {
    _exit(status);
}

/* ====================================================================
 * §3. Instruction tracer globals
 *
 * The vendored arm_step / t16_step (patched via patches/01-*.patch)
 * find these via extern declarations there. main.c arms by writing
 * scev_trace_remaining = N. Format mirrors a mGBA `-l 0x4000` trace
 * well enough for hand-diffing.
 * ==================================================================== */
volatile int scev_trace_remaining = 0;

static void scev_trace_default(int thumb, uint32_t pc, uint32_t op,
                               uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                               uint32_t sp, uint32_t lr) {
    uart_printf("T%c %x %x  r0=%x r1=%x r2=%x r3=%x sp=%x lr=%x\n",
                (uint64_t)(thumb ? '1' : '0'),
                (uint64_t)pc, (uint64_t)op,
                (uint64_t)r0, (uint64_t)r1, (uint64_t)r2, (uint64_t)r3,
                (uint64_t)sp, (uint64_t)lr);
}

void (*scev_trace_cb)(int, uint32_t, uint32_t,
                      uint32_t, uint32_t, uint32_t, uint32_t,
                      uint32_t, uint32_t) = scev_trace_default;

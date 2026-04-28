/* gdkgba_shim — freestanding shim layer for gdkGBA.
 *
 * gdkGBA's core (arm.c, arm_mem.c, dma.c, io.c, sound.c, timer.c,
 * video.c) is much smaller-footprint than binjgb's was: arm.c uses
 * `malloc`/`free` for the boot-time memory regions, and that's it.
 * No printf, fread, fopen, qsort, etc. inside the core. We only
 * need to provide:
 *
 *   malloc / calloc / realloc / free   bump allocator on a static
 *                                      pool. arm.c calls this 10×
 *                                      at boot for the GBA memory
 *                                      regions (BIOS, WRAM, ROM,
 *                                      etc.) and never frees during
 *                                      run, so the bump strategy
 *                                      fits perfectly. ~32.5 MiB
 *                                      total request across all
 *                                      arm_init() allocations.
 *
 *   exit / abort                       panic + wfi.
 *
 *   __assert_fail                      glibc-style; zig-cc emits
 *                                      this for assert() calls.
 *
 *   memchr / strlen / strcmp etc       not actually called by the
 *                                      gdkGBA core, but provided
 *                                      for safety in case some
 *                                      header drags them in.
 *
 * (sdl.c and main.c from gdkGBA are NOT compiled — they're the
 * frontend we replace with src/main.c + the rvvm-hal device
 * drivers. So we don't need printf or any file I/O surface here.) */

#include <stdint.h>
#include <stddef.h>

#include "uart.h"

/* ---------------------------------------------------------------- */
/* Bump allocator.                                                   */
/*                                                                   */
/* Sized to fit gdkGBA's arm_init() footprint with comfortable        */
/* slack:                                                             */
/*   bios    16 KiB                                                   */
/*   wram   256 KiB                                                   */
/*   iwram   32 KiB                                                   */
/*   pram     1 KiB                                                   */
/*   vram    96 KiB                                                   */
/*   oam      1 KiB                                                   */
/*   rom   32768 KiB    ← dominates                                  */
/*   eeprom   8 KiB                                                   */
/*   sram    64 KiB                                                   */
/*   flash  128 KiB                                                   */
/*   ─────────────                                                    */
/*   total ≈ 33152 KiB ≈ 32.4 MiB.                                   */
/*                                                                    */
/* Round to 40 MiB for headroom + per-alloc 16-byte alignment loss.  */
/* The HAL link.ld bumps LENGTH to 256 MiB, so 40 MiB is fine.       */
/* ---------------------------------------------------------------- */
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

/* ---------------------------------------------------------------- */
/* exit / abort / assert.                                            */
/* ---------------------------------------------------------------- */

void exit(int code) {
    uart_printf("\ngdkgba_shim: exit(%d). halting.\n", (int64_t)code);
    for (;;) __asm__ volatile ("wfi");
}

void abort(void) {
    uart_puts("\ngdkgba_shim: abort. halting.\n");
    for (;;) __asm__ volatile ("wfi");
}

void __assert_fail(const char *expr, const char *file, unsigned line,
                   const char *func) {
    uart_printf("\ngdkGBA assert FAIL: %s\n  at %s:%u in %s\n",
                expr ? expr : "?", file ? file : "?",
                (uint64_t)line, func ? func : "?");
    for (;;) __asm__ volatile ("wfi");
}

/* ---------------------------------------------------------------- */
/* String helpers (precautionary — not currently called by core).   */
/* ---------------------------------------------------------------- */

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    unsigned char target = (unsigned char)c;
    while (n--) { if (*p == target) return (void *)p; p++; }
    return NULL;
}
size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
char *strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char *)s; s++; }
    return (c == 0) ? (char *)s : NULL;
}
char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) { if (*s == (char)c) last = s; s++; }
    if (c == 0) return (char *)s;
    return (char *)last;
}

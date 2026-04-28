/* Minimal SDL2 stubs for the freestanding gdkGBA port.
 *
 * gdkGBA's vendored sdl.h pulls in <SDL2/SDL.h> for both the
 * frontend (sdl.c, main.c — which we replace) AND four calls in
 * video.c that bracket the per-frame texture update:
 *
 *   SDL_LockTexture(texture, NULL, &screen, &tex_pitch)
 *   SDL_UnlockTexture(texture)
 *   SDL_RenderCopy(renderer, texture, NULL, NULL)
 *   SDL_RenderPresent(renderer)
 *
 * In our setup `screen` is a void* set by main.c to point at our
 * own RGBA framebuffer, so SDL_LockTexture must NOT overwrite it
 * (it's a no-op). The other three are presentation-side and also
 * no-op for us — main.c blits screen → bochs vram each frame.
 *
 * We deliberately avoid pulling in real <SDL2/SDL.h> on the freestanding
 * RV target (huge headers, depends on libc / X11 / Wayland prototypes
 * that don't exist here). */

#ifndef SCEV_GBA_SDL2_STUB_H
#define SCEV_GBA_SDL2_STUB_H

#include <stdint.h>
#include <stddef.h>    /* NULL — video.c passes NULL rect to SDL_LockTexture */

typedef struct SDL_Window   SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture  SDL_Texture;
typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

/* No-op stubs. video.c's SDL_LockTexture leaves *pixels untouched
 * so main.c's pre-set `screen` global stays valid across frames. */
static inline int  SDL_LockTexture(SDL_Texture *t, const SDL_Rect *r,
                                   void **pixels, int *pitch) {
    (void)t; (void)r; (void)pixels; (void)pitch; return 0;
}
static inline void SDL_UnlockTexture(SDL_Texture *t) { (void)t; }
static inline int  SDL_RenderCopy(SDL_Renderer *r, SDL_Texture *t,
                                  const SDL_Rect *src, const SDL_Rect *dst) {
    (void)r; (void)t; (void)src; (void)dst; return 0;
}
static inline void SDL_RenderPresent(SDL_Renderer *r) { (void)r; }

#endif

/* Empty — gdkGBA's sdl.h includes this but the bits we actually
 * compile (video.c) don't reference any SDL audio symbols. The
 * frontend pieces that DO (sdl.c, sound_mix's callback signature)
 * are not part of our compile set; we drive audio from main.c
 * via the rvvm-hal audio_pcm path instead. */

#ifndef SCEV_GBA_SDL2_AUDIO_STUB_H
#define SCEV_GBA_SDL2_AUDIO_STUB_H
#endif

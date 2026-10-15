#ifndef SNESRECOMP_PLATFORM_H
#define SNESRECOMP_PLATFORM_H

/*
 * SDL3 + OpenGL ES 3.0 platform layer — window, GL context, audio output,
 * input events and the fixed-timestep scheduler.
 *
 * Game logic and rendering are decoupled:
 *   - logic runs in fixed ticks of 1/60.0988 s (one SNES NTSC frame);
 *     platform_ticks_due() says how many to run before the next present
 *   - each tick hands its framebuffer to platform_submit_frame()
 *   - platform_present() composes the latest frame (640x480 internal
 *     target, 856x480 in 16:9 widescreen → 480p/720p/1080p output) and
 *     swaps, at most once per tick on a 60 Hz vsync'd display (locked 60 FPS)
 *
 * You usually don't call these directly — the snesrecomp_* frame API wraps
 * them. They're here if you need finer control.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SNES output resolution (LakeSnes PPU buffer, 2x doubled) */
#define SNES_RENDER_WIDTH   512
#define SNES_RENDER_HEIGHT  478

/* Fixed logic tick: one NTSC SNES frame (21.477 MHz / 357366 clocks). */
#define SNES_TICK_HZ 60.0988

/* Output resolution presets (window size, or fullscreen display mode). */
enum {
    PLATFORM_RES_480P  = 0,   /*  640 x 480  */
    PLATFORM_RES_720P  = 1,   /* 1280 x 720  */
    PLATFORM_RES_1080P = 2,   /* 1920 x 1080 */
};
enum {
    PLATFORM_WINDOWED   = 0,
    PLATFORM_FULLSCREEN = 1,  /* exclusive, display switched to the preset */
    PLATFORM_BORDERLESS = 2,  /* desktop-resolution fullscreen */
};

/* Command-line overrides applied at init (pass -1 to keep the saved
 * config). Call before platform_init / snesrecomp_init. */
void platform_set_launch_options(int resolution, int display_mode);

/* Initialize SDL3 window + GL ES 3.0 context + audio. `scale` is unused
 * (kept for API compatibility with the SDL2 layer). */
bool platform_init(const char *window_title, int scale);

/* Hand over the latest frame (uploaded at present): XBGR rows of `width`
 * pixels (2 per SNES column; 512 for 4:3, more in widescreen), of which rows
 * y..y+height-1 are the active picture. The internal render target takes the
 * frame's aspect: 640x480 for 256 columns, 2.5 px per column beyond that. */
void platform_submit_frame(const uint8_t *pixels, int width, int y, int height);

/* Save the next presented frame (window pixels, menu included) as a BMP. */
void platform_request_screenshot(const char *path);

/* Compose + present the most recently submitted frame and the menu. */
void platform_present(void);

/* Queue audio samples (interleaved stereo int16 @ 32040 Hz) */
void platform_queue_audio(const int16_t *samples, int sample_count);

/* Poll SDL events. Returns false if quit was requested. */
bool platform_poll_events(void);

/* Fixed-timestep scheduler: blocks until at least one logic tick is due and
 * returns how many to run (1..3; backlog beyond that is dropped). */
int  platform_ticks_due(void);

/* Rolling stats for the FPS overlay. */
void platform_get_stats(float *present_fps, float *tick_rate, float *frame_ms);

/* Clean shutdown */
void platform_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_PLATFORM_H */

/*
 * SDL3 platform layer — window, OpenGL ES 3.0 context, audio stream, events
 * and the fixed-timestep scheduler. See platform.h for the frame model.
 */

#include "snesrecomp/platform.h"
#include "snesrecomp/mp_session.h"
#include "snesrecomp/menu_overlay.h"
#include "snesrecomp/snesrecomp.h"
#include "gfx/gl.h"
#include "gfx/renderer.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window     *s_window  = NULL;
static SDL_GLContext   s_gl      = NULL;
static bool            s_have_gl = false;
static SDL_AudioStream *s_audio  = NULL;

/* Latest submitted frame (uploaded at present). */
static const uint8_t  *s_pending_fb = NULL;
static int             s_pending_w = 0, s_pending_y = 0, s_pending_h = 0;

/* Live display settings (mirrors the menu; applied when they change). */
static int s_cur_res   = -1;
static int s_cur_mode  = -1;
static int s_cur_vsync = -1;       /* requested */
static bool s_vsync_on = false;    /* actually granted by the driver */
static int s_launch_res  = -1;
static int s_launch_mode = -1;

/* Scheduler state. */
static bool     s_fast = false;     /* SMK_FAST: no throttle, no vsync */
static uint64_t s_last_ns = 0;
static double   s_acc = 0.0;

/* Stats (1 s window). */
static uint64_t s_stat_t0 = 0;
static int      s_stat_presents = 0, s_stat_ticks = 0;
static float    s_fps = 0.0f, s_tps = 0.0f, s_frame_ms = 0.0f;
static uint64_t s_prev_present_ns = 0;

/* Audio: running average of the queue depth drives a tiny resample-ratio
 * correction, so the 60.0988 Hz emulation never drifts against the device
 * clock (no buffer overruns/underruns, no audible pitch change). */
#define AUDIO_RATE        32040
#define AUDIO_FRAME_BYTES (534 * 2 * (int)sizeof(int16_t))
static double s_audio_avg = 0.0;

static const double TICK_S = 1.0 / SNES_TICK_HZ;

static void res_size(int res, int *w, int *h) {
    switch (res) {
    case PLATFORM_RES_1080P: *w = 1920; *h = 1080; break;
    case PLATFORM_RES_720P:  *w = 1280; *h = 720;  break;
    default:                 *w = 640;  *h = 480;  break;
    }
}

void platform_set_launch_options(int resolution, int display_mode) {
    s_launch_res  = resolution;
    s_launch_mode = display_mode;
}

/* Create the window + a GL context of the requested flavour. On failure
 * everything is torn down so the next flavour starts from a clean slate
 * (on X11/Windows the window's pixel format depends on the GL flavour). */
static bool create_window_and_context(bool es, const char *title, int w, int h,
                                      SDL_WindowFlags flags) {
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        es ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, es ? 0 : 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);     /* depth lives in the internal FBO */
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    s_window = SDL_CreateWindow(title, w, h, flags | SDL_WINDOW_OPENGL);
    if (!s_window) {
        fprintf(stderr, "platform: SDL_CreateWindow (%s) failed: %s\n",
                es ? "GLES" : "GL", SDL_GetError());
        return false;
    }
    s_gl = SDL_GL_CreateContext(s_window);
    if (!s_gl || !SDL_GL_MakeCurrent(s_window, s_gl) || !smkgl_load(es)) {
        fprintf(stderr, "platform: %s context unavailable: %s\n",
                es ? "OpenGL ES 3.0" : "OpenGL 3.3 core", SDL_GetError());
        if (s_gl) SDL_GL_DestroyContext(s_gl);
        SDL_DestroyWindow(s_window);
        s_gl = NULL;
        s_window = NULL;
        return false;
    }
    return true;
}

static void apply_display_settings(bool force) {
    int res  = menu_overlay_get_resolution();
    int mode = menu_overlay_get_display_mode();
    if (!force && res == s_cur_res && mode == s_cur_mode) return;

    int w, h;
    res_size(res, &w, &h);
    if (mode == PLATFORM_WINDOWED) {
        SDL_SetWindowFullscreen(s_window, false);
        SDL_SetWindowSize(s_window, w, h);
        SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else if (mode == PLATFORM_FULLSCREEN) {
        /* Exclusive: switch the display to the closest mode to the preset at
         * ~60 Hz (lets a Pi drive a 4K TV at 720p/1080p). */
        SDL_DisplayMode dm;
        SDL_DisplayID disp = SDL_GetDisplayForWindow(s_window);
        if (SDL_GetClosestFullscreenDisplayMode(disp, w, h, 60.0f, false, &dm))
            SDL_SetWindowFullscreenMode(s_window, &dm);
        else
            SDL_SetWindowFullscreenMode(s_window, NULL);
        SDL_SetWindowFullscreen(s_window, true);
    } else {
        SDL_SetWindowFullscreenMode(s_window, NULL);   /* borderless desktop */
        SDL_SetWindowFullscreen(s_window, true);
    }
    s_cur_res = res;
    s_cur_mode = mode;
}

static void apply_vsync(bool force) {
    int vsync = (s_fast || !s_have_gl) ? 0 : menu_overlay_get_vsync();
    if (!force && vsync == s_cur_vsync) return;
    s_cur_vsync = vsync;
    s_vsync_on = false;
    if (!s_have_gl) return;
    if (SDL_GL_SetSwapInterval(vsync ? 1 : 0)) {
        s_vsync_on = vsync != 0;
    } else if (vsync) {
        /* No vsync (some VMs / virtual displays): the scheduler's sleep
         * still holds logic and presents to the 60.0988 Hz tick. */
        fprintf(stderr, "platform: vsync unavailable (%s) — pacing with timers\n", SDL_GetError());
    }
}

bool platform_init(const char *window_title, int scale) {
    (void)scale;
    const char *title = window_title ? window_title : "snesrecomp";
    bool headless = getenv("SMK_HEADLESS") != NULL;
    s_fast = getenv("SMK_FAST") != NULL;

    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    printf("platform: SDL %d.%d.%d, video driver '%s'\n",
           SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
           SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver());

    /* Menu settings are needed before the window exists (resolution etc.):
     * menu_overlay_init with no context only loads the config. */
    menu_overlay_init(NULL, NULL);
    if (s_launch_res >= 0)  menu_overlay_set_resolution(s_launch_res);
    if (s_launch_mode >= 0) menu_overlay_set_display_mode(s_launch_mode);

    int w, h;
    res_size(menu_overlay_get_resolution(), &w, &h);
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (headless) flags |= SDL_WINDOW_HIDDEN;

    /* GL flavour: OpenGL ES 3.0 first (the target API), desktop GL 3.3 core
     * as a fallback. SMK_GL=gl / SMK_GL=es forces one. */
    const char *force = getenv("SMK_GL");
    bool try_es = !force || SDL_strcasecmp(force, "gl") != 0;
    bool try_gl = !force || SDL_strcasecmp(force, "es") != 0;
    if (try_es && create_window_and_context(true, title, w, h, flags)) s_have_gl = true;
    else if (try_gl && create_window_and_context(false, title, w, h, flags)) s_have_gl = true;

    if (s_have_gl) {
        printf("platform: %s | %s | %s\n", (const char *)glGetString(GL_VERSION),
               (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VENDOR));
        if (!renderer_init()) {
            fprintf(stderr, "platform: renderer init failed\n");
            s_have_gl = false;
        }
    }
    if (!s_have_gl) {
        if (!headless) {
            fprintf(stderr, "platform: no usable OpenGL ES 3.0 / OpenGL 3.3 context\n");
            platform_shutdown();
            return false;
        }
        /* Headless runs (CI, scripted RE harnesses) work without a GPU. */
        if (!s_window) s_window = SDL_CreateWindow(title, w, h, flags);
        printf("platform: headless without GL — rendering disabled\n");
    }

    /* Audio: SDL3 stream converts 32040 Hz S16 stereo to the device format. */
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = AUDIO_RATE;
    s_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (s_audio) SDL_ResumeAudioStreamDevice(s_audio);
    else fprintf(stderr, "platform: no audio device (%s) — continuing silent\n", SDL_GetError());

    /* ImGui menu overlay on the GL context (no-op in headless/scripted runs). */
    if (s_have_gl) menu_overlay_init(s_window, s_gl);

    if (s_window) apply_display_settings(true);
    apply_vsync(true);

    s_last_ns = SDL_GetTicksNS();
    s_stat_t0 = s_last_ns;
    return true;
}

/* Screenshot of the next presented frame (dev / testing). */
static char s_shot_path[512];

void platform_request_screenshot(const char *path) {
    SDL_strlcpy(s_shot_path, path ? path : "", sizeof(s_shot_path));
}

/* Read back the default framebuffer and write a 24-bit BMP. */
static void save_screenshot(const char *path, int w, int h) {
    uint8_t *px = (uint8_t *)SDL_malloc((size_t)w * h * 4);
    if (!px) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        int row = (w * 3 + 3) & ~3;
        uint32_t size = 54u + (uint32_t)row * (uint32_t)h;
        uint8_t hdr[54] = { 'B', 'M' };
        hdr[2] = size & 0xFF; hdr[3] = (size >> 8) & 0xFF; hdr[4] = (size >> 16) & 0xFF; hdr[5] = size >> 24;
        hdr[10] = 54; hdr[14] = 40;
        hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF; hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, 54, f);
        uint8_t *line = (uint8_t *)SDL_calloc(1, (size_t)row);
        for (int y = 0; y < h && line; y++) {        /* GL rows are bottom-up, like BMP */
            const uint8_t *src = px + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                line[x * 3 + 0] = src[x * 4 + 2];
                line[x * 3 + 1] = src[x * 4 + 1];
                line[x * 3 + 2] = src[x * 4 + 0];
            }
            fwrite(line, 1, (size_t)row, f);
        }
        SDL_free(line);
        fclose(f);
        printf("platform: screenshot -> %s\n", path);
    }
    SDL_free(px);
}

void platform_submit_frame(const uint8_t *pixels, int width, int y, int height) {
    s_pending_fb = pixels;
    s_pending_w = width;
    s_pending_y = y;
    s_pending_h = height;
    s_stat_ticks++;
}

void platform_present(void) {
    uint64_t now = SDL_GetTicksNS();
    if (s_prev_present_ns) s_frame_ms = (float)((now - s_prev_present_ns) / 1e6);
    s_prev_present_ns = now;
    s_stat_presents++;
    if (now - s_stat_t0 >= 1000000000ull) {
        double secs = (now - s_stat_t0) / 1e9;
        s_fps = (float)(s_stat_presents / secs);
        s_tps = (float)(s_stat_ticks / secs);
        s_stat_presents = s_stat_ticks = 0;
        s_stat_t0 = now;
    }
    if (!s_have_gl) return;

    apply_display_settings(false);
    apply_vsync(false);

    if (s_pending_fb) {
        renderer_upload_snes(s_pending_fb, s_pending_w, s_pending_y, s_pending_h);
        s_pending_fb = NULL;
    }

    int pw = 0, ph = 0, ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(s_window, &pw, &ph);
    SDL_GetWindowSize(s_window, &ww, &wh);
    float px_scale = (wh > 0) ? (float)ph / (float)wh : 1.0f;
    int menu_h = (int)(menu_overlay_get_menubar_height() * px_scale + 0.5f);
    int br = 0, bg = 0, bb = 0;
    if (menu_h > 0) menu_overlay_get_bar_color(&br, &bg, &bb);

    RendererSettings rs;
    rs.filter    = menu_overlay_get_filter();
    rs.fxaa      = menu_overlay_get_fxaa();
    rs.scanlines = menu_overlay_get_scanlines();
    renderer_draw(pw, ph, menu_h, br / 255.0f, bg / 255.0f, bb / 255.0f, &rs);
    menu_overlay_render();
    if (s_shot_path[0]) {
        save_screenshot(s_shot_path, pw, ph);
        s_shot_path[0] = 0;
    }
    SDL_GL_SwapWindow(s_window);
}

void platform_queue_audio(const int16_t *samples, int sample_count) {
    if (!s_audio || !samples || sample_count <= 0) return;
    int bytes = sample_count * 2 * (int)sizeof(int16_t);
    int queued = SDL_GetAudioStreamQueued(s_audio);
    if (queued > AUDIO_FRAME_BYTES * 10) {
        /* Way behind (window drag, debugger, uncapped run): resync. */
        SDL_ClearAudioStream(s_audio);
        queued = 0;
        s_audio_avg = 0.0;
    }
    /* Target ~3 frames (~50 ms) queued; nudge the playback ratio by at most
     * ±0.5% to hold it there. */
    const double target = AUDIO_FRAME_BYTES * 3.0;
    s_audio_avg = s_audio_avg * 0.95 + queued * 0.05;
    double err = (s_audio_avg - target) / target;
    if (err > 1.0) err = 1.0;
    if (err < -1.0) err = -1.0;
    SDL_SetAudioStreamFrequencyRatio(s_audio, (float)(1.0 + err * 0.005));
    SDL_SetAudioStreamGain(s_audio, menu_overlay_get_volume());
    SDL_PutAudioStreamData(s_audio, samples, bytes);
}

/* Mouse motion accumulator (defined in input.c) */
extern void recomp_input_accumulate_mouse(int dx, int dy);

bool platform_poll_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        /* Let the ImGui menu see every event first; if it captured the event
         * (typing in a field, rebinding, clicking a menu) the game ignores it. */
        int consumed = menu_overlay_process_event(&ev);
        switch (ev.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return false;
        case SDL_EVENT_KEY_DOWN:
            if (!consumed && !ev.key.repeat) {
                /* Esc quits only when the menu isn't using it (rebind-cancel). */
                if (ev.key.key == SDLK_ESCAPE) return false;
                /* Save-state hotkeys (mirror File -> Save / Load). */
                if (ev.key.key == SDLK_F5) snesrecomp_save_state("smk_state.sav");
                if (ev.key.key == SDLK_F8 && (mp_get_state() == MP_IDLE || mp_get_state() == MP_DISCONNECTED))
                    snesrecomp_load_state("smk_state.sav");   /* not during netplay */
                if (ev.key.key == SDLK_F11 ||
                    (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT))) {
                    int m = menu_overlay_get_display_mode();
                    menu_overlay_set_display_mode(m == PLATFORM_WINDOWED ? PLATFORM_BORDERLESS
                                                                         : PLATFORM_WINDOWED);
                }
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (!consumed)
                recomp_input_accumulate_mouse((int)ev.motion.xrel, (int)ev.motion.yrel);
            break;
        default:
            break;
        }
    }
    if (menu_overlay_quit_requested()) return false;
    return true;
}

int platform_ticks_due(void) {
    if (s_fast) { s_last_ns = SDL_GetTicksNS(); return 1; }
    for (;;) {
        uint64_t now = SDL_GetTicksNS();
        double dt = (now - s_last_ns) / 1e9;
        s_last_ns = now;
        /* Vsync lock: on a ~60 Hz display each swap takes ~one tick. Treat
         * those intervals as exactly one tick so logic runs 1:1 with
         * refreshes (no judder from the 60.0988 vs 60.000 Hz mismatch; the
         * audio ratio control absorbs the 0.16% difference). */
        if (s_vsync_on && fabs(dt - TICK_S) < 0.0015) dt = TICK_S;
        s_acc += dt;
        if (s_acc >= TICK_S) break;
        double wait = TICK_S - s_acc;
        SDL_DelayPrecise((uint64_t)(wait * 1e9));
    }
    int n = (int)(s_acc / TICK_S);
    if (n > 3) {
        /* Hitch (window drag, slow frame): run a short catch-up, drop the rest
         * rather than spiralling. */
        n = 3;
        s_acc = 0.0;
    } else {
        s_acc -= n * TICK_S;
    }
    return n;
}

void platform_get_stats(float *present_fps, float *tick_rate, float *frame_ms) {
    if (present_fps) *present_fps = s_fps;
    if (tick_rate)   *tick_rate = s_tps;
    if (frame_ms)    *frame_ms = s_frame_ms;
}

void platform_shutdown(void) {
    menu_overlay_shutdown();
    if (s_audio) {
        SDL_DestroyAudioStream(s_audio);
        s_audio = NULL;
    }
    if (s_have_gl) renderer_shutdown();
    if (s_gl)     SDL_GL_DestroyContext(s_gl);
    if (s_window) SDL_DestroyWindow(s_window);
    SDL_Quit();
    s_gl = NULL;
    s_window = NULL;
    s_have_gl = false;
}

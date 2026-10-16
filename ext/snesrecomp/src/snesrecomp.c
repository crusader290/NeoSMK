/*
 * snesrecomp core lifecycle — init, frame, shutdown.
 *
 * This is the glue that ties LakeSnes hardware, the recomp CPU state,
 * and the SDL3 / OpenGL ES platform layer together into one clean package.
 */

#include "snesrecomp/snesrecomp.h"
#include "snesrecomp/mp_session.h"
#include "snesrecomp/menu_overlay.h"
#include "m7view_internal.h"
#include "gfx/renderer.h"
#include "snes.h"
#include "ppu.h"
#include "input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The LakeSnes instance that provides all real SNES hardware */
static Snes *s_snes = NULL;

/* Pixel buffer for PPU output (512x478x4 = ~978 KB) */
/* ppu_putPixels writes/clears up to 480 rows (the overscan-centred layout +
 * trailing memset), so the buffer must hold 480 rows even though we only show
 * 478 — a 478-row buffer overflows by 4096 bytes into adjacent statics. */
static uint8_t s_pixel_buf[512 * 480 * 4];

/* Bumped whenever emulated state jumps (reset, load-state, netplay sync), so
 * layers keeping their own per-frame state can resynchronise. */
static uint32_t s_state_epoch = 0;

uint32_t snesrecomp_state_epoch(void) { return s_state_epoch; }

/* Camera reconstructed from the submitted frame's mode-7 lines. */
static M7View s_view;

/* Widescreen output (PPU extension columns, single-height rows). */
static uint8_t s_wide_buf[(256 + 2 * PPU_WIDE_MAX) * 8 * 239];

/* Audio sample buffer (enough for one frame at 32040 Hz / 60 fps) */
#define SAMPLES_PER_FRAME 534
static int16_t s_audio_buf[SAMPLES_PER_FRAME * 2]; /* stereo */

Snes *snesrecomp_get_snes(void) {
    return s_snes;
}

uint16_t snesrecomp_read_vram(uint16_t word_addr) {
    if (!s_snes || !s_snes->ppu) return 0;
    return s_snes->ppu->vram[word_addr & 0x7FFF];
}

uint16_t snesrecomp_read_cgram(uint8_t index) {
    if (!s_snes || !s_snes->ppu) return 0;
    return s_snes->ppu->cgram[index];
}

/*
 * Dump a compact binary snapshot of guest state (WRAM + VRAM) for lockstep
 * divergence analysis. Modeled on the snesrev zelda3 verification harness:
 * run the same ROM under two execution paths (pure interpreter as the
 * ground-truth reference vs. recompiled functions), snapshot every frame,
 * and diff to find the first WRAM/VRAM address that diverges — i.e. the
 * first incorrect recompiled function.
 *
 * Format (little-endian, matches the x86 host):
 *   char  magic[8]  = "SMKSNAP2"
 *   u32   wram_size = 0x20000
 *   u32   vram_size = 0x10000   (bytes; 0x8000 words)
 *   u32   cgram_size = 0x200    (bytes; 0x100 words)
 *   u8    wram[wram_size]
 *   u8    vram[vram_size]       (raw uint16 array, low byte first)
 *   u8    cgram[cgram_size]     (raw uint16 array, low byte first)
 *
 * CGRAM is included because Mode-7 BG1 garbling can be a palette fault as
 * easily as a char-data fault; comparing both against a Mesen2 reference dump
 * (tools/mesen/dump_snapshot.lua emits this exact format) isolates which.
 * The diff tool still reads the older "SMKSNAP1" (no cgram) for back-compat.
 */
void snesrecomp_dump_snapshot(const char *filepath) {
    if (!s_snes || !s_snes->ppu) return;
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "snesrecomp: failed to open snapshot '%s'\n", filepath);
        return;
    }
    const uint32_t wram_size = 0x20000;
    const uint32_t vram_size = 0x10000;  /* 0x8000 words * 2 bytes */
    const uint32_t cgram_size = 0x200;   /* 0x100 words * 2 bytes */
    fwrite("SMKSNAP2", 1, 8, f);
    fwrite(&wram_size, sizeof(uint32_t), 1, f);
    fwrite(&vram_size, sizeof(uint32_t), 1, f);
    fwrite(&cgram_size, sizeof(uint32_t), 1, f);
    fwrite(s_snes->ram, 1, wram_size, f);
    fwrite(s_snes->ppu->vram, 1, vram_size, f);
    fwrite(s_snes->ppu->cgram, 1, cgram_size, f);
    fclose(f);
}

bool snesrecomp_init(const char *window_title, int scale) {
    /* Create LakeSnes instance (allocates CPU, PPU, APU, DMA, Cart) */
    s_snes = snes_init();
    if (!s_snes) {
        fprintf(stderr, "snesrecomp: failed to create SNES instance\n");
        return false;
    }

    /* Pixel format: XBGR bytes (R in byte 3); the renderer swizzles. */
    snes_setPixelFormat(s_snes, pixelFormatRGBX);

    /* Point PPU pixel output at our buffer */
    snes_setPixels(s_snes, s_pixel_buf);

    /* Initialize recomp CPU state */
    recomp_cpu_reset();

    /* Note: func_table_init() is intentionally NOT called here.
     * RECOMP_PATCH static constructors run before main() and populate the
     * dispatch table; init() would wipe them. The table is zero-initialized
     * by C static-storage semantics, so no explicit reset is needed.
     * Call func_table_init() yourself only if you need to clear the table. */

    /* Initialize SDL3 platform (window, GL ES context, audio) */
    if (!platform_init(window_title, scale)) {
        fprintf(stderr, "snesrecomp: failed to initialize platform\n");
        snes_free(s_snes);
        s_snes = NULL;
        return false;
    }

    printf("snesrecomp: initialized (LakeSnes backend)\n");
    return true;
}

bool snesrecomp_load_rom(const char *path) {
    if (!s_snes) return false;

    /* Read ROM file into memory */
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "snesrecomp: failed to open ROM: %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return false;
    }

    size_t read = fread(data, 1, (size_t)size, f);
    fclose(f);

    if ((long)read != size) {
        fprintf(stderr, "snesrecomp: failed to read ROM (%zu of %ld bytes)\n", read, size);
        free(data);
        return false;
    }

    /* Let LakeSnes handle header detection, mapping type, region, etc. */
    bool ok = snes_loadRom(s_snes, data, (int)size);
    free(data);

    if (!ok) {
        fprintf(stderr, "snesrecomp: LakeSnes rejected the ROM\n");
        return false;
    }

    /* Re-point pixel output (reset clears it) */
    snes_setPixels(s_snes, s_pixel_buf);

    printf("snesrecomp: ROM loaded successfully\n");
    return true;
}

/* Extra PPU columns per side for each aspect setting: 256 SNES columns fill
 * 4:3, so N:M needs 224 * N/M * 7/8 columns (8:7 pixel aspect). */
static int widescreen_ext(int aspect) {
    switch (aspect) {
    case SNESRECOMP_ASPECT_16_10: return 26;   /* 308 cols ≈ 16:10 */
    case SNESRECOMP_ASPECT_16_9:  return 43;   /* 342 cols ≈ 16:9  */
    default:                      return 0;
    }
}

/* Applied before every frame: presentation-only PPU state, not saved in
 * save-states (and reset by them), so it is simply re-asserted. */
static void apply_widescreen(void) {
    ppu_setWidescreen(s_snes->ppu, widescreen_ext(menu_overlay_get_aspect()),
                      menu_overlay_get_wide_menus() ? ppu_wideModeAlways : ppu_wideModeMode7);
}

/* Hand the finished frame to the platform: the 256-wide 2x buffer, or the
 * widened single-height buffer when widescreen is on. */
/* HUD separation (full-screen layout). */
static SnesHudConfig s_hud_cfg;
static bool          s_hud_cfg_set = false;
static SnesHudFrame  s_hud;
static uint8_t       s_hud_buf[(256 + 2 * PPU_WIDE_MAX) * 4 * 239];

void snesrecomp_set_hud_config(const SnesHudConfig *cfg) {
    s_hud_cfg_set = cfg != NULL;
    if (cfg) s_hud_cfg = *cfg;
}

const SnesHudFrame *snesrecomp_hud(void) { return &s_hud; }

/* Before a frame: separate the HUD only while presenting full-screen (the
 * previous frame's decision; the first full-screen frame keeps it in place). */
static void apply_hud(void) {
    bool on = s_hud_cfg_set && s_hud_cfg.separate && s_view.layout == M7_LAYOUT_FULLSCREEN;
    /* lines above the band's first visible line are the game's blank strip */
    ppu_setHud(s_snes->ppu, on, s_view.top_skip + 1, on ? s_view.band[0].line1 : 0, s_hud_cfg.obj_mask,
               s_hud_cfg.bg_mask, s_hud_cfg.window_math);
}

/* Upward extension (full-screen layout): the PPU renders the lines that sit
 * above the band's first visible line, which the band shift uncovers. */
static uint8_t s_above_buf[(256 + 2 * PPU_WIDE_MAX) * 8 * PPU_ABOVE_MAX];

static void apply_above(void) {
    int n = 0;
    if (s_view.layout == M7_LAYOUT_FULLSCREEN) {
        n = s_view.line_offset + s_view.top_skip;
        if (n > PPU_ABOVE_MAX) n = PPU_ABOVE_MAX;
        if (n > RENDERER_ABOVE_MAX) n = RENDERER_ABOVE_MAX;
    }
    ppu_setAbove(s_snes->ppu, n, s_view.top_skip + 1);
}

static void collect_above(int cols) {
    int n = s_view.layout == M7_LAYOUT_FULLSCREEN ? ppu_putAbove(s_snes->ppu, s_above_buf) : 0;
    renderer_set_above(n > 0 ? s_above_buf : NULL, cols, n);
}

/* Mode-7 tile override and sprite layer, applied to the chase bands of the
 * previous frame (presentation only). */
static const uint16_t *s_m7_override = NULL;
static bool            s_spr_on = false;
static uint8_t         s_spr_exclude[16];
static uint8_t         s_spr_buf[(256 + 2 * PPU_WIDE_MAX) * 4 * 239];

void snesrecomp_set_m7_override(const uint16_t *cells) { s_m7_override = cells; }

void snesrecomp_set_sprite_layer(bool enabled, const uint8_t exclude[16]) {
    s_spr_on = enabled;
    if (exclude) memcpy(s_spr_exclude, exclude, sizeof(s_spr_exclude));
    else memset(s_spr_exclude, 0, sizeof(s_spr_exclude));
}

static void apply_track_layers(void) {
    int l0 = 0, l1 = 0;
    for (int i = 0; i < s_view.nbands; i++) {
        const M7Band *b = &s_view.band[i];
        if (!b->chase) continue;
        if (l0 == 0 || b->line0 < l0) l0 = b->line0;
        if (b->line1 > l1) l1 = b->line1;
    }
    ppu_setM7Override(s_snes->ppu, l1 > 0 ? s_m7_override : NULL, l1);
    /* native layout: karts poke out a little below the band */
    int max = s_view.layout == M7_LAYOUT_FULLSCREEN ? l1 : l1 + 8;
    ppu_setObjSep(s_snes->ppu, s_spr_on && l1 > 0, l0, max, s_spr_exclude);
}

static void collect_sprites(int cols) {
    if (!s_snes->ppu->objSepEnabled) {
        renderer_set_sprites(NULL, 0, 0, 0, NULL, 0);
        return;
    }
    int lines = ppu_putObjSep(s_snes->ppu, s_spr_buf);
    RendererSpriteBand bands[4];
    int n = 0;
    for (int i = 0; i < s_view.nbands && n < 4; i++) {
        const M7Band *b = &s_view.band[i];
        if (!b->chase) continue;
        bands[n].line0 = b->line0;
        bands[n].line1 = b->line1;
        bands[n].horizon = b->horizon;
        bands[n].focal_height = b->focal * b->height;
        n++;
    }
    renderer_set_sprites(s_spr_buf, cols, lines,
                         s_view.layout == M7_LAYOUT_FULLSCREEN ? s_view.line_offset : 0, bands, n);
}

/* After a frame: copy the HUD layer and box each class. */
static void collect_hud(int cols) {
    s_hud.valid = false;
    if (!s_snes->ppu->hudEnabled || s_view.layout != M7_LAYOUT_FULLSCREEN) {
        renderer_set_hud(NULL, 0, 0);
        return;
    }
    int lines = ppu_putHud(s_snes->ppu, s_hud_buf);
    s_hud.math_flags = 0;
    for (int c = 0; c < 4; c++) {
        s_hud.box[c].present = false;
        s_hud.box[c].x0 = s_hud.box[c].y0 = 1 << 20;
        s_hud.box[c].x1 = s_hud.box[c].y1 = -1;
    }
    for (int y = 0; y < lines; y++) {
        const uint8_t *row = s_hud_buf + (size_t)y * cols * 4;
        for (int x = 0; x < cols; x++) {
            int c = row[x * 4 + 3] & 0x0F;
            if (c <= 0 || c > 3) continue;
            if (c == SNES_HUD_MATH) {
                s_hud.math_flags = ((row[x * 4 + 3] >> 4) & 1) | (((row[x * 4 + 3] >> 5) & 1) << 1);
                memcpy(s_hud.math_rgb, &row[x * 4], 3);
            }
            if (row[x * 4 + 3] & 0x40) {      /* math under another HUD element */
                SnesHudFrame *h = &s_hud;
                if (x < h->box[SNES_HUD_MATH].x0) h->box[SNES_HUD_MATH].x0 = x;
                if (x > h->box[SNES_HUD_MATH].x1) h->box[SNES_HUD_MATH].x1 = x;
                if (y < h->box[SNES_HUD_MATH].y0) h->box[SNES_HUD_MATH].y0 = y;
                if (y > h->box[SNES_HUD_MATH].y1) h->box[SNES_HUD_MATH].y1 = y;
                h->box[SNES_HUD_MATH].present = true;
            }
            if (x < s_hud.box[c].x0) s_hud.box[c].x0 = x;
            if (x > s_hud.box[c].x1) s_hud.box[c].x1 = x;
            if (y < s_hud.box[c].y0) s_hud.box[c].y0 = y;
            if (y > s_hud.box[c].y1) s_hud.box[c].y1 = y;
            s_hud.box[c].present = true;
        }
    }
    s_hud.valid = true;
    s_hud.cols = cols;
    s_hud.lines = lines;
    renderer_set_hud(s_hud_buf, cols, lines);
}

/* Full-screen 1-player layout: decided per frame from the solved bands. */
static RendererM7Frame s_m7frame;

/* XBGR pixel (as 0..1 RGB) of column x, scanline `line`, in the submitted frame. */
static void frame_pixel(const uint8_t *pix, int width, int row0, int rows_per_line,
                        int x, int line, float rgb[3]) {
    const uint8_t *p = pix + ((size_t)(row0 + (line - 1) * rows_per_line) * width + (size_t)x * 2 + 1) * 4;
    rgb[0] = p[3] / 255.0f; rgb[1] = p[2] / 255.0f; rgb[2] = p[1] / 255.0f;
}

static void decide_layout(const uint8_t *pix, int width, int row0, int rows_per_line) {
    M7View *v = &s_view;
    v->layout = M7_LAYOUT_NATIVE;
    v->line_offset = 0;
    v->top_skip = 0;
    if (!menu_overlay_get_fullscreen_m7() || v->nbands == 0) return;

    /* one perspective band at the top (1-player), nothing else in perspective */
    const M7Band *top = &v->band[0];
    if (!top->chase || top->line0 > 48 || top->rms_line > 2.5f) return;
    for (int i = 1; i < v->nbands; i++) if (v->band[i].chase) return;   /* 2-player */
    if (top->line1 > v->lines * 2 / 3) return;   /* already fills the screen */

    /* drop blank (black) lines the game draws above the band (checked over
     * the original 256 columns: widescreen extensions aren't blank) */
    int cols = width / 2;
    int ext = (cols - 256) / 2;
    int skip = 0;
    for (int line = 1; line <= 8; line++) {
        bool black = true;
        for (int x = ext; x < ext + 256 && black; x += 4) {
            float c[3];
            frame_pixel(pix, width, row0, rows_per_line, x, line, c);
            if (c[0] + c[1] + c[2] > 0.03f) black = false;
        }
        if (!black) break;
        skip++;
    }

    /* horizon at ~30% of the screen, at least 40 lines of floor under the band */
    int shift = (int)(0.30f * v->lines - top->horizon + 0.5f);
    if (shift > v->lines - 40 - top->line1) shift = v->lines - 40 - top->line1;
    if (shift < 0) shift = 0;

    /* sky: most common colour of the band's first visible line */
    float best[3] = { 0, 0, 0 };
    int best_n = 0;
    for (int x = 0; x < cols; x += 2) {
        float c[3], d[3];
        frame_pixel(pix, width, row0, rows_per_line, x, skip + 1, c);
        int n = 0;
        for (int y = 0; y < cols; y += 2) {
            frame_pixel(pix, width, row0, rows_per_line, y, skip + 1, d);
            if (d[0] == c[0] && d[1] == c[1] && d[2] == c[2]) n++;
        }
        if (n > best_n) { best_n = n; best[0] = c[0]; best[1] = c[1]; best[2] = c[2]; }
    }

    v->layout = M7_LAYOUT_FULLSCREEN;
    v->line_offset = shift;
    v->top_skip = skip;
    v->sky[0] = best[0]; v->sky[1] = best[1]; v->sky[2] = best[2];
    for (int i = 0; i < v->nbands; i++) {      /* only the race band is presented */
        v->band[i].draw_line0 = 1;
        v->band[i].draw_line1 = 0;
    }
    v->band[0].draw_line0 = top->line0 + shift;
    v->band[0].draw_line1 = v->lines;

    /* floor data for the GPU ground */
    RendererM7Frame *m = &s_m7frame;
    Ppu *ppu = s_snes->ppu;
    m->cols = v->cols;
    m->lines = v->lines;
    m->line_offset = shift;
    m->band_first = skip + 1;
    m->band_last = top->line1;
    m->sky[0] = best[0]; m->sky[1] = best[1]; m->sky[2] = best[2];
    m->cam[0] = top->cam[0];     m->cam[1] = top->cam[1];
    m->fwd[0] = top->fwd[0];     m->fwd[1] = top->fwd[1];
    m->right[0] = top->right[0]; m->right[1] = top->right[1];
    m->focal = top->focal;
    m->height = top->height;
    m->horizon = top->horizon;
    m->outside = !ppu->m7largeField ? 0 : (ppu->m7charFill ? 2 : 1);
    for (int i = 0; i < 128 * 128; i++)
        m->tilemap[i] = (uint8_t)(s_m7_override && s_m7_override[i] != 0xFFFF ? s_m7_override[i]
                                                                               : ppu->vram[i] & 0xFF);
    for (int t = 0; t < 256; t++)
        for (int py = 0; py < 8; py++)
            for (int px = 0; px < 8; px++)
                m->chars[((t >> 4) * 8 + py) * 128 + (t & 15) * 8 + px] =
                    (uint8_t)(ppu->vram[(t * 64 + py * 8 + px) & 0x7FFF] >> 8);
    /* brightness the band was drawn with (at submit time the game may be in
     * forced blank for its vblank DMA) */
    int bright = ppu_getM7Lines(ppu)[(top->line0 + top->line1) / 2 - 1].brightness;
    for (int i = 0; i < 256; i++) {
        uint16_t c = ppu->cgram[i];
        int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        m->palette[i * 4 + 0] = (uint8_t)(((r << 3) | (r >> 2)) * bright / 15);
        m->palette[i * 4 + 1] = (uint8_t)(((g << 3) | (g >> 2)) * bright / 15);
        m->palette[i * 4 + 2] = (uint8_t)(((b << 3) | (b >> 2)) * bright / 15);
        m->palette[i * 4 + 3] = 255;
    }
}

static void submit_video(void) {
    m7view_solve(&s_view, ppu_getM7Lines(s_snes->ppu), 256 + 2 * s_snes->ppu->wideExt,
                 s_snes->ppu->frameOverscan ? 239 : 224);
    const uint8_t *pix;
    int width, row0, rows, rows_per_line;
    if (s_snes->ppu->wideExt > 0) {
        rows = ppu_putPixelsWide(s_snes->ppu, s_wide_buf);
        pix = s_wide_buf;
        width = (256 + 2 * s_snes->ppu->wideExt) * 2;
        row0 = 0;
        rows_per_line = 1;
    } else {
        int rx, ry, rw;
        snesrecomp_active_video_rect(&rx, &ry, &rw, &rows);
        pix = s_pixel_buf;
        width = SNES_RENDER_WIDTH;
        row0 = ry;
        rows_per_line = 2;
    }
    decide_layout(pix, width, row0, rows_per_line);
    static int last_layout = -1;
    if (s_view.layout != last_layout) {
        printf("snesrecomp: layout -> %s\n", s_view.layout == M7_LAYOUT_FULLSCREEN ? "full-screen" : "native");
        last_layout = s_view.layout;
    }
    renderer_set_m7_frame(s_view.layout == M7_LAYOUT_FULLSCREEN ? &s_m7frame : NULL);
    collect_hud(width / 2);
    collect_above(width / 2);
    collect_sprites(width / 2);
    platform_submit_frame(pix, width, row0, rows);
}

const M7View *snesrecomp_m7_view(void) {
    return &s_view;
}

bool snesrecomp_poll(void) {
    return platform_poll_events();
}

int snesrecomp_ticks_due(void) {
    return platform_ticks_due();
}

void snesrecomp_present(void) {
    platform_present();
}

bool snesrecomp_begin_frame(void) {
    /* Update input and feed into LakeSnes */
    recomp_input_update();

    /* Perform the auto-joypad read that real hardware does each vblank.
     * snesrecomp bypasses the LakeSnes per-cycle CPU loop (where this would
     * normally fire), so the auto-read registers $4218-$421F would otherwise
     * never be populated and recompiled code would see no input. */
    if (s_snes) snes_doAutoJoypad(s_snes);

    return true;
}

void snesrecomp_end_frame(void) {
    if (!s_snes) return;

    /* Start-of-frame PPU bookkeeping (toggles evenFrame, resets mosaic, etc.) */
    apply_widescreen();
    apply_hud();
    apply_above();
    apply_track_layers();
    ppu_handleFrameStart(s_snes->ppu);
    s_snes->input2->scopeLatched = false; /* reset Super Scope latch for new frame */

    /* Initialize HDMA for this frame. snesrecomp renders scanlines manually and
     * never steps the per-cycle CPU loop where LakeSnes normally runs HDMA, so
     * we drive it here (untimed). Without this, per-scanline register effects
     * (Mode-7 BGMODE switching, matrix updates, window/color raster effects)
     * never apply — e.g. SMK's track plane never enters Mode 7. */
    if (s_snes->dma) dma_hdmaInitFrame(s_snes->dma);

    /* Diagnostic: when SMK_HDMA_DEBUG is set, on the first Mode-7 frame report
     * which registers the active HDMA channels target and CGRAM before/after
     * the scanline loop. Used to confirm whether per-scanline HDMA clobbers the
     * Mode-7 palette (CGRAM). One-shot. */
    static int s_hdma_dbg_done = 0;
    int hdma_dbg = getenv("SMK_HDMA_DEBUG") && !s_hdma_dbg_done &&
                   s_snes->ppu->mode == 7;
    if (hdma_dbg) {
        s_hdma_dbg_done = 1;
        fprintf(stderr, "[HDMA_DEBUG] mode=7 frame, active HDMA channels:\n");
        for (int c = 0; c < 8; c++) {
            if (s_snes->dma->channel[c].hdmaActive)
                fprintf(stderr, "  ch%d -> $21%02X (mode=%d aBank=%02X aAdr=%04X)\n",
                        c, s_snes->dma->channel[c].bAdr,
                        s_snes->dma->channel[c].mode,
                        s_snes->dma->channel[c].aBank,
                        s_snes->dma->channel[c].aAdr);
        }
        fprintf(stderr, "  CGRAM[0..7] BEFORE scanline HDMA: ");
        for (int i = 0; i < 8; i++) fprintf(stderr, "%04X ", s_snes->ppu->cgram[i]);
        fprintf(stderr, "\n");
    }

    /* Run all PPU scanlines for the frame */
    for (int line = 1; line <= 224; line++) {
        /* Apply this scanline's HDMA register writes before rendering it
         * (real hardware does HDMA during the preceding HBlank). */
        if (s_snes->dma) dma_hdmaRunLine(s_snes->dma);
        ppu_runLine(s_snes->ppu, line);

        /*
         * Super Scope beam detection: when the PPU renders the scanline
         * the scope is aimed at, trigger a counter latch. This simulates
         * the photodiode firing when the CRT beam passes the target.
         * snes_runCycle() isn't used in recomp, so we do it here.
         */
        if (s_snes->input2->type == inputDeviceSuperScope &&
            s_snes->ppuLatch && !s_snes->input2->scopeOffscreen &&
            !s_snes->input2->scopeLatched) {
            uint16_t targetV = s_snes->input2->scopeY + 1;
            if ((uint16_t)line == targetV) {
                s_snes->ppu->hCount = s_snes->input2->scopeX + 22;
                s_snes->ppu->vCount = targetV;
                s_snes->ppu->countersLatched = true;
                s_snes->input2->scopeLatched = true;
            }
        }
    }
    ppu_handleVblank(s_snes->ppu);

    if (hdma_dbg) {
        fprintf(stderr, "  CGRAM[0..7] AFTER  scanline HDMA: ");
        for (int i = 0; i < 8; i++) fprintf(stderr, "%04X ", s_snes->ppu->cgram[i]);
        fprintf(stderr, "\n");
    }

    /* Copy rendered pixels from PPU internal buffer to output buffer */
    snes_setPixels(s_snes, s_pixel_buf);

    /* Catch up APU audio and extract samples */
    int apu_cycles = (int)(s_snes->palTiming ? 534 : 534);
    apu_runCycles(s_snes->apu, apu_cycles * 32);
    snes_setSamples(s_snes, s_audio_buf, SAMPLES_PER_FRAME);

    /* Hand the frame to the renderer (drawn at the next present) */
    submit_video();

    /* Queue audio */
    platform_queue_audio(s_audio_buf, SAMPLES_PER_FRAME);
}

/* ---- Real-frame path ----
 * Runs the genuine ROM via LakeSnes's full cycle-accurate frame (CPU + NMI +
 * per-scanline HDMA + PPU, real timing) — identical to how tools/lakesnes_ref
 * renders. Use this to play gameplay the recomp's per-frame shells can't yet
 * drive (the Mode-7 race's multi-frame init / fade-in never completes under the
 * shells, which drop the vblank waits the init loops on). No recomp boot chain
 * or NMI/main shells are involved; snes->cpu boots from the reset vector on the
 * first call. Mirrors begin_frame/end_frame so main.c can swap modes. */
/* Netplay per-frame orchestration (generic; menu starts/stops the session).
 * On the first connected frame the host sends its save-state and the client
 * loads it (so both share an identical starting point); thereafter each frame
 * exchanges the local pad for the remote's and feeds the synchronized pair to
 * controllers 1 (host) and 2 (client). Lockstep keeps the two deterministic
 * emulations in sync. */
static void snesrecomp_mp_update(void) {
    static bool synced = false;
    if (mp_get_state() != MP_CONNECTED) { synced = false; return; }
    if (!s_snes) return;

    if (!synced) {
        s_state_epoch++;   /* both peers restart from the shared state */
        if (mp_is_host()) {
            uint8_t *buf = (uint8_t *)malloc(1 << 20);
            if (buf) { int sz = snes_saveState(s_snes, buf); mp_send_blob(buf, sz); free(buf); }
        } else {
            uint8_t *buf = NULL; int sz = 0;
            if (mp_recv_blob(&buf, &sz)) { snes_loadState(s_snes, buf, sz); free(buf); }
        }
        synced = true;
    }

    /* The local player drives controller 1's bindings; route it to the right
     * port and take the remote's for the other. */
    uint16_t local = s_snes->input1->currentState;
    uint16_t p1 = 0, p2 = 0;
    if (mp_exchange(local, &p1, &p2)) {
        s_snes->input1->currentState = p1;
        s_snes->input2->currentState = p2;
    }
}

bool snesrecomp_realframe_begin(void) {
    recomp_input_update();
    snesrecomp_mp_update();   /* lockstep input exchange when a session is active */
    return true;
}

void snesrecomp_realframe_end(void) {
    if (!s_snes) return;
    apply_widescreen();
    apply_hud();
    apply_above();
    apply_track_layers();
    snes_runFrame(s_snes);
    snes_setPixels(s_snes, s_pixel_buf);
    snes_setSamples(s_snes, s_audio_buf, SAMPLES_PER_FRAME);
    submit_video();
    platform_queue_audio(s_audio_buf, SAMPLES_PER_FRAME);
}

/* ---- Emulator controls (menu File -> New / Save / Load) ---- */

/* Hard reset — re-fills WRAM (0x55) and restarts the ROM from the reset vector.
 * In real-frame mode the next frame reboots the game. */
void snesrecomp_reset(void) {
    if (s_snes) snes_reset(s_snes, true);
    s_state_epoch++;
}

/* Save the full machine state (LakeSnes save-state: CPU/PPU/APU/RAM/regs) to a
 * file. Captures the genuine ROM execution that real-frame mode runs. */
bool snesrecomp_save_state(const char *path) {
    if (!s_snes || !path) return false;
    uint8_t *buf = (uint8_t *)malloc(1 << 20);   /* 1 MB — ample for SNES state */
    if (!buf) return false;
    int size = snes_saveState(s_snes, buf);
    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (f) { ok = (int)fwrite(buf, 1, (size_t)size, f) == size; fclose(f); }
    free(buf);
    if (ok) printf("smk: saved state -> %s (%d bytes)\n", path, size);
    else    fprintf(stderr, "smk: save state failed (%s)\n", path);
    return ok;
}

/* Load a previously saved machine state. Validated by LakeSnes (id/version/cart
 * must match); ignored if no/invalid file. */
bool snesrecomp_load_state(const char *path) {
    if (!s_snes || !path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "smk: no save state at %s\n", path); return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    bool ok = false;
    if (sz > 0) {
        uint8_t *buf = (uint8_t *)malloc((size_t)sz);
        if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz)
            ok = snes_loadState(s_snes, buf, (int)sz);
        free(buf);
    }
    fclose(f);
    if (ok) s_state_epoch++;
    if (ok) printf("smk: loaded state <- %s\n", path);
    else    fprintf(stderr, "smk: load state failed (%s) — wrong ROM or corrupt?\n", path);
    return ok;
}

/* FNV-1a hash of WRAM — for netplay desync detection / lockstep verification. */
uint32_t snesrecomp_wram_checksum(void) {
    if (!s_snes) return 0;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 0x20000; i++) { h ^= s_snes->ram[i]; h *= 16777619u; }
    return h;
}

void snesrecomp_active_video_rect(int *x, int *y, int *w, int *h) {
    /* The PPU framebuffer (512x478) centers the active picture with black
     * overscan bands: ppu_putPixels places non-overscan content (224 lines,
     * doubled) at rows 16..463, leaving 16px black at top and 14px at bottom.
     * Return the active region so the platform can crop those bands. */
    *x = 0;
    *w = 512;
    if (s_snes && s_snes->ppu && s_snes->ppu->frameOverscan) {
        *y = 4;  *h = 470;   /* overscan frame — show ~full height */
    } else {
        *y = 16; *h = 448;   /* 224 active NTSC lines, 2x */
    }
}

void snesrecomp_dump_m7(const char *filepath) {
    if (!s_snes || !s_snes->ppu) return;
    FILE *f = fopen(filepath, "w");
    if (!f) return;
    const PpuM7Line *l = ppu_getM7Lines(s_snes->ppu);
    fprintf(f, "# line xflip startX startY a c\n");
    for (int i = 0; i < 240; i++)
        if (l[i].valid)
            fprintf(f, "%d %d %d %d %d %d\n", i + 1, l[i].xFlip, l[i].startX, l[i].startY, l[i].a, l[i].c);
    fclose(f);
}

void snesrecomp_dump_oam(const char *filepath) {
    if (!s_snes || !s_snes->ppu) return;
    FILE *f = fopen(filepath, "w");
    if (!f) return;
    Ppu *ppu = s_snes->ppu;
    fprintf(f, "# objSize=%d tileAdr1=%04X tileAdr2=%04X\n# idx x y tile nt pal pri hf vf large\n",
            ppu->objSize, ppu->objTileAdr1, ppu->objTileAdr2);
    for (int i = 0; i < 128; i++) {
        uint16_t w0 = ppu->oam[i * 2], w1 = ppu->oam[i * 2 + 1];
        int hi = (ppu->highOam[i >> 2] >> ((i & 3) * 2)) & 3;
        int x = (w0 & 0xFF) | ((hi & 1) << 8);
        if (x > 255) x -= 512;
        fprintf(f, "%d %d %d %d %d %d %d %d %d %d\n", i, x, w0 >> 8, w1 & 0xFF, (w1 >> 8) & 1,
                (w1 >> 9) & 7, (w1 >> 12) & 3, (w1 >> 14) & 1, (w1 >> 15) & 1, (hi >> 1) & 1);
    }
    fclose(f);
}

void snesrecomp_trigger_vblank(void) {
    if (!s_snes) return;
    s_snes->inVblank = true;
    s_snes->inNmi = true;
}

const uint8_t *snesrecomp_get_framebuffer(void) {
    return s_pixel_buf;
}

void snesrecomp_dump_ppu(const char *filepath) {
    FILE *dbg = fopen(filepath, "w");
    if (!dbg) return;
    fprintf(dbg, "s_snes=%p\n", (void*)s_snes);
    if (!s_snes) { fclose(dbg); return; }
    Ppu *ppu = s_snes->ppu;
    fprintf(dbg, "ppu=%p\n", (void*)ppu);
    if (!ppu) { fclose(dbg); return; }
    fprintf(dbg, "forcedBlank=%d brightness=%d mode=%d evenFrame=%d\n",
           ppu->forcedBlank, ppu->brightness, ppu->mode, ppu->evenFrame);
    fprintf(dbg, "M7 matrix a=%d b=%d c=%d d=%d x=%d y=%d h=%d v=%d  startX=%d startY=%d sel=%d extBg=%d\n",
           ppu->m7matrix[0], ppu->m7matrix[1], ppu->m7matrix[2], ppu->m7matrix[3],
           ppu->m7matrix[4], ppu->m7matrix[5], ppu->m7matrix[6], ppu->m7matrix[7],
           ppu->m7startX, ppu->m7startY, ppu->m7largeField, ppu->m7extBg);
    fprintf(dbg, "layer mainScreenEnabled: %d %d %d %d %d\n",
           ppu->layer[0].mainScreenEnabled, ppu->layer[1].mainScreenEnabled,
           ppu->layer[2].mainScreenEnabled, ppu->layer[3].mainScreenEnabled,
           ppu->layer[4].mainScreenEnabled);
    fprintf(dbg, "BG tilemapAdr: %04X %04X %04X %04X\n",
           ppu->bgLayer[0].tilemapAdr, ppu->bgLayer[1].tilemapAdr,
           ppu->bgLayer[2].tilemapAdr, ppu->bgLayer[3].tilemapAdr);
    fprintf(dbg, "BG tileAdr: %04X %04X %04X %04X\n",
           ppu->bgLayer[0].tileAdr, ppu->bgLayer[1].tileAdr,
           ppu->bgLayer[2].tileAdr, ppu->bgLayer[3].tileAdr);
    fprintf(dbg, "CGRAM[0-7]: %04X %04X %04X %04X %04X %04X %04X %04X\n",
           ppu->cgram[0], ppu->cgram[1], ppu->cgram[2], ppu->cgram[3],
           ppu->cgram[4], ppu->cgram[5], ppu->cgram[6], ppu->cgram[7]);
    fprintf(dbg, "VRAM[0-7]: %04X %04X %04X %04X %04X %04X %04X %04X\n",
           ppu->vram[0], ppu->vram[1], ppu->vram[2], ppu->vram[3],
           ppu->vram[4], ppu->vram[5], ppu->vram[6], ppu->vram[7]);
    /* Dump VRAM at key tilemap addresses */
    fprintf(dbg, "VRAM[$1000-$1007]: %04X %04X %04X %04X %04X %04X %04X %04X\n",
           ppu->vram[0x1000], ppu->vram[0x1001], ppu->vram[0x1002], ppu->vram[0x1003],
           ppu->vram[0x1004], ppu->vram[0x1005], ppu->vram[0x1006], ppu->vram[0x1007]);
    fprintf(dbg, "VRAM[$1400-$1407]: %04X %04X %04X %04X %04X %04X %04X %04X\n",
           ppu->vram[0x1400], ppu->vram[0x1401], ppu->vram[0x1402], ppu->vram[0x1403],
           ppu->vram[0x1400+4], ppu->vram[0x1400+5], ppu->vram[0x1400+6], ppu->vram[0x1400+7]);
    fprintf(dbg, "VRAM[$C000-$C007]: %04X %04X %04X %04X %04X %04X %04X %04X\n",
           ppu->vram[0xC000&0x7FFF], ppu->vram[(0xC001)&0x7FFF], ppu->vram[(0xC002)&0x7FFF], ppu->vram[(0xC003)&0x7FFF],
           ppu->vram[(0xC004)&0x7FFF], ppu->vram[(0xC005)&0x7FFF], ppu->vram[(0xC006)&0x7FFF], ppu->vram[(0xC007)&0x7FFF]);
    /* VRAM size check */
    int vram_nonzero = 0;
    for (int i = 0; i < 0x8000; i++) {
        if (ppu->vram[i] != 0) vram_nonzero++;
    }
    fprintf(dbg, "VRAM total nonzero words: %d / 32768\n", vram_nonzero);
    /* VRAM heatmap: count nonzero words in each 1K-word block */
    fprintf(dbg, "VRAM heatmap (1K-word blocks, word addr):\n");
    for (int blk = 0; blk < 32; blk++) {
        int cnt = 0;
        for (int j = 0; j < 0x400; j++) {
            if (ppu->vram[blk * 0x400 + j] != 0) cnt++;
        }
        if (cnt > 0) {
            fprintf(dbg, "  $%04X-$%04X: %4d / 1024 nonzero\n",
                   blk * 0x400, blk * 0x400 + 0x3FF, cnt);
        }
    }
    /* OBJ settings */
    fprintf(dbg, "objSize=%d objTileAdr1=$%04X objTileAdr2=$%04X\n",
           ppu->objSize, ppu->objTileAdr1, ppu->objTileAdr2);
    /* OAM entries (first 16 sprites) */
    fprintf(dbg, "OAM (first 16 sprites):\n");
    for (int i = 0; i < 16; i++) {
        int idx = i * 2;
        uint16_t w0 = ppu->oam[idx];
        uint16_t w1 = ppu->oam[idx + 1];
        uint8_t x = w0 & 0xFF;
        uint8_t y = w0 >> 8;
        uint8_t tile = w1 & 0xFF;
        int nt = (w1 >> 8) & 1;
        int pal = (w1 >> 9) & 7;
        int pri = (w1 >> 12) & 3;
        int hf = (w1 >> 14) & 1;
        int vf = (w1 >> 15) & 1;
        /* High OAM: size + X bit 9 */
        int hi_byte = ppu->highOam[idx >> 3];
        int x9 = (hi_byte >> (idx & 7)) & 1;
        int sz = (hi_byte >> ((idx & 7) + 1)) & 1;
        fprintf(dbg, "  [%2d] x=%3d%s y=%3d tile=$%02X nt=%d pal=%d pri=%d hf=%d vf=%d sz=%d\n",
               i, x | (x9 << 8), x9 ? "+" : " ", y, tile, nt, pal, pri, hf, vf, sz);
    }
    /* CGRAM OBJ palette (entries 128-143 = OBJ palette 0) */
    for (int p = 0; p < 8; p++) {
        fprintf(dbg, "CGRAM OBJ pal %d (%d-%d): ", p, 128+p*16, 128+p*16+15);
        for (int i = 128+p*16; i < 128+p*16+16; i++) {
            fprintf(dbg, "%04X ", ppu->cgram[i]);
        }
        fprintf(dbg, "\n");
    }
    /* VRAM at sprite tile base ($4000) - first 16 words */
    fprintf(dbg, "VRAM[$4000-$400F]: ");
    for (int i = 0; i < 16; i++) {
        fprintf(dbg, "%04X ", ppu->vram[0x4000 + i]);
    }
    fprintf(dbg, "\n");
    /* VRAM at DMA'd sprite tile locations (per slot) */
    {
        static const uint16_t slot_vram[] = {0x5800,0x5840,0x5880,0x58C0,0x5C00,0x5C40,0x5C80,0x5CC0};
        for (int s = 0; s < 8; s++) {
            uint16_t base = slot_vram[s];
            int nz = 0;
            for (int w = 0; w < 16; w++) {
                if (ppu->vram[base + w] != 0) nz++;
            }
            fprintf(dbg, "VRAM[$%04X] slot%d: %d/16 nonzero", base, s, nz);
            if (nz > 0) {
                fprintf(dbg, " first4: %04X %04X %04X %04X",
                       ppu->vram[base], ppu->vram[base+1], ppu->vram[base+2], ppu->vram[base+3]);
            }
            fprintf(dbg, "\n");
        }
    }
    /* WRAM OAM staging buffer at $0200 (first 16 bytes) */
    fprintf(dbg, "WRAM[$0200-$020F]: ");
    for (int i = 0; i < 16; i++) {
        fprintf(dbg, "%02X ", s_snes->ram[0x0200 + i]);
    }
    fprintf(dbg, "\n");
    /* WRAM OAM "PUSH START" region at $03D0 (48 bytes = 12 sprites) */
    fprintf(dbg, "WRAM[$03D0-$03FF]: ");
    for (int i = 0; i < 48; i++) {
        fprintf(dbg, "%02X ", s_snes->ram[0x03D0 + i]);
    }
    fprintf(dbg, "\n");
    /* WRAM OAM high table at $0400 (32 bytes) */
    fprintf(dbg, "WRAM[$0400-$041F]: ");
    for (int i = 0; i < 32; i++) {
        fprintf(dbg, "%02X ", s_snes->ram[0x0400 + i]);
    }
    fprintf(dbg, "\n");
    /* PPU OAM entries 116-127 */
    fprintf(dbg, "OAM (sprites 116-127):\n");
    for (int i = 116; i < 128; i++) {
        int idx = i * 2;
        uint16_t w0 = ppu->oam[idx];
        uint16_t w1 = ppu->oam[idx + 1];
        uint8_t x = w0 & 0xFF;
        uint8_t y = w0 >> 8;
        uint8_t tile = w1 & 0xFF;
        int nt = (w1 >> 8) & 1;
        int pal = (w1 >> 9) & 7;
        int pri = (w1 >> 12) & 3;
        int hf = (w1 >> 14) & 1;
        int vf = (w1 >> 15) & 1;
        int hi_byte = ppu->highOam[idx >> 3];
        int x9 = (hi_byte >> (idx & 7)) & 1;
        int sz = (hi_byte >> ((idx & 7) + 1)) & 1;
        fprintf(dbg, "  [%3d] x=%3d%s y=%3d tile=$%02X nt=%d pal=%d pri=%d hf=%d vf=%d sz=%d\n",
               i, x | (x9 << 8), x9 ? "+" : " ", y, tile, nt, pal, pri, hf, vf, sz);
    }
    /* pixelBuffer check */
    int pb_nonzero = 0;
    for (int i = 0; i < 512 * 2 * 239 * 4; i += 101) {
        if (ppu->pixelBuffer[i] != 0) pb_nonzero++;
    }
    fprintf(dbg, "pixelBuffer nonzero: %d\n", pb_nonzero);
    int sb_nonzero = 0;
    for (int i = 0; i < 512 * 478 * 4; i += 101) {
        if (s_pixel_buf[i] != 0) sb_nonzero++;
    }
    fprintf(dbg, "s_pixel_buf nonzero: %d\n", sb_nonzero);
    fclose(dbg);

    /* Save BMP screenshot alongside the PPU dump */
    {
        /* Build BMP path from filepath: replace extension with .bmp */
        char bmp_path[512];
        strncpy(bmp_path, filepath, sizeof(bmp_path) - 1);
        bmp_path[sizeof(bmp_path) - 1] = '\0';
        char *dot = strrchr(bmp_path, '.');
        if (dot) strcpy(dot, ".bmp");
        else strcat(bmp_path, ".bmp");

        /* SNES renders 256x224 (or 512x224 in hi-res, but PPU buffer is 512 wide).
         * s_pixel_buf is 512x478x4 (RGBX). We save the left 256 pixels of 224 lines. */
        int w = 256, h = 224;
        FILE *bmp = fopen(bmp_path, "wb");
        if (bmp) {
            /* BMP header (54 bytes) */
            int row_stride = w * 3;
            int pad = (4 - (row_stride % 4)) % 4;
            int data_size = (row_stride + pad) * h;
            int file_size = 54 + data_size;
            uint8_t hdr[54];
            memset(hdr, 0, 54);
            hdr[0] = 'B'; hdr[1] = 'M';
            hdr[2] = file_size & 0xFF; hdr[3] = (file_size >> 8) & 0xFF;
            hdr[4] = (file_size >> 16) & 0xFF; hdr[5] = (file_size >> 24) & 0xFF;
            hdr[10] = 54; /* pixel data offset */
            hdr[14] = 40; /* DIB header size */
            hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
            hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
            hdr[26] = 1; /* planes */
            hdr[28] = 24; /* bits per pixel */
            hdr[34] = data_size & 0xFF; hdr[35] = (data_size >> 8) & 0xFF;
            hdr[36] = (data_size >> 16) & 0xFF; hdr[37] = (data_size >> 24) & 0xFF;
            fwrite(hdr, 1, 54, bmp);

            /* BMP stores bottom-up, BGR. */
            uint8_t pad_bytes[3] = {0, 0, 0};
            for (int y = h - 1; y >= 0; y--) {
                for (int x = 0; x < w; x++) {
                    int idx = y * 2048 + x * 8;
                    uint8_t bgr[3];
                    bgr[0] = s_pixel_buf[idx + 1]; /* B */
                    bgr[1] = s_pixel_buf[idx + 2]; /* G */
                    bgr[2] = s_pixel_buf[idx + 3]; /* R */
                    fwrite(bgr, 1, 3, bmp);
                }
                if (pad > 0) fwrite(pad_bytes, 1, pad, bmp);
            }
            fclose(bmp);
        }
    }
}

void snesrecomp_shutdown(void) {
    mp_shutdown();
    platform_shutdown();

    if (s_snes) {
        snes_free(s_snes);
        s_snes = NULL;
    }

    printf("snesrecomp: shutdown complete\n");
}

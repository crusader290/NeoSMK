/*
 * renderer.h — OpenGL ES 3.0 frame compositor.
 *
 * Frame pipeline (all genuine GPU triangles, no CPU scaling):
 *
 *   SNES PPU framebuffer (XBGR, CPU; 512 wide, up to 768 in widescreen)
 *        │  glTexSubImage2D — latest frame, once per present
 *        ▼
 *   [1] internal target  640x480 (4:3) / 770x480 (16:10) / 856x480 (16:9)
 *       RGBA8 + 24-bit depth — 2.5 px per SNES column, 480 lines
 *        · SNES layer drawn as a full-screen triangle (bilinear over the
 *          2x-doubled PPU output = near-sharp 256→640 resample)
 *        · 3D layer (scene3d.h hook): geometry drawn here with depth
 *          testing, cameras reconstructed from the Mode-7 lines (m7view.h)
 *        ▼
 *   [2] optional FXAA at internal resolution (cheap, 9 taps)
 *        ▼
 *   [3] output pass → default framebuffer, aspect-fit to the window
 *       (480p / 720p / 1080p / fullscreen): nearest, linear or
 *       sharp-bilinear upscale, optional scanlines
 *        ▼
 *   ImGui menu drawn on top at native output resolution
 */
#ifndef SNESRECOMP_GFX_RENDERER_H
#define SNESRECOMP_GFX_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Internal target for the original 4:3 picture; widescreen widens it. */
#define SMK_INTERNAL_W 640
#define SMK_INTERNAL_H 480
#define SMK_INTERNAL_MAX_W 960

enum {
    SMK_FILTER_NEAREST = 0,
    SMK_FILTER_LINEAR  = 1,
    SMK_FILTER_SHARP   = 2,   /* sharp-bilinear: integer prescale + linear */
};

typedef struct {
    int filter;      /* SMK_FILTER_* for the internal → output upscale */
    int fxaa;        /* 1 = run the FXAA pass on the internal target */
    int scanlines;   /* 1 = darken between SNES lines */
} RendererSettings;

/* Create GL objects. Requires a current context with smkgl_load() done. */
bool renderer_init(void);
void renderer_shutdown(void);

/* Upload rows y..y+h-1 of a `width`-pixel XBGR frame (2 pixels per SNES
 * column). Resizes the internal target to match the frame's aspect. */
void renderer_upload_snes(const uint8_t *fb, int width, int y, int h);

/* Current internal target size (pixels). */
void renderer_get_internal_size(int *w, int *h);

/* Full-screen Mode-7 presentation (see m7view.h M7_LAYOUT_FULLSCREEN): the
 * race band is drawn shifted down, the ground below it is rendered on the
 * GPU from the Mode-7 tilemap / characters / palette through the camera, and
 * the area above is filled with the sky colour. */
typedef struct {
    int     cols, lines;            /* presented picture (SNES columns / lines) */
    int     line_offset;            /* SNES line y is presented at y + line_offset */
    int     band_first, band_last;  /* SNES lines of the race band to keep */
    float   sky[3];
    float   cam[2], fwd[2], right[2], focal, height, horizon;   /* floor camera */
    int     outside;                /* 0 wrap, 1 backdrop, 2 tile 0 (charFill) */
    uint8_t tilemap[128 * 128];     /* Mode-7 tile ids */
    uint8_t chars[128 * 128];       /* 256 8x8 characters as a 16x16 atlas */
    uint8_t palette[256 * 4];       /* RGBA, screen brightness applied */
} RendererM7Frame;

/* Copy the full-screen data for the next present (NULL: native layout). */
void renderer_set_m7_frame(const RendererM7Frame *f);

/* Lines the PPU rendered above the race band's first visible line
 * (full-screen layout; same 8-bytes-per-column XBGR pairs as the SNES frame,
 * row 0 topmost; NULL: none). Drawn directly above the band. */
#define RENDERER_ABOVE_MAX 64
void renderer_set_above(const uint8_t *pixels, int cols, int lines);

/* Separated HUD layer for the next present (RGBA + class per pixel, cols x
 * lines; NULL: none) and the texture it lands in (0 when absent). */
void renderer_set_hud(const uint8_t *pixels, int cols, int lines);
unsigned renderer_hud_texture(int *cols, int *lines);

/* Compose and draw one output frame into the default framebuffer of size
 * out_w x out_h (pixels). `top_inset` pixels at the top are reserved for
 * the menu bar and cleared to (bar_r, bar_g, bar_b). */
void renderer_draw(int out_w, int out_h, int top_inset,
                   float bar_r, float bar_g, float bar_b,
                   const RendererSettings *s);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_GFX_RENDERER_H */

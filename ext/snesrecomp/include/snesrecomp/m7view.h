/*
 * m7view.h — reconstruct a 3D camera from what the PPU drew in Mode 7.
 *
 * A perspective Mode-7 floor is a camera looking at a ground plane: each
 * scanline y maps screen column x to ground point P(y) + x * S(y). Captured
 * per line by the PPU (presentation only), those affine rows are fitted to a
 * pinhole camera with a horizontal view axis:
 *
 *     s(y) = |S(y)| = H / (y - y_h)          (world units per pixel)
 *     ground(col 128, y) = cam + F * f * s(y)
 *
 * giving camera position `cam` (ground plane), height H, forward F, right R,
 * focal length f (pixels) and horizon line y_h. On Super Mario Kart races the
 * fit is within ~1 px vertically and ~1.5 px rms horizontally. No game-specific
 * knowledge is used, so it works for any Mode-7 game.
 *
 * World units are Mode-7 map pixels (0..1023); X grows right on the map, Y
 * grows down the map, Z is height above the ground plane.
 */
#ifndef SNESRECOMP_M7VIEW_H
#define SNESRECOMP_M7VIEW_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M7VIEW_MAX_BANDS 4

typedef struct {
    int   line0, line1;     /* first / last scanline of the band (1-based) */
    bool  perspective;      /* false = flat (scale constant down the band) */
    bool  chase;            /* perspective with the horizon at the top of the band:
                             * a racing view (SMK's tilted overview map is not) */
    /* perspective camera (valid when perspective) */
    float cam[2];           /* camera ground position */
    float height;           /* H: camera height above the ground */
    float fwd[2];           /* F: unit forward on the ground plane */
    float right[2];         /* R: unit screen-right on the ground plane */
    float focal;            /* f: pixels */
    float horizon;          /* y_h: scanline of the horizon */
    float rms_line;         /* fit quality, pixels */
    float rms_col;
    /* scanlines of the *presented* picture where 3D for this band may draw
     * (differs from line0/line1 in the full-screen layout) */
    int   draw_line0, draw_line1;
} M7Band;

/* How the frame is presented. */
enum {
    M7_LAYOUT_NATIVE = 0,      /* the SNES picture as-is */
    M7_LAYOUT_FULLSCREEN = 1,  /* 1-player race: top band shifted down by line_offset,
                                * GPU floor below it, sky above, split-screen map dropped */
};

typedef struct {
    int    cols;            /* picture width in SNES columns (256 + 2*ext) */
    int    lines;           /* picture height (224, or 239 with overscan) */
    int    nbands;
    M7Band band[M7VIEW_MAX_BANDS];
    int    layout;          /* M7_LAYOUT_* */
    int    line_offset;     /* full-screen: SNES line y is presented at y + line_offset */
    int    top_skip;        /* full-screen: blank lines dropped from the top of the band */
    float  sky[3];          /* full-screen: sky colour above the band (0..1) */
} M7View;

/* View of the most recently submitted frame (nbands == 0 when the frame has
 * no Mode-7 lines). */
const M7View *snesrecomp_m7_view(void);

/* Column-major clip-space matrix for a perspective band, mapping world
 * (X, Y, Z) into the renderer's internal target (whole picture in NDC).
 * Depth: near..far world units in front of the camera. */
void m7view_clip_matrix(const M7View *v, const M7Band *b, float near_z, float far_z,
                        float out[16]);

/* Project a world point to picture coordinates (column, line); returns false
 * if it is behind the camera. Column 0..cols-1 counts from the left edge of
 * the (possibly widened) picture. */
bool m7view_project(const M7View *v, const M7Band *b, float x, float y, float z,
                    float *col, float *line);

/* Band whose perspective view contains scanline `line`, or NULL. */
const M7Band *m7view_band_at(const M7View *v, int line);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_M7VIEW_H */

/*
 * neo_karts.c — keep other karts visible below SMK's race view.
 *
 * In the full-screen layout the track continues below the band SMK draws,
 * but SMK stops drawing a kart once it drops past the band's bottom edge (on
 * the SNES that is the split-screen border), so karts vanished as you passed
 * them. While a kart is inside the band, its sprite is captured from the
 * separated sprite layer (the pixels around its projected position); once it
 * is below the band, that capture is drawn as a camera-facing billboard at
 * its WRAM position, with a blob shadow. Presentation only.
 */
#include "neo.h"

#include <snesrecomp/bus.h>
#include <snesrecomp/snesrecomp.h>
#include <math.h>
#include <string.h>

#define CAP     48              /* capture size, pixels (SMK karts are <= 32) */
#define SLOTS   SMK_RACER_COUNT
#define HUMAN_FLAG 0x8000

typedef struct {
    bool  valid;
    int   w, h;                 /* captured pixels */
    float world_w, world_h;     /* size in world units at capture */
    float dx;                   /* sprite centre offset to the right, world units */
    float z;                    /* height of its bottom row above the ground */
    int   age;                  /* frames since captured */
} Capture;

static Capture     s_cap[SLOTS];
static uint8_t     s_atlas[CAP * SLOTS * CAP * 4];   /* SLOTS captures side by side */
static Scene3DTex *s_tex = NULL;
static bool        s_dirty = false;

/* Grab the sprite pixels of a kart standing at picture (col, line), drawn at
 * `depth`; true if something kart-sized was found. */
static bool capture(int slot, const uint8_t *spr, int cols, int lines, float col, float line,
                    float depth, float focal) {
    float px_per_unit = focal / depth;
    int r = (int)(5.5f * px_per_unit) + 3;            /* half width of the search box */
    if (r > CAP / 2) r = CAP / 2;
    int cx = (int)floorf(col), base = (int)floorf(line);
    int x0 = cx - r, x1 = cx + r, y0 = base - 2 * r, y1 = base + 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= cols) x1 = cols - 1;
    if (y1 >= lines) y1 = lines - 1;
    int bx0 = 1 << 20, by0 = 1 << 20, bx1 = -1, by1 = -1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int bottom = spr[(y * cols + x) * 4 + 3];
            /* this kart's sprites end at or a little above its base; nearer
             * objects end lower, front-most ones are 255 */
            if (bottom == 0 || bottom == 255 || bottom > base + 4 || bottom < base - 2 * r - 2) continue;
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            if (y > by1) by1 = y;
        }
    }
    if (bx1 < 0 || bx1 - bx0 < 3 || by1 - by0 < 3) return false;
    int w = bx1 - bx0 + 1, h = by1 - by0 + 1;
    if (w > CAP) w = CAP;
    if (h > CAP) h = CAP;
    Capture *c = &s_cap[slot];
    for (int y = 0; y < CAP; y++) {
        uint8_t *row = &s_atlas[(y * CAP * SLOTS + slot * CAP) * 4];
        memset(row, 0, CAP * 4);
        if (y >= h) continue;
        for (int x = 0; x < w; x++) {
            const uint8_t *p = &spr[((by0 + y) * cols + bx0 + x) * 4];
            int bottom = p[3];
            bool mine = bottom != 0 && bottom != 255 && bottom <= base + 4 && bottom >= base - 2 * r - 2;
            row[x * 4 + 0] = p[0]; row[x * 4 + 1] = p[1]; row[x * 4 + 2] = p[2];
            row[x * 4 + 3] = mine ? 255 : 0;
        }
    }
    c->valid = true;
    c->w = w;
    c->h = h;
    c->world_w = w / px_per_unit;
    c->world_h = h / px_per_unit;
    c->z = by1 < base ? (base - by1) / px_per_unit : 0.0f;
    c->dx = ((bx0 + bx1 + 1) * 0.5f - col) / px_per_unit;
    c->age = 0;
    s_dirty = true;
    return true;
}

void neo_karts_draw(const Scene3DFrame *f, const M7View *v, const NeoWorld *w) {
    if (v->layout != M7_LAYOUT_FULLSCREEN) {
        memset(s_cap, 0, sizeof(s_cap));
        return;
    }
    int cols, lines;
    const uint8_t *spr = snesrecomp_sprite_layer(&cols, &lines);
    const M7Band *b = &v->band[0];
    if (!spr || !b->chase) return;
    if (!s_tex) s_tex = scene3d_tex_rgba(CAP * SLOTS, CAP);

    int draw[SLOTS], ndraw = 0;
    for (int i = 0; i < SMK_RACER_COUNT; i++) {
        uint32_t rec = SMK_RAM_RACERS + (uint32_t)i * SMK_RACER_STRIDE;
        if (bus_wram_read16(rec + SMK_RACER_FLAGS) & HUMAN_FLAG) continue;   /* the player */
        const NeoRacer *r = &w->racer[i];
        Capture *c = &s_cap[i];
        if (c->valid && ++c->age > 600) c->valid = false;   /* stale after 10 s */
        float col, line;
        if (!m7view_project(v, b, r->x, r->y, 0.0f, &col, &line)) continue;
        float depth = (r->x - b->cam[0]) * b->fwd[0] + (r->y - b->cam[1]) * b->fwd[1];
        float pic_line = line + 0.5f - v->line_offset;     /* scanline of its base */
        if (pic_line < b->line0 + 4 || col < 0 || col >= cols) continue;
        if (pic_line <= b->line1 - 2) {
            capture(i, spr, cols, lines, col, pic_line, depth, b->focal);   /* inside the band */
        } else if (c->valid && depth > M7VIEW_NEAR_Z + 2.0f) {
            draw[ndraw++] = i;                                              /* below it */
        }
    }
    if (ndraw == 0) return;
    if (s_dirty) { scene3d_tex_update_rgba(s_tex, s_atlas); s_dirty = false; }

    static const float light[3] = { 0.35f, 0.55f, -1.0f };
    float clip[16];
    m7view_clip_matrix(v, b, M7VIEW_NEAR_Z, M7VIEW_FAR_Z, clip);
    int l0, l1;
    if (!neo_band_lines(v, b, 0, &l0, &l1)) return;
    scene3d_clip_lines(f, l0, l1);
    scene3d_begin(clip, light);
    float br = b->brightness;
    for (int k = 0; k < ndraw; k++) {
        const NeoRacer *r = &w->racer[draw[k]];
        scene3d_blob_shadow(r->x, r->y, 3.5f, 0.45f * br);
    }
    float tint[4] = { br, br, br, 1.0f };
    for (int k = 0; k < ndraw; k++) {
        int i = draw[k];
        const Capture *c = &s_cap[i];
        const NeoRacer *r = &w->racer[i];
        float uv0[2] = { (float)(i * CAP) / (CAP * SLOTS), 0.0f };
        float uv1[2] = { (float)(i * CAP + c->w) / (CAP * SLOTS), (float)c->h / CAP };
        float x = r->x + b->right[0] * c->dx, y = r->y + b->right[1] * c->dx;
        scene3d_billboard(s_tex, uv0, uv1, x, y, c->z, b->right, c->world_w, c->world_h, tint);
    }
    scene3d_end();
}

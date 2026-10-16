/*
 * neo_minimap.c — Mario Kart 8-style circuit map for the full-screen
 * 1-player layout (bottom right), replacing SMK's split-screen overview.
 *
 * The circuit shape comes from the Mode-7 tilemap: tiles whose behaviour
 * (WRAM $0B00 table) is road, item panel or coin form a 128x128 mask, drawn
 * filled with a dark outline via linear filtering. Racers are discs in their
 * character colour, player 1 on top with a white ring. Visual only.
 */
#include "neo.h"

#include <snesrecomp/bus.h>
#include <snesrecomp/snesrecomp.h>
#include <string.h>

static Scene3DTex *s_mask_tex = NULL;
static uint8_t     s_mask[128 * 128];
static uint8_t     s_uploaded[128 * 128];
static bool        s_have_upload = false;
static int         s_bx0, s_by0, s_bx1, s_by1;   /* track bounds, tiles */

static bool is_track(uint8_t behaviour) {
    return behaviour == SMK_TILE_ROAD || behaviour == SMK_TILE_ITEM_PANEL ||
           behaviour == SMK_TILE_COIN;
}

/* Rebuild the mask from VRAM; returns false if no track is visible. */
static bool build_mask(void) {
    int x0 = 128, y0 = 128, x1 = -1, y1 = -1, n = 0;
    for (int ty = 0; ty < 128; ty++) {
        for (int tx = 0; tx < 128; tx++) {
            uint8_t tile = (uint8_t)snesrecomp_read_vram((uint16_t)(ty * 128 + tx));
            bool t = is_track(bus_wram_read8(SMK_RAM_TILE_BEHAVIOUR + tile));
            s_mask[ty * 128 + tx] = t ? 255 : 0;
            if (!t) continue;
            n++;
            if (tx < x0) x0 = tx;
            if (tx > x1) x1 = tx;
            if (ty < y0) y0 = ty;
            if (ty > y1) y1 = ty;
        }
    }
    if (n < 64) return false;
    s_bx0 = x0 - 2; s_by0 = y0 - 2; s_bx1 = x1 + 3; s_by1 = y1 + 3;   /* margin for the outline */
    return true;
}

void neo_minimap_draw(const Scene3DFrame *f, const M7View *v, const NeoWorld *w) {
    if (v->layout != M7_LAYOUT_FULLSCREEN || !build_mask()) return;
    if (!s_mask_tex) s_mask_tex = scene3d_tex_r8(128, 128);
    if (!s_have_upload || memcmp(s_mask, s_uploaded, sizeof(s_mask)) != 0) {
        scene3d_tex_update(s_mask_tex, s_mask);
        memcpy(s_uploaded, s_mask, sizeof(s_mask));
        s_have_upload = true;
    }

    /* fit inside the floor area under the race band, bottom right */
    float tw = (float)f->target_w, th = (float)f->target_h;
    float band_bottom = (float)(v->band[0].line1 + v->line_offset) / v->lines * th;
    float margin = th * 0.03f;
    float h = th - band_bottom - 2.0f * margin;
    if (h > th * 0.34f) h = th * 0.34f;
    if (h < th * 0.12f) return;
    float bw = (float)(s_bx1 - s_bx0), bh = (float)(s_by1 - s_by0);
    float wpx = h * bw / bh;
    if (wpx > tw * 0.32f) { h *= tw * 0.32f / wpx; wpx = tw * 0.32f; }
    float x = tw - margin - wpx, y = th - margin - h;

    scene3d_ui_begin(f);
    static const float fill[4] = { 0.97f, 0.97f, 0.99f, 0.88f };
    static const float edge[4] = { 0.10f, 0.11f, 0.16f, 0.80f };
    float uv0[2] = { s_bx0 / 128.0f, s_by0 / 128.0f };
    float uv1[2] = { s_bx1 / 128.0f, s_by1 / 128.0f };
    scene3d_ui_mask(s_mask_tex, x, y, wpx, h, uv0, uv1, fill, edge);

    /* racers: map pixels -> minimap; CPUs first, player 1 last (on top) */
    float sx = wpx / (bw * 8.0f), sy = h / (bh * 8.0f);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = SMK_RACER_COUNT - 1; i >= 0; i--) {
            bool player = (i == 0);
            if ((pass == 1) != player) continue;
            const NeoRacer *r = &w->racer[i];
            float cx = x + (r->x - s_bx0 * 8.0f) * sx;
            float cy = y + (r->y - s_by0 * 8.0f) * sy;
            const float *c = neo_char_color[r->character];
            float col[4] = { c[0], c[1], c[2], 1.0f };
            static const float ring_cpu[4] = { 0.08f, 0.08f, 0.10f, 0.95f };
            static const float ring_p1[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            float rad = th * (player ? 0.024f : 0.016f);
            scene3d_ui_disc(cx, cy, rad, col, player ? ring_p1 : ring_cpu, player ? 2.5f : 1.5f);
        }
    }
    scene3d_ui_end();
}

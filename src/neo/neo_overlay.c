/*
 * neo_overlay.c — the NeoSMK 3D layer, drawn over the SNES picture through
 * the renderer's scene hook with cameras reconstructed from Mode 7.
 *
 * Debug → "3D camera markers" floats a marker over every racer: if the
 * camera reconstruction and RAM map are right, each marker sits on its kart
 * sprite. (No blob shadows here: drawn over the SNES picture they would
 * darken the kart sprite itself; they belong under NeoSMK's own 3D objects.)
 */
#include "neo.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <snesrecomp/m7view.h>
#include <snesrecomp/menu_overlay.h>
#include <snesrecomp/scene3d.h>
#include <snesrecomp/snesrecomp.h>
#include <string.h>

/* options (persisted in smk_config.ini) */
static int s_markers = 0;
static int s_itemboxes = 1;
static int s_minimap = 1;
static int s_mk8_hud = 1;
static int s_ui_anchor = 1;


static Scene3DMesh *s_pyramid = NULL;
static Scene3DMesh *s_spark = NULL;

#define NEAR_Z 1.0f
#define FAR_Z  2048.0f

static void draw_markers(const Scene3DFrame *f, const M7View *v, const NeoWorld *w) {
    if (!s_pyramid) s_pyramid = scene3d_mesh_pyramid(1.0f, 1.0f, 1.0f);
    static const float light[3] = { 0.35f, 0.55f, -1.0f };
    for (int bi = 0; bi < v->nbands; bi++) {
        const M7Band *b = &v->band[bi];
        if (!b->chase) continue;
        float clip[16];
        m7view_clip_matrix(v, b, NEAR_Z, FAR_Z, clip);
        int l0, l1;
        if (!neo_band_lines(v, b, 8, &l0, &l1)) continue;   /* karts overhang the band a little */
        scene3d_clip_lines(f, l0, l1);
        scene3d_begin(clip, light);
        for (int i = 0; i < SMK_RACER_COUNT; i++) {
            const NeoRacer *r = &w->racer[i];
            float model[16];
            const float *c = neo_char_color[r->character];
            float tint[4] = { c[0], c[1], c[2], 1.0f };
            /* hovering marker, turned to the racer's heading */
            scene3d_mat_trs(model, r->x, r->y, 9.0f, -r->heading, 3.0f, 3.0f, 5.0f);
            scene3d_draw(s_pyramid, model, tint);
        }
        scene3d_end();
    }
}

/* Cheap deterministic hash for visual jitter (not gameplay). */
static float jitter(uint32_t n) {
    n = (n ^ 61u) ^ (n >> 16);
    n *= 9u;
    n ^= n >> 4;
    n *= 0x27d4eb2du;
    n ^= n >> 15;
    return (float)(n & 0xFFFF) / 65535.0f;
}

/* Mini-turbo sparks at the rear wheels (yellow / orange) and a flame while
 * boosting, for each human player in its own perspective band. */
static void draw_player_fx(const Scene3DFrame *f, const M7View *v, const NeoWorld *w) {
    static uint32_t tick = 0;
    tick++;
    if (!s_spark) s_spark = scene3d_mesh_cube(1.0f, 1.0f, 1.0f);
    static const float light[3] = { 0.35f, 0.55f, -1.0f };
    int band_for_player = 0;
    for (int bi = 0; bi < v->nbands && band_for_player < 2; bi++) {
        const M7Band *b = &v->band[bi];
        if (!b->chase) continue;
        int p = band_for_player++;
        int stage = 0, dir = 0, boosting = 0;
        neo_gameplay_spark_state(p, &stage, &dir, &boosting);
        if (stage == 0 && !boosting) continue;
        const NeoRacer *r = &w->racer[p];
        float fx = sinf(r->heading), fy = -cosf(r->heading);   /* kart forward */
        float rx = cosf(r->heading), ry = sinf(r->heading);    /* kart right */
        float clip[16];
        m7view_clip_matrix(v, b, NEAR_Z, FAR_Z, clip);
        int l0, l1;
        if (!neo_band_lines(v, b, 12, &l0, &l1)) continue;
        scene3d_clip_lines(f, l0, l1);
        scene3d_begin(clip, light);
        for (int side = -1; side <= 1; side += 2) {
            float wx = r->x - fx * 2.6f + rx * 2.3f * side;
            float wy = r->y - fy * 2.6f + ry * 2.3f * side;
            int n = boosting ? 6 : (stage == 2 ? 6 : 4);
            for (int k = 0; k < n; k++) {
                uint32_t seed = tick * 131u + (uint32_t)k * 17u + (uint32_t)(side + 1) * 7u + (uint32_t)p * 1009u;
                float jx = (jitter(seed) - 0.5f) * 2.4f, jy = (jitter(seed + 1) - 0.5f) * 2.4f;
                float jz = jitter(seed + 2) * (boosting ? 1.6f : 2.6f);
                float sz = (boosting ? 1.0f : 0.7f) + jitter(seed + 3) * (stage == 2 || boosting ? 1.1f : 0.7f);
                float model[16];
                float tint[4];
                if (boosting) {        /* flame: white-hot core to orange */
                    float t = jitter(seed + 4);
                    tint[0] = 2.4f; tint[1] = 1.2f + t; tint[2] = 0.4f + t * 0.6f; tint[3] = 0.9f;
                    jx -= fx * (1.0f + jitter(seed + 6) * 2.5f);
                    jy -= fy * (1.0f + jitter(seed + 6) * 2.5f);
                } else if (stage == 2) { /* orange sparks */
                    tint[0] = 2.4f; tint[1] = 1.0f; tint[2] = 0.15f; tint[3] = 1.0f;
                } else {                 /* yellow sparks */
                    tint[0] = 2.2f; tint[1] = 2.0f; tint[2] = 0.35f; tint[3] = 1.0f;
                }
                scene3d_mat_trs(model, wx + jx, wy + jy, 0.2f + jz,
                                jitter(seed + 5) * 6.2831853f, sz, sz, sz);
                scene3d_draw(s_spark, model, tint);
            }
        }
        scene3d_end();
    }
}

/* SMK's HUD as the PPU tags it (snesrecomp_set_hud_config): lives / coins are
 * OAM 16-23, timer + lap box are BG2 of the Mode-0 scenery strip (BG3/BG4 are
 * the hills and trees), and the position
 * numeral is window-shaped colour math (fixed colour added inside colour
 * windows 1 | 2, reshaped per line by HDMA). */
static void configure_hud(bool in_race) {
    SnesHudConfig c;
    memset(&c, 0, sizeof(c));
    c.obj_mask[16 >> 3] = 0xFF;          /* OAM 16..23 */
    c.bg_mask = 1 << 1;                  /* BG2 */
    c.window_math = true;
    c.separate = in_race;      /* drawn by draw_hud, over the 3D */
    snesrecomp_set_hud_config(&c);
}

/* Mario Kart 8 placement: timer top right, lives / coins bottom left, a big
 * position numeral bottom right with the circuit map above it. */
/* SMK's HUD where the game put it (separated so it stays over the 3D). */
static void draw_hud_in_place(const Scene3DFrame *f, const M7View *v) {
    const SnesHudFrame *hf = snesrecomp_hud();
    if (!hf->valid) return;
    float sx = (float)f->target_w / hf->cols, sy = (float)f->target_h / hf->lines;
    int off = v->layout == M7_LAYOUT_FULLSCREEN ? v->line_offset : 0;
    for (int cls = 1; cls <= 3; cls++) {
        if (!hf->box[cls].present) continue;
        scene3d_ui_hud(cls, hf->box[cls].x0 * sx, (hf->box[cls].y0 + off) * sy, 1.0f);
    }
    if (hf->box[SNES_HUD_BG].present)
        neo_items_draw_hud(hf->box[SNES_HUD_BG].x0 * sx, (hf->box[SNES_HUD_BG].y0 + off) * sy, sx, sy);
}

static void draw_hud(const Scene3DFrame *f, const M7View *v, const NeoWorld *w) {
    float tw = (float)f->target_w, th = (float)f->target_h;
    float m = th * 0.03f;
    float bottom = th - m;
    bool full = v->layout == M7_LAYOUT_FULLSCREEN;
    scene3d_ui_begin(f);
    if (!full || !s_mk8_hud) {
        draw_hud_in_place(f, v);
    } else {
        float wd, ht;
        if (scene3d_ui_hud_size(SNES_HUD_BG, 1.0f, &wd, &ht)) {
            scene3d_ui_hud(SNES_HUD_BG, tw - m - wd, m, 1.0f);
            const SnesHudFrame *hf = snesrecomp_hud();
            neo_items_draw_hud(tw - m - wd, m, tw / hf->cols, th / hf->lines);
        }
        if (scene3d_ui_hud_size(SNES_HUD_OBJ, 1.25f, &wd, &ht))
            scene3d_ui_hud(SNES_HUD_OBJ, m, th - m - ht, 1.25f);
        /* Position numeral. SMK zooms it briefly when the place changes and
         * blows it up at the finish; keep it at its steady height so it
         * stays in the corner (the circuit map sits on top of it). */
        static float steady_h = 0.0f, last_h = 0.0f;
        static int stable = 0;
        float ht1;
        if (scene3d_ui_hud_size(SNES_HUD_MATH, 1.0f, NULL, &ht1)) {
            stable = fabsf(ht1 - last_h) < 0.5f ? stable + 1 : 0;
            last_h = ht1;
            /* adopt a height once it has held for a while (a new glyph); never
             * one far above the normal size (the finish zoom can last) */
            if (ht1 < th * 0.12f && (steady_h == 0.0f || stable >= 30)) steady_h = ht1;
            float target = steady_h > 0.0f ? steady_h : th * 0.06f;
            float scale = 2.0f * target / ht1;
            scene3d_ui_hud_size(SNES_HUD_MATH, scale, &wd, &ht);
            scene3d_ui_hud(SNES_HUD_MATH, tw - m - wd, th - m - ht, scale);
        }
        if (steady_h > 0.0f) bottom = th - m - 2.0f * steady_h - m * 0.5f;
    }
    scene3d_ui_end();
    if (full && s_minimap) neo_minimap_draw(f, v, w, tw - m, bottom, th * 0.30f);
}

/* The player's own kart stays in front of walls / pipes between it and the
 * camera: tell the sprite layer where it is (chase bands are in player order,
 * player n = racer n). */
static void player_front_boxes(const M7View *v, const NeoWorld *w) {
    int p = 0;
    for (int bi = 0; bi < v->nbands && p < 2; bi++) {
        const M7Band *b = &v->band[bi];
        if (!b->chase) continue;
        const NeoRacer *r = &w->racer[p];
        float col, line;
        if (m7view_project(v, b, r->x, r->y, 0.0f, &col, &line)) {
            float depth = (r->x - b->cam[0]) * b->fwd[0] + (r->y - b->cam[1]) * b->fwd[1];
            float px = b->focal / (depth > 1.0f ? depth : 1.0f);
            float base = line + 0.5f - (v->layout == M7_LAYOUT_FULLSCREEN ? v->line_offset : 0);
            scene3d_sprites_front_box(p, col - 8.0f * px, base - 14.0f * px, col + 8.0f * px,
                                      base + 3.0f, base);
        }
        p++;
    }
}

static void neo_scene(const Scene3DFrame *f, void *user) {
    (void)user;
    const M7View *v = snesrecomp_m7_view();
    if (v->nbands == 0) return;
    NeoWorld w;
    neo_world_read(&w);
    if (w.in_race) {
        static uint32_t frames = 0;
        frames++;
        neo_track_draw(f, v, &w, frames / 60.0f);
        neo_pipes_draw(f, v, &w);
        neo_karts_draw(f, v, &w);
        if (s_itemboxes) neo_itembox_draw(f, v, frames / 60.0f);
        draw_player_fx(f, v, &w);
        if (s_markers) draw_markers(f, v, &w);
        player_front_boxes(v, &w);
        scene3d_sprites();      /* karts and track objects, depth-sorted with the 3D */
    }
    /* the separated HUD (also on the frame the race ends, while the
     * separation still lags a frame behind) */
    if (snesrecomp_hud()->valid) draw_hud(f, v, &w);
}

bool neo_track_hides_panels(void) { return s_itemboxes != 0; }

/* The 3D layer needs the track sprites (karts, shells, ...) on their own
 * depth-sorted layer. Screen-space sprites go in front of all 3D: OAM 0-7
 * (race position icons), 8-15 (Lakitu and the start lights), 16-23 (lives /
 * coins), 24-27 (banner text) and 120-127 (Lakitu's shadow). SMK hands out
 * the other entries to karts and objects as it needs them. */
static const uint8_t k_front_oam[16] = { 0xFF, 0xFF, 0xFF, 0x0F, 0, 0, 0, 0,
                                          0, 0, 0, 0, 0, 0, 0, 0xFF };

/* Widescreen: SMK's own UI moves to the screen edges instead of staying in
 * the 4:3 middle. Position icons (OAM 0-7) go left; lives / coins (16-23),
 * the timer / item box (BG2) and the position number (colour window) go right. */
static void configure_anchors(void) {
    static int8_t obj[128];
    static const int8_t bg[4] = { 0, 1, 0, 0 };
    for (int i = 0; i < 8; i++) obj[i] = -1;
    for (int i = 16; i < 24; i++) obj[i] = 1;
    /* BG2 lines 1-3 are a full-width black border; the timer starts below */
    if (s_ui_anchor) snesrecomp_set_ui_anchors(obj, bg, 4, 1);
    else snesrecomp_set_ui_anchors(NULL, NULL, 0, 0);
}

void neo_overlay_sync(bool in_race) {
    static int applied = -1;
    if (applied != (int)in_race) { configure_hud(in_race); applied = in_race; }
    bool any3d = s_itemboxes || s_markers || neo_track_coins_enabled() || neo_track_walls_enabled();
    snesrecomp_set_sprite_layer(in_race && any3d, NULL, k_front_oam);
    snesrecomp_set_sprite_front_palettes(1 << 4);   /* palette 4: SMK's text (HUD, banners, pause menu) */
    static int anchors = -1;
    int want = in_race && s_ui_anchor;
    if (want != anchors) {
        anchors = want;
        if (want) configure_anchors();
        else snesrecomp_set_ui_anchors(NULL, NULL, 0, 0);
    }
}

void neo_register_options(void) {
    neo_gameplay_register_options();
    neo_track_register_options();
    neo_pipes_register_options();
    menu_overlay_add_option("NeoSMK", "3D item boxes", "neo_itemboxes", &s_itemboxes, 1, NULL,
                            "Spinning Mario Kart 64-style boxes over the \"?\" panels\n"
                            "(visual only; a box disappears once its panel is used).");
    menu_overlay_add_option("NeoSMK", "Circuit map (full-screen)", "neo_minimap", &s_minimap, 1, NULL,
                            "Mario Kart 8-style circuit map with racer positions in the\n"
                            "bottom right of full-screen single-player races\n"
                            "(Graphics -> Full-screen 1P races).");
    menu_overlay_add_option("NeoSMK", "MK8-style HUD (full-screen)", "neo_mk8_hud", &s_mk8_hud, 1, NULL,
                            "Move the timer, lives / coins and position number to the\n"
                            "screen corners in full-screen single-player races.");
    menu_overlay_add_option("NeoSMK", "Widescreen UI at the edges", "neo_ui_anchor", &s_ui_anchor, 1, NULL,
                            "In widescreen, move SMK's own HUD (timer and item box,\n"
                            "lives / coins, position number and icons) out to the screen\n"
                            "edges instead of the 4:3 middle.");
    menu_overlay_add_option("Debug", "3D camera markers", "neo_debug_markers", &s_markers,
                            1, NULL,
                            "Float a 3D marker over every racer using the camera\n"
                            "reconstructed from Mode 7 and the WRAM racer table.\n"
                            "Each should sit on its kart sprite.");
}

void neo_install(void) {
    scene3d_set_hook(neo_scene, NULL);
    neo_pipes_install();
}

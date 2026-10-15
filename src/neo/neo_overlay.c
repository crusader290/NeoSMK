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

/* options (persisted in smk_config.ini) */
static int s_markers = 0;
static int s_itemboxes = 1;

/* per-character marker colours: Mario, Luigi, Bowser, Peach, DK Jr, Koopa,
 * Toad, Yoshi (SMK's character order, id * 2 in the racer record) */
static const float kCharColor[8][3] = {
    { 0.90f, 0.15f, 0.15f }, { 0.20f, 0.75f, 0.25f }, { 0.95f, 0.60f, 0.10f },
    { 1.00f, 0.55f, 0.75f }, { 0.55f, 0.35f, 0.20f }, { 0.30f, 0.65f, 0.95f },
    { 0.95f, 0.95f, 0.95f }, { 0.55f, 0.95f, 0.40f },
};

static Scene3DMesh *s_pyramid = NULL;
static Scene3DMesh *s_spark = NULL;

#define NEAR_Z 1.0f
#define FAR_Z  2048.0f

static void draw_markers(const Scene3DFrame *f, const M7View *v, const NeoWorld *w) {
    if (!s_pyramid) s_pyramid = scene3d_mesh_pyramid(1.0f, 1.0f, 1.0f);
    static const float light[3] = { 0.35f, 0.55f, -1.0f };
    for (int bi = 0; bi < v->nbands; bi++) {
        const M7Band *b = &v->band[bi];
        if (!b->perspective) continue;
        float clip[16];
        m7view_clip_matrix(v, b, NEAR_Z, FAR_Z, clip);
        scene3d_clip_lines(f, b->line0, b->line1 + 8);   /* karts overhang the band a little */
        scene3d_begin(clip, light);
        for (int i = 0; i < SMK_RACER_COUNT; i++) {
            const NeoRacer *r = &w->racer[i];
            float model[16];
            const float *c = kCharColor[r->character];
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
        if (!b->perspective) continue;
        int p = band_for_player++;
        int stage = 0, dir = 0, boosting = 0;
        neo_gameplay_spark_state(p, &stage, &dir, &boosting);
        if (stage == 0 && !boosting) continue;
        const NeoRacer *r = &w->racer[p];
        float fx = sinf(r->heading), fy = -cosf(r->heading);   /* kart forward */
        float rx = cosf(r->heading), ry = sinf(r->heading);    /* kart right */
        float clip[16];
        m7view_clip_matrix(v, b, NEAR_Z, FAR_Z, clip);
        scene3d_clip_lines(f, b->line0, b->line1 + 12);
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

static void neo_scene(const Scene3DFrame *f, void *user) {
    (void)user;
    const M7View *v = snesrecomp_m7_view();
    if (v->nbands == 0) return;
    NeoWorld w;
    neo_world_read(&w);
    if (!w.in_race) return;
    static uint32_t frames = 0;
    frames++;
    if (s_itemboxes) neo_itembox_draw(f, v, frames / 60.0f);
    draw_player_fx(f, v, &w);
    if (s_markers) draw_markers(f, v, &w);
}

void neo_register_options(void) {
    neo_gameplay_register_options();
    menu_overlay_add_option("NeoSMK", "3D item boxes", "neo_itemboxes", &s_itemboxes, 1, NULL,
                            "Spinning Mario Kart 64-style boxes over the \"?\" panels\n"
                            "(visual only; a box disappears once its panel is used).");
    menu_overlay_add_option("Debug", "3D camera markers", "neo_debug_markers", &s_markers,
                            1, NULL,
                            "Float a 3D marker over every racer using the camera\n"
                            "reconstructed from Mode 7 and the WRAM racer table.\n"
                            "Each should sit on its kart sprite.");
}

void neo_install(void) {
    scene3d_set_hook(neo_scene, NULL);
}

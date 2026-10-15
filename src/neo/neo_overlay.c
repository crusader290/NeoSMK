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

#include <stddef.h>

#include <snesrecomp/m7view.h>
#include <snesrecomp/menu_overlay.h>
#include <snesrecomp/scene3d.h>

/* options (persisted in smk_config.ini) */
static int s_markers = 0;

/* per-character marker colours: Mario, Luigi, Bowser, Peach, DK Jr, Koopa,
 * Toad, Yoshi (SMK's character order, id * 2 in the racer record) */
static const float kCharColor[8][3] = {
    { 0.90f, 0.15f, 0.15f }, { 0.20f, 0.75f, 0.25f }, { 0.95f, 0.60f, 0.10f },
    { 1.00f, 0.55f, 0.75f }, { 0.55f, 0.35f, 0.20f }, { 0.30f, 0.65f, 0.95f },
    { 0.95f, 0.95f, 0.95f }, { 0.55f, 0.95f, 0.40f },
};

static Scene3DMesh *s_pyramid = NULL;

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

static void neo_scene(const Scene3DFrame *f, void *user) {
    (void)user;
    if (!s_markers) return;
    const M7View *v = snesrecomp_m7_view();
    if (v->nbands == 0) return;
    NeoWorld w;
    neo_world_read(&w);
    if (!w.in_race) return;
    draw_markers(f, v, &w);
}

void neo_register_options(void) {
    menu_overlay_add_option("Debug", "3D camera markers", "neo_debug_markers", &s_markers,
                            1, NULL,
                            "Float a 3D marker over every racer using the camera\n"
                            "reconstructed from Mode 7 and the WRAM racer table.\n"
                            "Each should sit on its kart sprite.");
}

void neo_install(void) {
    scene3d_set_hook(neo_scene, NULL);
}

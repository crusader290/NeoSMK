/*
 * neo_itembox.c — Mario Kart 64-style 3D item boxes over SMK's "?" panels.
 *
 * SMK's item boxes are flat 2x2-tile panels painted into the Mode-7 tilemap.
 * Each frame the tilemap is scanned for tiles whose behaviour (WRAM $0B00
 * table) is "item panel"; tile ids come in aligned groups of four (top-left,
 * top-right, bottom-left, bottom-right), so (tile & 3) == 0 marks a panel's
 * top-left cell. A used panel is rewritten to a plain road tile, so its cube
 * simply disappears. Presentation only: VRAM and WRAM are read, never written.
 */
#include "neo.h"

#include <snesrecomp/bus.h>
#include <snesrecomp/m7view.h>
#include <snesrecomp/scene3d.h>
#include <snesrecomp/snesrecomp.h>
#include <math.h>
#include <stdlib.h>

#define MAX_PANELS        64
#define BOX_SIZE          5.0f     /* world units (a kart is ~7) */
#define BOX_HOVER         6.5f
#define NEAR_Z            1.0f
#define FAR_Z             2048.0f

typedef struct { float x, y, depth; } Panel;

static Panel  s_panels[MAX_PANELS];
static int    s_npanels = 0;
static Scene3DMesh *s_shell = NULL, *s_core = NULL;

/* Find every "?" panel in the current Mode-7 tilemap. */
static void scan_panels(void) {
    s_npanels = 0;
    for (int ty = 0; ty < 128; ty++) {
        for (int tx = 0; tx < 128; tx++) {
            uint8_t tile = (uint8_t)snesrecomp_read_vram((uint16_t)(ty * 128 + tx));
            if ((tile & 3) != 0) continue;
            if (bus_wram_read8(SMK_RAM_TILE_BEHAVIOUR + tile) != SMK_TILE_ITEM_PANEL) continue;
            if (s_npanels == MAX_PANELS) return;
            s_panels[s_npanels].x = tx * 8.0f + 8.0f;    /* centre of the 16x16 panel */
            s_panels[s_npanels].y = ty * 8.0f + 8.0f;
            s_npanels++;
        }
    }
}

static int by_depth_desc(const void *a, const void *b) {
    float da = ((const Panel *)a)->depth, db = ((const Panel *)b)->depth;
    return (da < db) - (da > db);
}

static void ensure_meshes(void) {
    if (s_shell) return;
    /* rainbow faces, like the N64 boxes' shimmer */
    static const float rainbow[6][3] = {
        { 1.00f, 0.25f, 0.25f }, { 0.25f, 0.30f, 1.00f }, { 1.00f, 0.85f, 0.10f },
        { 0.15f, 1.00f, 0.35f }, { 0.95f, 0.25f, 1.00f }, { 0.15f, 0.90f, 1.00f },
    };
    static const float core[6][3] = {
        { 1.0f, 0.95f, 0.55f }, { 1.0f, 0.95f, 0.55f }, { 1.0f, 0.95f, 0.55f },
        { 1.0f, 0.95f, 0.55f }, { 1.0f, 0.95f, 0.55f }, { 1.0f, 0.95f, 0.55f },
    };
    s_shell = scene3d_mesh_box(rainbow);
    s_core = scene3d_mesh_box(core);
}

void neo_itembox_draw(const Scene3DFrame *f, const M7View *v, float time_s) {
    scan_panels();
    if (s_npanels == 0) return;
    ensure_meshes();
    static const float light[3] = { 0.35f, 0.55f, -1.0f };

    for (int bi = 0; bi < v->nbands; bi++) {
        const M7Band *b = &v->band[bi];
        if (!b->perspective) continue;

        /* visible panels, far to near (the shells are translucent) */
        Panel vis[MAX_PANELS];
        int nvis = 0;
        for (int i = 0; i < s_npanels; i++) {
            float dx = s_panels[i].x - b->cam[0], dy = s_panels[i].y - b->cam[1];
            float depth = dx * b->fwd[0] + dy * b->fwd[1];
            float lat = dx * b->right[0] + dy * b->right[1];
            if (depth < NEAR_Z + BOX_SIZE || depth > FAR_Z) continue;
            if (fabsf(lat) > depth * 1.2f + BOX_SIZE) continue;      /* well outside the view */
            vis[nvis] = s_panels[i];
            vis[nvis].depth = depth;
            nvis++;
        }
        if (nvis == 0) continue;
        qsort(vis, (size_t)nvis, sizeof(Panel), by_depth_desc);

        float clip[16];
        m7view_clip_matrix(v, b, NEAR_Z, FAR_Z, clip);
        scene3d_clip_lines(f, b->line0, b->line1);
        scene3d_begin(clip, light);
        for (int i = 0; i < nvis; i++)
            scene3d_blob_shadow(vis[i].x, vis[i].y, BOX_SIZE * 0.8f, 0.5f);
        for (int i = 0; i < nvis; i++) {
            /* each box bobs and spins a little out of phase */
            float phase = vis[i].x * 0.037f + vis[i].y * 0.051f;
            float z = BOX_HOVER + sinf(time_s * 2.2f + phase) * 0.8f;
            float yaw = time_s * 1.8f + phase;
            float model[16];
            scene3d_mat_spin(model, vis[i].x, vis[i].y, z, yaw, 0.62f, BOX_SIZE * 0.42f);
            static const float core_tint[4] = { 1.7f, 1.6f, 1.2f, 1.0f };
            scene3d_draw(s_core, model, core_tint);
        }
        for (int i = 0; i < nvis; i++) {
            float phase = vis[i].x * 0.037f + vis[i].y * 0.051f;
            float z = BOX_HOVER + sinf(time_s * 2.2f + phase) * 0.8f;
            float yaw = time_s * 1.8f + phase;
            float hue = 0.85f + 0.15f * sinf(time_s * 3.0f + phase);   /* shimmer */
            float tint[4] = { 1.55f * hue, 1.55f * hue, 1.55f * hue, 0.80f };
            float model[16];
            scene3d_mat_spin(model, vis[i].x, vis[i].y, z, yaw, 0.62f, BOX_SIZE);
            scene3d_draw(s_shell, model, tint);
        }
        scene3d_end();
    }
}

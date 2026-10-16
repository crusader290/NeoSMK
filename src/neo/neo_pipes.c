/*
 * neo_pipes.c — Mario Kart 64-style 3D warp pipes instead of SMK's pipe sprites.
 *
 * Pipes are track objects: SMK keeps the ones near the player in its object
 * slots (WRAM $1800 + n * $80, X at +$18, Y at +$1C like the racers), all run
 * by the same handler (+$04 = $E4E7). Each is drawn as a green 3D pipe, and
 * before every frame the pipe's sprites (palette 7, matched to the pipe's
 * projected position) are hidden from the picture. Presentation only.
 */
#include "neo.h"

#include <snesrecomp/bus.h>
#include <snesrecomp/menu_overlay.h>
#include <snesrecomp/snesrecomp.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SMK_RAM_OBJECTS     0x1800
#define SMK_OBJ_STRIDE      0x80
#define SMK_OBJ_SLOTS       8
#define SMK_OBJ_HANDLER     0x04
#define SMK_OBJ_X           0x18
#define SMK_OBJ_Y           0x1C
#define SMK_PIPE_HANDLER    0xE4E7
#define SMK_PIPE_PALETTE    7

#define PIPE_RADIUS   3.6f      /* world units (a kart is ~7) */
#define PIPE_HEIGHT   8.0f
#define RIM_RADIUS    4.4f
#define RIM_HEIGHT    2.6f

static int s_pipes3d = 1;

typedef struct { float x, y; } Pipe;
static Pipe s_pipe[SMK_OBJ_SLOTS];
static int  s_npipes = 0;
static Scene3DMesh *s_mesh = NULL;

static void read_pipes(void) {
    s_npipes = 0;
    for (int i = 0; i < SMK_OBJ_SLOTS; i++) {
        uint32_t o = SMK_RAM_OBJECTS + (uint32_t)i * SMK_OBJ_STRIDE;
        if (bus_wram_read16(o + SMK_OBJ_HANDLER) != SMK_PIPE_HANDLER) continue;
        uint16_t x = bus_wram_read16(o + SMK_OBJ_X), y = bus_wram_read16(o + SMK_OBJ_Y);
        if (x >= 1024 || y >= 1024) continue;
        s_pipe[s_npipes].x = x;
        s_pipe[s_npipes].y = y;
        s_npipes++;
    }
}

/* Before a frame: hide the sprites of pipes drawn in 3D. A pipe sprite is a
 * palette-7 sprite standing on (or stacked above) a pipe's projected base. */
static void preframe(void *user) {
    (void)user;
    bool in_race = bus_wram_read16(SMK_RAM_GAME_STATE) == 0x0002;
    if (!in_race || !s_pipes3d) { s_npipes = 0; snesrecomp_set_obj_hide(NULL); return; }
    read_pipes();
    uint8_t hide[16] = { 0 };
    const M7View *v = snesrecomp_m7_view();
    int ext = (v->cols - 256) / 2;
    int off = v->layout == M7_LAYOUT_FULLSCREEN ? v->line_offset : 0;
    for (int bi = 0; bi < v->nbands; bi++) {
        const M7Band *b = &v->band[bi];
        if (!b->chase) continue;
        for (int p = 0; p < s_npipes; p++) {
            float col, line;
            if (!m7view_project(v, b, s_pipe[p].x, s_pipe[p].y, 0.0f, &col, &line)) continue;
            float sx = col - ext;           /* screen x as OAM counts it */
            float base = line + 0.5f - off; /* scanline under the pipe's base */
            if (base < b->line0 - 2 || base > b->line1 + 24) continue;
            for (int n = 0; n < 128; n++) {
                SnesOamEntry e;
                snesrecomp_read_oam(n, &e);
                if (e.palette != SMK_PIPE_PALETTE || e.y >= 240) continue;
                float cx = e.x + e.size * 0.5f, bottom = (float)(e.y + e.size);
                if (fabsf(cx - sx) > e.size * 0.5f + 6.0f) continue;
                if (bottom > base + 8.0f || bottom < base - 4.0f * e.size - 8.0f) continue;
                hide[n >> 3] |= (uint8_t)(1 << (n & 7));
            }
        }
    }
    snesrecomp_set_obj_hide(hide);
}

/* Tube along Z with caps, flat-shaded sides via per-vertex radial normals. */
static void tube(float *v, int *nv, uint16_t *ix, int *ni, float r, float z0, float z1,
                 const float side[3], const float top[3]) {
    enum { SEG = 16 };
    int base = *nv;
    for (int k = 0; k < SEG; k++) {
        float a = 6.2831853f * k / SEG, c = cosf(a), s = sinf(a);
        for (int h = 0; h < 2; h++) {
            float *o = v + (*nv) * 9;
            o[0] = r * c; o[1] = r * s; o[2] = h ? z1 : z0;
            o[3] = c; o[4] = s; o[5] = 0;
            o[6] = side[0]; o[7] = side[1]; o[8] = side[2];
            (*nv)++;
        }
    }
    for (int k = 0; k < SEG; k++) {
        int a0 = base + k * 2, a1 = a0 + 1, b0 = base + ((k + 1) % SEG) * 2, b1 = b0 + 1;
        ix[(*ni)++] = (uint16_t)a0; ix[(*ni)++] = (uint16_t)b0; ix[(*ni)++] = (uint16_t)b1;
        ix[(*ni)++] = (uint16_t)a0; ix[(*ni)++] = (uint16_t)b1; ix[(*ni)++] = (uint16_t)a1;
    }
    int centre = *nv;                   /* top cap */
    float *o = v + (*nv) * 9;
    o[0] = 0; o[1] = 0; o[2] = z1; o[3] = 0; o[4] = 0; o[5] = 1;
    o[6] = top[0]; o[7] = top[1]; o[8] = top[2];
    (*nv)++;
    for (int k = 0; k < SEG; k++) {
        float a = 6.2831853f * k / SEG;
        o = v + (*nv) * 9;
        o[0] = r * cosf(a); o[1] = r * sinf(a); o[2] = z1; o[3] = 0; o[4] = 0; o[5] = 1;
        o[6] = top[0]; o[7] = top[1]; o[8] = top[2];
        (*nv)++;
    }
    for (int k = 0; k < SEG; k++) {
        ix[(*ni)++] = (uint16_t)centre;
        ix[(*ni)++] = (uint16_t)(centre + 1 + k);
        ix[(*ni)++] = (uint16_t)(centre + 1 + (k + 1) % SEG);
    }
}

static void ensure_mesh(void) {
    if (s_mesh) return;
    static float v[200 * 9];
    static uint16_t ix[600];
    int nv = 0, ni = 0;
    static const float body[3]  = { 0.10f, 0.62f, 0.16f };
    static const float rim[3]   = { 0.16f, 0.80f, 0.22f };
    static const float rim_top[3] = { 0.20f, 0.85f, 0.25f };
    static const float hole[3]  = { 0.01f, 0.12f, 0.03f };
    tube(v, &nv, ix, &ni, PIPE_RADIUS, 0.0f, PIPE_HEIGHT, body, body);
    tube(v, &nv, ix, &ni, RIM_RADIUS, PIPE_HEIGHT, PIPE_HEIGHT + RIM_HEIGHT, rim, rim_top);
    tube(v, &nv, ix, &ni, RIM_RADIUS * 0.72f, PIPE_HEIGHT + RIM_HEIGHT, PIPE_HEIGHT + RIM_HEIGHT + 0.02f,
         hole, hole);                    /* the dark opening */
    s_mesh = scene3d_mesh_create(v, nv, ix, ni);
}

void neo_pipes_draw(const Scene3DFrame *f, const M7View *v, const NeoWorld *w) {
    if (!s_pipes3d || s_npipes == 0) return;
    ensure_mesh();
    static const float light[3] = { 0.35f, 0.55f, -1.0f };
    int nchase = 0;
    for (int bi = 0; bi < v->nbands; bi++) {
        const M7Band *b = &v->band[bi];
        if (!b->chase) continue;
        int player = nchase++;
        int l0, l1;
        if (!neo_band_lines(v, b, 0, &l0, &l1)) continue;
        float clip[16];
        m7view_clip_matrix(v, b, M7VIEW_NEAR_Z, M7VIEW_FAR_Z, clip);
        scene3d_clip_lines(f, l0, l1);
        scene3d_begin(clip, light);
        scene3d_near_cut(neo_view_cut(b, w, player));
        float br = b->brightness;
        float tint[4] = { 1.15f * br, 1.15f * br, 1.15f * br, 1.0f };
        for (int i = 0; i < s_npipes; i++) {
            float dx = s_pipe[i].x - b->cam[0], dy = s_pipe[i].y - b->cam[1];
            if (dx * b->fwd[0] + dy * b->fwd[1] < M7VIEW_NEAR_Z + RIM_RADIUS) continue;
            scene3d_blob_shadow(s_pipe[i].x, s_pipe[i].y, RIM_RADIUS * 1.2f, 0.4f * br);
            float model[16];
            scene3d_mat_trs(model, s_pipe[i].x, s_pipe[i].y, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
            scene3d_draw(s_mesh, model, tint);
        }
        scene3d_end();
    }
}

void neo_pipes_install(void) {
    snesrecomp_set_preframe_hook(preframe, NULL);
}

void neo_pipes_register_options(void) {
    menu_overlay_add_option("NeoSMK", "3D pipes", "neo_pipes3d", &s_pipes3d, 1, NULL,
                            "Green Mario Kart 64-style 3D pipes instead of the flat pipe\n"
                            "sprites (visual only; collisions are unchanged).");
}

/*
 * neo_track.c — track objects in 3D: Mario Kart 64-style coins and walls.
 *
 * Everything comes from the Mode-7 tilemap and SMK's tile behaviour table
 * (WRAM $0B00): coin tiles get a spinning 3D coin, barrier tiles are raised
 * into studded blocks, and the flat coin / "?" panel graphics are painted
 * over with the surrounding road through snesrecomp's Mode-7 tile override,
 * so only the 3D versions show. A collected coin or used panel is rewritten
 * to road by the game, so its 3D object disappears with it.
 *
 * Presentation only: VRAM and WRAM are read, never written.
 */
#include "neo.h"

#include <snesrecomp/bus.h>
#include <snesrecomp/menu_overlay.h>
#include <snesrecomp/snesrecomp.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COINS        256
#define COIN_RADIUS      3.0f      /* world units (a kart is ~7) */
#define COIN_HOVER       4.0f
#define WALL_HEIGHT      5.0f      /* block height; the stud adds WALL_STUD */
#define WALL_STUD        2.0f
#define WALL_MESH_VERTS  60000     /* uint16 indices: split the walls into chunks */
#define MAX_WALL_MESHES  8

/* options (persisted in smk_config.ini) */
static int s_coins3d = 1;
static int s_walls3d = 1;

/* tilemap as last scanned */
static uint8_t  s_tile[128 * 128];
static uint8_t  s_beh[256];
static bool     s_valid = false;
static uint16_t s_override[128 * 128];
static uint8_t  s_was_panel[128 * 128];  /* cells that held a "?" panel this race */
static bool     s_override_on = false;

typedef struct { float x, y, depth; } Coin;
static Coin s_coin[MAX_COINS];
static int  s_ncoins = 0;

static Scene3DMesh *s_coin_mesh = NULL;
static Scene3DMesh *s_wall_mesh[MAX_WALL_MESHES];
static int  s_nwall_meshes = 0;
static bool s_walls_dirty = true;

/* ---- mesh building ---- */

typedef struct {
    float    *v;
    uint16_t *ix;
    int       nv, ni, cap_v, cap_i;
} MeshBuf;

static bool mb_reserve(MeshBuf *m, int nv, int ni) {
    if (m->nv + nv > m->cap_v) {
        int c = m->cap_v ? m->cap_v * 2 : 1024;
        while (c < m->nv + nv) c *= 2;
        float *p = (float *)realloc(m->v, (size_t)c * 9 * sizeof(float));
        if (!p) return false;
        m->v = p; m->cap_v = c;
    }
    if (m->ni + ni > m->cap_i) {
        int c = m->cap_i ? m->cap_i * 2 : 2048;
        while (c < m->ni + ni) c *= 2;
        uint16_t *p = (uint16_t *)realloc(m->ix, (size_t)c * sizeof(uint16_t));
        if (!p) return false;
        m->ix = p; m->cap_i = c;
    }
    return true;
}

static void mb_vert(MeshBuf *m, float x, float y, float z, const float n[3], const float c[3]) {
    float *o = m->v + (size_t)m->nv * 9;
    o[0] = x; o[1] = y; o[2] = z;
    o[3] = n[0]; o[4] = n[1]; o[5] = n[2];
    o[6] = c[0]; o[7] = c[1]; o[8] = c[2];
    m->nv++;
}

static void mb_tri_flat(MeshBuf *m, const float a[3], const float b[3], const float c[3],
                        const float col[3]) {
    float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                   e1[0] * e2[1] - e1[1] * e2[0] };
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 0) { n[0] /= l; n[1] /= l; n[2] /= l; }
    int base = m->nv;
    mb_vert(m, a[0], a[1], a[2], n, col);
    mb_vert(m, b[0], b[1], b[2], n, col);
    mb_vert(m, c[0], c[1], c[2], n, col);
    m->ix[m->ni++] = (uint16_t)base;
    m->ix[m->ni++] = (uint16_t)(base + 1);
    m->ix[m->ni++] = (uint16_t)(base + 2);
}

static void mb_quad(MeshBuf *m, const float p[4][3], const float n[3], const float col[3]) {
    int base = m->nv;
    for (int k = 0; k < 4; k++) mb_vert(m, p[k][0], p[k][1], p[k][2], n, col);
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (int k = 0; k < 6; k++) m->ix[m->ni++] = (uint16_t)(base + tri[k]);
}

static void mb_free(MeshBuf *m) {
    free(m->v);
    free(m->ix);
    memset(m, 0, sizeof(*m));
}

/* Upright disc (faces along +-Y), centred on the origin. */
static void mb_cylinder(MeshBuf *m, float r, float half_t, const float face[3], const float rim[3]) {
    enum { SEG = 20 };
    mb_reserve(m, SEG * 6 + 2, SEG * 12);
    for (int side = -1; side <= 1; side += 2) {   /* the two faces: triangle fans */
        float n[3] = { 0, (float)side, 0 };
        int centre = m->nv;
        mb_vert(m, 0, side * half_t, 0, n, face);
        for (int k = 0; k < SEG; k++) {
            float a = 6.2831853f * k / SEG;
            mb_vert(m, r * cosf(a), side * half_t, r * sinf(a), n, face);
        }
        for (int k = 0; k < SEG; k++) {
            int a = centre + 1 + k, b = centre + 1 + (k + 1) % SEG;
            m->ix[m->ni++] = (uint16_t)centre;
            m->ix[m->ni++] = (uint16_t)(side > 0 ? b : a);
            m->ix[m->ni++] = (uint16_t)(side > 0 ? a : b);
        }
    }
    int base = m->nv;                              /* rim: smooth normals */
    for (int k = 0; k < SEG; k++) {
        float a = 6.2831853f * k / SEG, c = cosf(a), s = sinf(a);
        float n[3] = { c, 0, s };
        mb_vert(m, r * c, -half_t, r * s, n, rim);
        mb_vert(m, r * c, half_t, r * s, n, rim);
    }
    for (int k = 0; k < SEG; k++) {
        int a0 = base + k * 2, a1 = a0 + 1;
        int b0 = base + ((k + 1) % SEG) * 2, b1 = b0 + 1;
        m->ix[m->ni++] = (uint16_t)a0; m->ix[m->ni++] = (uint16_t)b0; m->ix[m->ni++] = (uint16_t)b1;
        m->ix[m->ni++] = (uint16_t)a0; m->ix[m->ni++] = (uint16_t)b1; m->ix[m->ni++] = (uint16_t)a1;
    }
}

static void ensure_coin_mesh(void) {
    if (s_coin_mesh) return;
    MeshBuf m = { 0 };
    static const float gold[3]  = { 1.00f, 0.78f, 0.10f };
    static const float rim[3]   = { 0.85f, 0.55f, 0.05f };
    static const float inner[3] = { 1.00f, 0.92f, 0.35f };
    mb_cylinder(&m, 1.0f, 0.12f, gold, rim);       /* unit radius; scaled at draw */
    mb_cylinder(&m, 0.62f, 0.17f, inner, rim);     /* raised centre, MK64-style */
    s_coin_mesh = scene3d_mesh_create(m.v, m.nv, m.ix, m.ni);
    mb_free(&m);
}

/* ---- tilemap scan ---- */

static bool hidden_beh(uint8_t beh) {
    return (beh == SMK_TILE_COIN && s_coins3d) ||
           (beh == SMK_TILE_ITEM_PANEL && neo_track_hides_panels()) ||
           (beh == SMK_TILE_BARRIER && s_walls3d);   /* the flat art under 3D walls */
}

/* Road tile to show instead of a coin / panel. Road art is often striped
 * (the start grid), so take the nearest visible tiles left / right and
 * up / down: if a pair agrees, the cell lies inside that stripe; otherwise
 * use the nearest one. */
static bool visible_tile(int x, int y, uint8_t *t) {
    if (x < 0 || y < 0 || x > 127 || y > 127) return false;
    *t = s_tile[y * 128 + x];
    uint8_t b = s_beh[*t];
    return b != SMK_TILE_COIN && b != SMK_TILE_ITEM_PANEL && b != SMK_TILE_BARRIER &&
           !s_was_panel[y * 128 + x];
}

static uint8_t replacement_tile(int tx, int ty) {
    static const int dir[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    uint8_t t[4];
    int dist[4];
    for (int d = 0; d < 4; d++) {
        dist[d] = 99;
        for (int k = 1; k <= 6; k++) {
            if (visible_tile(tx + dir[d][0] * k, ty + dir[d][1] * k, &t[d])) { dist[d] = k; break; }
        }
    }
    bool h = dist[0] < 99 && dist[1] < 99 && t[0] == t[1];
    bool v = dist[2] < 99 && dist[3] < 99 && t[2] == t[3];
    if (h && v) return dist[0] + dist[1] <= dist[2] + dist[3] ? t[0] : t[2];
    if (h) return t[0];
    if (v) return t[2];
    int best = -1;
    for (int d = 0; d < 4; d++) if (dist[d] < 99 && (best < 0 || dist[d] < dist[best])) best = d;
    return best >= 0 ? t[best] : s_tile[ty * 128 + tx];
}

static void rebuild(void) {
    s_ncoins = 0;
    bool any = false;
    for (int i = 0; i < 128 * 128; i++) {
        s_override[i] = 0xFFFF;
        uint8_t beh = s_beh[s_tile[i]];
        if (beh == SMK_TILE_COIN && s_ncoins < MAX_COINS) {
            s_coin[s_ncoins].x = (i & 127) * 8.0f + 4.0f;
            s_coin[s_ncoins].y = (i >> 7) * 8.0f + 4.0f;
            s_ncoins++;
        }
        /* a used panel becomes a dimmed "used" tile with road behaviour */
        if (hidden_beh(beh) || (s_was_panel[i] && neo_track_hides_panels())) {
            s_override[i] = replacement_tile(i & 127, i >> 7);
            any = true;
        }
    }
    s_override_on = any;
}

/* Called once per frame after it ran: rescan the tilemap and set the
 * override for the next frame. */
void neo_track_update(bool in_race) {
    static int last_coins = -1, last_panels = -1, last_walls = -1;
    int hide_panels = neo_track_hides_panels();
    neo_overlay_sync(in_race);
    if (!in_race) {
        if (s_valid) { s_valid = false; s_walls_dirty = true; }
        snesrecomp_set_m7_override(NULL);
        return;
    }
    uint8_t tile[128 * 128], beh[256];
    for (int i = 0; i < 128 * 128; i++) tile[i] = (uint8_t)snesrecomp_read_vram((uint16_t)i);
    for (int t = 0; t < 256; t++) beh[t] = bus_wram_read8(SMK_RAM_TILE_BEHAVIOUR + t);
    bool changed = !s_valid || memcmp(tile, s_tile, sizeof(tile)) != 0 ||
                   memcmp(beh, s_beh, sizeof(beh)) != 0;
    if (!s_valid || memcmp(beh, s_beh, sizeof(beh)) != 0)    /* new track */
        memset(s_was_panel, 0, sizeof(s_was_panel));
    for (int i = 0; i < 128 * 128; i++)
        if (beh[tile[i]] == SMK_TILE_ITEM_PANEL) s_was_panel[i] = 1;
    if (changed || last_coins != s_coins3d || last_panels != hide_panels || last_walls != s_walls3d) {
        /* walls only change with the barrier layout */
        if (!s_valid || memcmp(beh, s_beh, sizeof(beh)) != 0) s_walls_dirty = true;
        else {
            for (int i = 0; i < 128 * 128; i++) {
                bool a = s_beh[s_tile[i]] == SMK_TILE_BARRIER, b = beh[tile[i]] == SMK_TILE_BARRIER;
                if (a != b || (a && s_tile[i] != tile[i])) { s_walls_dirty = true; break; }
            }
        }
        memcpy(s_tile, tile, sizeof(tile));
        memcpy(s_beh, beh, sizeof(beh));
        s_valid = true;
        last_coins = s_coins3d;
        last_panels = hide_panels;
        last_walls = s_walls3d;
        rebuild();
    }
    snesrecomp_set_m7_override(s_override_on ? s_override : NULL);
}

/* ---- walls ---- */

/* Average colour of a Mode-7 character through the current palette. */
static void tile_colour(uint8_t t, float out[3]) {
    float r = 0, g = 0, b = 0;
    for (int p = 0; p < 64; p++) {
        uint8_t ci = (uint8_t)(snesrecomp_read_vram((uint16_t)(t * 64 + p)) >> 8);
        uint16_t c = snesrecomp_read_cgram(ci);
        r += (c & 31) / 31.0f; g += ((c >> 5) & 31) / 31.0f; b += ((c >> 10) & 31) / 31.0f;
    }
    out[0] = r / 64.0f; out[1] = g / 64.0f; out[2] = b / 64.0f;
}

static bool is_wall(int tx, int ty) {
    if (tx < 0 || ty < 0 || tx > 127 || ty > 127) return false;
    return s_beh[s_tile[ty * 128 + tx]] == SMK_TILE_BARRIER;
}

static void build_walls(void) {
    for (int i = 0; i < s_nwall_meshes; i++) scene3d_mesh_destroy(s_wall_mesh[i]);
    s_nwall_meshes = 0;
    s_walls_dirty = false;
    if (!s_valid) return;

    float colour[256][3];
    bool have[256] = { false };
    MeshBuf m = { 0 };
    const float H = WALL_HEIGHT, S = WALL_STUD;
    for (int ty = 0; ty < 128; ty++) {
        for (int tx = 0; tx < 128; tx++) {
            if (!is_wall(tx, ty)) continue;
            uint8_t t = s_tile[ty * 128 + tx];
            if (!have[t]) { tile_colour(t, colour[t]); have[t] = true; }
            const float *c = colour[t];
            /* brighten: the tile average includes the dark outline */
            float top[3] = { fminf(c[0] * 1.35f, 1.0f), fminf(c[1] * 1.35f, 1.0f), fminf(c[2] * 1.35f, 1.0f) };
            float side[3] = { c[0] * 0.9f, c[1] * 0.9f, c[2] * 0.9f };
            if (m.nv + 32 > WALL_MESH_VERTS) {
                if (s_nwall_meshes < MAX_WALL_MESHES)
                    s_wall_mesh[s_nwall_meshes++] = scene3d_mesh_create(m.v, m.nv, m.ix, m.ni);
                m.nv = m.ni = 0;
            }
            if (!mb_reserve(&m, 32, 48)) { mb_free(&m); return; }
            float x0 = tx * 8.0f, y0 = ty * 8.0f, x1 = x0 + 8.0f, y1 = y0 + 8.0f;
            float cx = x0 + 4.0f, cy = y0 + 4.0f;
            /* studded top: a low pyramid like the tile art */
            const float apex[3] = { cx, cy, H + S };
            const float k00[3] = { x0, y0, H }, k10[3] = { x1, y0, H };
            const float k11[3] = { x1, y1, H }, k01[3] = { x0, y1, H };
            mb_tri_flat(&m, k00, k10, apex, top);
            mb_tri_flat(&m, k10, k11, apex, top);
            mb_tri_flat(&m, k11, k01, apex, top);
            mb_tri_flat(&m, k01, k00, apex, top);
            /* sides facing open ground */
            if (!is_wall(tx, ty - 1)) {      /* -Y */
                const float p[4][3] = { { x0, y0, 0 }, { x1, y0, 0 }, { x1, y0, H }, { x0, y0, H } };
                const float n[3] = { 0, -1, 0 };
                mb_quad(&m, p, n, side);
            }
            if (!is_wall(tx, ty + 1)) {      /* +Y */
                const float p[4][3] = { { x1, y1, 0 }, { x0, y1, 0 }, { x0, y1, H }, { x1, y1, H } };
                const float n[3] = { 0, 1, 0 };
                mb_quad(&m, p, n, side);
            }
            if (!is_wall(tx - 1, ty)) {      /* -X */
                const float p[4][3] = { { x0, y1, 0 }, { x0, y0, 0 }, { x0, y0, H }, { x0, y1, H } };
                const float n[3] = { -1, 0, 0 };
                mb_quad(&m, p, n, side);
            }
            if (!is_wall(tx + 1, ty)) {      /* +X */
                const float p[4][3] = { { x1, y0, 0 }, { x1, y1, 0 }, { x1, y1, H }, { x1, y0, H } };
                const float n[3] = { 1, 0, 0 };
                mb_quad(&m, p, n, side);
            }
        }
    }
    if (m.nv > 0 && s_nwall_meshes < MAX_WALL_MESHES)
        s_wall_mesh[s_nwall_meshes++] = scene3d_mesh_create(m.v, m.nv, m.ix, m.ni);
    mb_free(&m);
}

/* ---- drawing ---- */

static int by_depth_desc(const void *a, const void *b) {
    float da = ((const Coin *)a)->depth, db = ((const Coin *)b)->depth;
    return (da < db) - (da > db);
}

void neo_track_draw(const Scene3DFrame *f, const M7View *v, const NeoWorld *w, float time_s) {
    if (!s_valid) return;
    if (s_walls3d && s_walls_dirty) build_walls();
    bool walls = s_walls3d && s_nwall_meshes > 0;
    bool coins = s_coins3d && s_ncoins > 0;
    if (!walls && !coins) return;
    if (coins) ensure_coin_mesh();
    static const float light[3] = { 0.35f, 0.55f, -1.0f };
    static float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

    int nchase = 0;
    for (int bi = 0; bi < v->nbands; bi++) {
        const M7Band *b = &v->band[bi];
        if (!b->chase) continue;
        nchase++;
        int l0, l1;
        if (!neo_band_lines(v, b, 0, &l0, &l1)) continue;
        float clip[16];
        m7view_clip_matrix(v, b, M7VIEW_NEAR_Z, M7VIEW_FAR_Z, clip);
        scene3d_clip_lines(f, l0, l1);
        scene3d_begin(clip, light);
        float br = b->brightness;
        if (walls) {
            float tint[4] = { br, br, br, 1.0f };
            for (int i = 0; i < s_nwall_meshes; i++) scene3d_draw(s_wall_mesh[i], ident, tint);
        }
        if (coins) {
            Coin vis[MAX_COINS];
            int nvis = 0;
            for (int i = 0; i < s_ncoins; i++) {
                float dx = s_coin[i].x - b->cam[0], dy = s_coin[i].y - b->cam[1];
                float depth = dx * b->fwd[0] + dy * b->fwd[1];
                float lat = dx * b->right[0] + dy * b->right[1];
                if (depth < M7VIEW_NEAR_Z + COIN_RADIUS || depth > 900.0f) continue;
                if (fabsf(lat) > depth * 1.2f + COIN_RADIUS) continue;
                vis[nvis] = s_coin[i];
                vis[nvis].depth = depth;
                nvis++;
            }
            qsort(vis, (size_t)nvis, sizeof(Coin), by_depth_desc);
            for (int i = 0; i < nvis; i++)
                scene3d_blob_shadow(vis[i].x, vis[i].y, COIN_RADIUS * 0.7f, 0.35f * br);
            float tint[4] = { 1.25f * br, 1.25f * br, 1.25f * br, 1.0f };
            for (int i = 0; i < nvis; i++) {
                float phase = vis[i].x * 0.043f + vis[i].y * 0.029f;
                float z = COIN_HOVER + sinf(time_s * 3.0f + phase) * 0.5f;
                float model[16];
                scene3d_mat_spin(model, vis[i].x, vis[i].y, z, time_s * 3.2f + phase, 0.0f, COIN_RADIUS);
                scene3d_draw(s_coin_mesh, model, tint);
            }
        }
        scene3d_end();
    }
}

bool neo_track_coins_enabled(void) { return s_coins3d != 0; }
bool neo_track_walls_enabled(void) { return s_walls3d != 0; }

void neo_track_register_options(void) {
    menu_overlay_add_option("NeoSMK", "3D coins", "neo_coins3d", &s_coins3d, 1, NULL,
                            "Spinning Mario Kart 64-style coins instead of the flat coin\n"
                            "tiles (visual only; a coin disappears once collected).");
    menu_overlay_add_option("NeoSMK", "3D walls", "neo_walls3d", &s_walls3d, 1, NULL,
                            "Raise the track's barrier blocks into 3D walls (visual only;\n"
                            "collisions are unchanged).");
}

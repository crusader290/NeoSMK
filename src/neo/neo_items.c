/*
 * neo_items.c — Mario Kart 64 item variants on top of SMK's item boxes.
 *
 * SMK hands player 1 one item per box (WRAM $0D70 id, $0D71 box state). When
 * the roulette settles, NeoSMK may upgrade the item to an MK64 variant:
 *
 *   mushroom     -> triple mushroom (3 uses) or golden mushroom (any number
 *                   of uses for 7.5 s after the first)
 *   green shell  -> triple green shells
 *   red shell    -> triple red shells
 *   banana       -> banana bunch (5)
 *
 * After each use (the box empties while A or L is pressed) the same item is
 * put back as ready while uses remain; losing the item to a hit ends the
 * variant. SMK only draws the box picture when the roulette settles, so the
 * picture is cached per item from the HUD layer and drawn over the empty box,
 * with a dot per use left.
 *
 * Gameplay change, deterministic: decisions use emulated state only, so
 * netplay peers stay in sync (the option is shared from the host).
 */
#include "neo.h"

#include <snesrecomp/bus.h>
#include <snesrecomp/input.h>
#include <snesrecomp/scene3d.h>
#include <snesrecomp/snesrecomp.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GOLDEN_FRAMES   450     /* 7.5 s at 60 Hz */
#define USE_WINDOW      3       /* frames an A / L press counts as a use */
#define ICON_W          40
#define ICON_H          40
#define N_ITEMS         16

enum { V_NONE, V_TRIPLE_MUSHROOM, V_GOLDEN, V_TRIPLE_GREEN, V_TRIPLE_RED, V_BUNCH };

static struct {
    int      variant;
    int      uses_left;       /* extra uses after the one in hand */
    int      golden_left;     /* frames; -1 = not started */
    int      item;
    uint8_t  last_state;
    int      last_press;      /* frame of the last A / L press */
    bool     refilled;        /* the ready item is NeoSMK's (box picture is ours) */
    uint32_t rng;
} s;

static uint32_t s_epoch = 0xFFFFFFFFu;

/* cached box pictures, by item id */
static uint8_t     s_icon[N_ITEMS][ICON_W * ICON_H * 4];
static int         s_icon_w[N_ITEMS], s_icon_h[N_ITEMS];
static bool        s_icon_ok[N_ITEMS];
static int         s_capture_in = -1;     /* frames until capturing the shown item */
static Scene3DTex *s_tex = NULL;
static int         s_tex_item = -1;

static uint32_t rnd(void) {
    /* LCG mixed with the game's frame counter: deterministic on every peer */
    s.rng = s.rng * 1103515245u + 12345u + bus_wram_read16(0x0064);
    return (s.rng >> 8) % 100;
}

static void roll(int item) {
    s.variant = V_NONE;
    s.uses_left = 0;
    s.golden_left = -1;
    uint32_t r = rnd();
    /* SMK_ITEM_ROLL=<0..99> (testing): fixed roll, e.g. 0 = always the variant */
    const char *fr = getenv("SMK_ITEM_ROLL");
    if (fr) r = (uint32_t)atoi(fr);
    /* SMK_ITEM_TRIPLE_ANY=1 (testing): every item comes as a triple */
    if (getenv("SMK_ITEM_TRIPLE_ANY")) { s.variant = V_TRIPLE_GREEN; s.uses_left = 2; return; }
    switch (item) {
    case SMK_ITEM_MUSHROOM:
        if (r < 35)      { s.variant = V_TRIPLE_MUSHROOM; s.uses_left = 2; }
        else if (r < 45) { s.variant = V_GOLDEN; }
        break;
    case SMK_ITEM_GREEN_SHELL:
        if (r < 30) { s.variant = V_TRIPLE_GREEN; s.uses_left = 2; }
        break;
    case SMK_ITEM_RED_SHELL:
        if (r < 25) { s.variant = V_TRIPLE_RED; s.uses_left = 2; }
        break;
    case SMK_ITEM_BANANA:
        if (r < 25) { s.variant = V_BUNCH; s.uses_left = 4; }
        break;
    }
}

static void refill(void) {
    bus_wram_write8(SMK_RAM_P1_ITEM, (uint8_t)s.item);
    bus_wram_write8(SMK_RAM_P1_ITEM_STATE, SMK_ITEM_READY);
    s.refilled = true;
}

void neo_items_tick(bool enabled, int frame) {
    if (snesrecomp_state_epoch() != s_epoch || !enabled) {
        s_epoch = snesrecomp_state_epoch();
        uint32_t keep = s.rng;
        memset(&s, 0, sizeof(s));
        s.rng = keep ? keep : 0x1234567u;
        s.golden_left = -1;
        s.last_state = bus_wram_read8(SMK_RAM_P1_ITEM_STATE);
        if (!enabled) return;
    }
    uint16_t pad = recomp_input_read_joypad(0);
    if (pad & (SNES_BTN_A | SNES_BTN_L)) s.last_press = frame;

    uint8_t st = bus_wram_read8(SMK_RAM_P1_ITEM_STATE);
    int item = bus_wram_read8(SMK_RAM_P1_ITEM);

    if (st == SMK_ITEM_READY && s.last_state != SMK_ITEM_READY) {   /* new item from a box */
        s.item = item;
        s.refilled = false;
        roll(item);
        if (item >= 0 && item < N_ITEMS) s_capture_in = 4;          /* once the box is drawn */
    } else if (s.last_state == SMK_ITEM_READY && st == 0x00) {     /* item gone */
        bool used = frame - s.last_press <= USE_WINDOW;
        if (used && s.variant == V_GOLDEN) {
            if (s.golden_left < 0) s.golden_left = GOLDEN_FRAMES;
            if (s.golden_left > 0) refill();
        } else if (used && s.uses_left > 0) {
            s.uses_left--;
            refill();
        } else {
            s.variant = V_NONE;          /* last one used, or lost to a hit */
            s.uses_left = 0;
            s.refilled = false;
        }
        st = bus_wram_read8(SMK_RAM_P1_ITEM_STATE);
    } else if (st != SMK_ITEM_READY && st != 0x00) {
        s.variant = V_NONE;              /* a new roulette */
        s.refilled = false;
    }
    if (s.variant == V_GOLDEN && s.golden_left > 0 && --s.golden_left == 0) {
        /* time's up: the golden mushroom in hand goes away */
        if (bus_wram_read8(SMK_RAM_P1_ITEM_STATE) == SMK_ITEM_READY && s.refilled)
            bus_wram_write8(SMK_RAM_P1_ITEM_STATE, 0x00);
        s.variant = V_NONE;
        s.refilled = false;
        st = 0x00;
    }
    s.last_state = st;
}

/* ---- HUD ---- */

static float lum(const uint8_t *p) { return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }

/* Copy the item box (the left part of the HUD BG box, up to the first empty
 * column) as the picture of `item`. */
static void capture_icon(int item) {
    const SnesHudFrame *hf = snesrecomp_hud();
    int cols, lines;
    const uint8_t *px = snesrecomp_hud_pixels(&cols, &lines);
    if (!px || !hf->box[SNES_HUD_BG].present) return;
    int x0 = hf->box[SNES_HUD_BG].x0, y0 = hf->box[SNES_HUD_BG].y0, y1 = hf->box[SNES_HUD_BG].y1;
    int x1 = x0;
    while (x1 < cols && x1 - x0 < ICON_W) {
        bool any = false;
        for (int y = y0; y <= y1 && !any; y++) any = (px[(y * cols + x1) * 4 + 3] & 15) == SNES_HUD_BG;
        if (!any && x1 > x0 + 4) break;
        x1++;
    }
    int w = x1 - x0, h = y1 - y0 + 1;
    if (w < 8 || h < 8) return;
    if (h > ICON_H) h = ICON_H;
    uint8_t *dst = s_icon[item];
    memset(dst, 0, sizeof(s_icon[item]));
    int bright = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *p = &px[((y0 + y) * cols + x0 + x) * 4];
            uint8_t *o = &dst[(y * ICON_W + x) * 4];
            if ((p[3] & 15) != SNES_HUD_BG) continue;
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = 255;
            if (lum(p) > 40.0f) bright++;
        }
    if (bright < 8) return;               /* the box was blank */
    s_icon_w[item] = w;
    s_icon_h[item] = h;
    s_icon_ok[item] = true;
    if (s_tex_item == item) s_tex_item = -1;
}

void neo_items_draw_hud(float x, float y, float px_w, float px_h) {
    if (s_capture_in >= 0 && s_capture_in-- == 0 && !s.refilled) capture_icon(s.item);
    bool ready = bus_wram_read8(SMK_RAM_P1_ITEM_STATE) == SMK_ITEM_READY;
    if (s.variant == V_NONE || !ready) return;
    int it = s.item;
    if (s.refilled && it >= 0 && it < N_ITEMS && s_icon_ok[it]) {
        if (!s_tex) s_tex = scene3d_tex_rgba(ICON_W, ICON_H);
        if (s_tex_item != it) { scene3d_tex_update_rgba(s_tex, s_icon[it]); s_tex_item = it; }
        float uv0[2] = { 0, 0 };
        float uv1[2] = { (float)s_icon_w[it] / ICON_W, (float)s_icon_h[it] / ICON_H };
        scene3d_ui_image(s_tex, x, y, s_icon_w[it] * px_w, s_icon_h[it] * px_h, uv0, uv1, NULL);
    }
    /* uses left: a dot each under the box; golden: a gold dot that shrinks */
    float bw = (s_icon_ok[it] ? s_icon_w[it] : 24) * px_w;
    float bh = (s_icon_ok[it] ? s_icon_h[it] : 24) * px_h;
    float r = px_h * 2.2f;
    float cy = y + bh + r * 1.6f;
    static const float ring[4] = { 0.05f, 0.05f, 0.08f, 0.9f };
    if (s.variant == V_GOLDEN) {
        static const float gold[4] = { 1.0f, 0.80f, 0.15f, 1.0f };
        float f = s.golden_left < 0 ? 1.0f : (float)s.golden_left / GOLDEN_FRAMES;
        scene3d_ui_disc(x + bw * 0.5f, cy, r * (0.6f + 0.6f * f), gold, ring, 1.5f);
        return;
    }
    int n = s.uses_left + 1;
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float step = r * 2.6f, x0 = x + bw * 0.5f - step * (n - 1) * 0.5f;
    for (int i = 0; i < n; i++) scene3d_ui_disc(x0 + step * i, cy, r, white, ring, 1.5f);
}

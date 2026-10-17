/*
 * neo.h — NeoSMK layer: game-aware features on top of the recompiled game.
 *
 * Everything here is presentation only unless stated: it reads WRAM and the
 * reconstructed Mode-7 camera, never writes emulated state, so netplay,
 * save-states and the snapshot-diff harness are unaffected.
 */
#ifndef NEOSMK_NEO_H
#define NEOSMK_NEO_H

#include <stdbool.h>
#include <stdint.h>

#include "smk_ram.h"

#include <snesrecomp/m7view.h>
#include <snesrecomp/scene3d.h>

typedef struct {
    float    x, y;        /* map position (with subpixel) */
    float    vx, vy;      /* velocity, map pixels per frame */
    float    heading;     /* radians, 0 = north (-Y), clockwise */
    uint16_t heading_raw;
    int      character;   /* 0..7 */
    float    speed;       /* map pixels per frame */
} NeoRacer;

typedef struct {
    bool     in_race;
    NeoRacer racer[SMK_RACER_COUNT];
} NeoWorld;

/* Character colours (Mario, Luigi, Bowser, Peach, DK Jr, Koopa, Toad, Yoshi). */
extern const float neo_char_color[8][3];

/* Snapshot the game state needed by the overlays (call at present time). */
void neo_world_read(NeoWorld *w);

/* Register menu options. Call before snesrecomp_init. */
void neo_register_options(void);

/* Gameplay tick: once per logic frame, after the frame ran. */
void neo_tick(void);
void neo_gameplay_register_options(void);
/* Mini-turbo state for drawing sparks: stage 0 none, 1 yellow, 2 orange. */
void neo_gameplay_spark_state(int player, int *stage, int *dir, int *boosting);

/* Scanlines of the presented picture a band's 3D may draw on (down from the
 * top of the player's view, so objects near the horizon can stick up past
 * it); `overhang` extra lines below the band in the native layout (karts poke
 * out a little). Returns false if the band isn't presented. */
static inline bool neo_band_lines(const M7View *v, const M7Band *b, int overhang,
                                  int *l0, int *l1) {
    /* from the top of this view: 3D near the horizon sticks up over the
     * scenery strip (full-screen: the real picture's first visible line;
     * a lower split band: below the band above it) */
    int idx = (int)(b - v->band);
    if (v->layout == M7_LAYOUT_FULLSCREEN) *l0 = v->top_skip + 1 + v->line_offset;
    else if (idx <= 0) *l0 = 1;
    else {
        *l0 = b->draw_line0 - 40;
        if (*l0 <= v->band[idx - 1].line1) *l0 = v->band[idx - 1].line1 + 1;
    }
    if (*l0 > b->draw_line0) *l0 = b->draw_line0;
    *l1 = b->draw_line1 + (v->layout == M7_LAYOUT_NATIVE ? overhang : 0);
    if (*l1 > v->lines) *l1 = v->lines;
    return *l1 >= *l0;
}

/* 3D item boxes over the "?" panels (draws into the current scene hook). */
void neo_itembox_draw(const Scene3DFrame *f, const M7View *v, float time_s);

/* Circuit map in the full-screen 1-player layout: right-aligned at `right`,
 * bottom at `bottom` (target px), at most max_h tall. */
void neo_minimap_draw(const Scene3DFrame *f, const M7View *v, const NeoWorld *w,
                      float right, float bottom, float max_h);

/* Track objects in 3D (coins, walls) and the Mode-7 override that hides the
 * flat coin / panel graphics they replace. */
void neo_track_register_options(void);
void neo_track_update(bool in_race);          /* once per frame, after it ran */
void neo_track_draw(const Scene3DFrame *f, const M7View *v, const NeoWorld *w, float time_s);
bool neo_track_coins_enabled(void);
bool neo_track_walls_enabled(void);
bool neo_track_hides_panels(void);            /* = 3D item boxes on (neo_overlay.c) */
/* Sprite layer on/off for the 3D layer (neo_overlay.c); once per frame. */
void neo_overlay_sync(bool in_race);

/* 3D warp pipes (their sprites are hidden before each frame). */
void neo_pipes_register_options(void);
void neo_pipes_install(void);
void neo_pipes_draw(const Scene3DFrame *f, const M7View *v, const NeoWorld *w);

/* Full-screen layout: other karts below SMK's race view, as billboards of
 * their last captured sprite. */
void neo_karts_draw(const Scene3DFrame *f, const M7View *v, const NeoWorld *w);
/* Picture box (x0, y0, x1, y1) and bottom line of the kart sprite standing at
 * (col, line), from its OAM block in the sprite layer; false if not found. */
bool neo_karts_sprite_box(float col, float line, float px_per_unit, float box[4], float *bottom);

/* MK64 item variants (gameplay, player 1): once per logic tick. */
void neo_items_tick(bool enabled, int frame);
/* Draw the item box picture (when NeoSMK refilled the item) and the uses
 * left, given where the HUD BG box (timer + item box) lands: its top-left in
 * target px and the size of one HUD pixel. Inside a scene3d_ui block. */
void neo_items_draw_hud(float x, float y, float px_w, float px_h);

/* Install the 3D overlay hook. Call after snesrecomp_init. */
void neo_install(void);

#endif /* NEOSMK_NEO_H */

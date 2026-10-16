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

/* Scanlines of the presented picture a band's 3D may draw on; `overhang`
 * extra lines below the band in the native layout (karts poke out a little).
 * Returns false if the band isn't presented. */
static inline bool neo_band_lines(const M7View *v, const M7Band *b, int overhang,
                                  int *l0, int *l1) {
    *l0 = b->draw_line0;
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

/* Install the 3D overlay hook. Call after snesrecomp_init. */
void neo_install(void);

#endif /* NEOSMK_NEO_H */

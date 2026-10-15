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

/* Snapshot the game state needed by the overlays (call at present time). */
void neo_world_read(NeoWorld *w);

/* Register menu options. Call before snesrecomp_init. */
void neo_register_options(void);

/* Install the 3D overlay hook. Call after snesrecomp_init. */
void neo_install(void);

#endif /* NEOSMK_NEO_H */

/*
 * neo_gameplay.c — NeoSMK gameplay changes, applied between frames.
 *
 * Runs once per logic tick after the frame, from emulated state only (WRAM
 * and the auto-joypad registers, which netplay keeps in lockstep), so every
 * peer and every replay makes the same decisions. Local state is dropped
 * whenever emulated state jumps (reset / load-state / netplay sync).
 *
 * Netplay: both peers must use the same NeoSMK options.
 */
#include "neo.h"

#include <snesrecomp/bus.h>
#include <snesrecomp/input.h>
#include <snesrecomp/menu_overlay.h>
#include <snesrecomp/snesrecomp.h>
#include <stddef.h>
#include <string.h>

/* ---- options ---- */
static int s_miniturbo = 1;

/* ---- Mario Kart 64 mini-turbo ----
 * While power-sliding (hop + R), flick the D-pad toward the outside of the
 * turn: 1 flick = yellow sparks, 3 flicks = orange. Releasing the slide on
 * orange gives a short boost (+25% over the pre-slide speed) that SMK's own
 * physics then bleeds off like a mushroom. */
#define MT_FLICKS_YELLOW   1
#define MT_FLICKS_ORANGE   3
#define MT_FLICK_GAP       4        /* frames between counted flicks */
#define MT_BOOST_FRAMES    30
#define MT_BOOST_NUM       5        /* target = 5/4 x pre-slide speed */
#define MT_BOOST_DEN       4
#define MT_BOOST_CAP       0x0500

#define HUMAN_FLAG         0x8000   /* racer +$10 on human players */

typedef struct {
    bool     sliding;
    int      dir;          /* -1 left, +1 right, 0 = no charge this slide */
    int      flicks;
    int      last_flick;   /* frame of the last counted flick */
    bool     outside_held;
    uint16_t pre_speed;    /* speed when the slide began */
    int      boost_left;   /* frames of boost remaining */
    uint16_t boost_target;
} MiniTurbo;

static MiniTurbo s_mt[2];
static int       s_frame = 0;
static uint32_t  s_epoch = 0xFFFFFFFFu;

static int mt_stage(const MiniTurbo *m) {
    if (!m->sliding || m->dir == 0) return 0;
    if (m->flicks >= MT_FLICKS_ORANGE) return 2;
    if (m->flicks >= MT_FLICKS_YELLOW) return 1;
    return 0;
}

static void mini_turbo(int player, uint32_t rec) {
    MiniTurbo *m = &s_mt[player];
    uint16_t pad = recomp_input_read_joypad(player);
    uint16_t speed = bus_wram_read16(rec + SMK_RACER_SPEED);
    bool sliding = bus_wram_read16(rec + SMK_RACER_SLIDE_TIME) != 0;
    int steer = (pad & SNES_BTN_LEFT) ? -1 : (pad & SNES_BTN_RIGHT) ? 1 : 0;

    if (sliding && !m->sliding) {                    /* slide starts */
        m->sliding = true;
        m->dir = steer;
        m->flicks = 0;
        m->last_flick = -1000;
        m->outside_held = false;
        m->pre_speed = speed;
    } else if (sliding) {                            /* charging */
        bool outside = m->dir != 0 && steer == -m->dir;
        if (outside && !m->outside_held && s_frame - m->last_flick >= MT_FLICK_GAP) {
            m->flicks++;
            m->last_flick = s_frame;
        }
        m->outside_held = outside;
    } else if (m->sliding) {                         /* slide released */
        if (mt_stage(m) == 2) {
            uint32_t base = m->pre_speed > speed ? m->pre_speed : speed;
            uint32_t target = base * MT_BOOST_NUM / MT_BOOST_DEN;
            m->boost_target = (uint16_t)(target > MT_BOOST_CAP ? MT_BOOST_CAP : target);
            m->boost_left = MT_BOOST_FRAMES;
        }
        m->sliding = false;
        m->dir = 0;
        m->flicks = 0;
    }

    if (m->boost_left > 0) {
        if (speed < m->boost_target) bus_wram_write16(rec + SMK_RACER_SPEED, m->boost_target);
        m->boost_left--;
    }
}

void neo_tick(void) {
    s_frame++;
    if (snesrecomp_state_epoch() != s_epoch) {
        s_epoch = snesrecomp_state_epoch();
        memset(s_mt, 0, sizeof(s_mt));
    }
    neo_track_update(bus_wram_read16(SMK_RAM_GAME_STATE) == 0x0002);   /* presentation */
    if (bus_wram_read16(SMK_RAM_GAME_STATE) != 0x0002) {   /* not racing */
        memset(s_mt, 0, sizeof(s_mt));
        return;
    }
    for (int p = 0; p < 2; p++) {
        uint32_t rec = SMK_RAM_RACERS + (uint32_t)p * SMK_RACER_STRIDE;
        if (!(bus_wram_read16(rec + SMK_RACER_FLAGS) & HUMAN_FLAG)) {
            memset(&s_mt[p], 0, sizeof(s_mt[p]));
            continue;
        }
        if (s_miniturbo) mini_turbo(p, rec);
    }
}

void neo_gameplay_spark_state(int player, int *stage, int *dir, int *boosting) {
    const MiniTurbo *m = &s_mt[player & 1];
    if (stage)    *stage = mt_stage(m);
    if (dir)      *dir = m->dir;
    if (boosting) *boosting = m->boost_left > 0;
}

void neo_gameplay_register_options(void) {
    /* gameplay-changing options: netplay peers use the host's values */
    snesrecomp_netplay_share(&s_miniturbo, (int)sizeof(s_miniturbo));
    menu_overlay_add_option("NeoSMK", "MK64 mini-turbo", "neo_miniturbo", &s_miniturbo, 1, NULL,
                            "Power-slide (hop with R), then flick the D-pad toward the\n"
                            "outside of the turn: yellow sparks, then orange after 3.\n"
                            "Release on orange for a boost. Netplay: the host's setting\n"
                            "is used by both players.");
}

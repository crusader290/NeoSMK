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
/* gameplay options, shared with netplay peers as one block (host's values) */
static struct { int miniturbo; int items; } s_opt = { 1, 1 };
#define s_miniturbo s_opt.miniturbo

/* ---- Mario Kart 64 mini-turbo ----
 * While power-sliding (hop + R), sparks charge up: yellow, then orange.
 * Holding the D-pad into the turn charges fastest, neutral or outward slower,
 * and an MK64 tap (outward, then back into the turn) adds a chunk at once.
 * Releasing the slide on orange gives a short boost (+25% over the
 * pre-slide speed) that SMK's own physics then bleeds off like a mushroom.
 * The turn direction is the one steered at the hop, else the first one
 * steered in the slide, else the way the kart is rotating (heading +$2A). */
#define MT_YELLOW          50       /* charge points */
#define MT_ORANGE          110
#define MT_CHARGE_INTO     2        /* per frame, steering into the turn */
#define MT_CHARGE_OTHER    1        /* per frame, neutral or outward */
#define MT_TAP_BONUS       16       /* outward tap, back into the turn */
#define MT_TAP_WINDOW      12       /* frames from leaving outward to into */
#define MT_TAP_GAP         4        /* frames between counted taps */
#define MT_TURN_DIR        0x0300   /* heading change that sets the turn */
#define MT_BOOST_FRAMES    30
#define MT_BOOST_NUM       5        /* target = 5/4 x pre-slide speed */
#define MT_BOOST_DEN       4
#define MT_BOOST_CAP       0x0500

#define HUMAN_FLAG         0x8000   /* racer +$10 on human players */

typedef struct {
    bool     sliding;
    int      dir;          /* -1 left, +1 right, 0 = not known yet */
    int      charge;
    int      turn;         /* heading change since the slide began */
    uint16_t heading;
    int      last_steer;
    int      left_outside; /* frame the D-pad left the outside, -1000 = no tap */
    int      last_tap;
    uint16_t pre_speed;    /* speed when the slide began */
    int      boost_left;   /* frames of boost remaining */
    uint16_t boost_target;
} MiniTurbo;

static MiniTurbo s_mt[2];
static int       s_frame = 0;
static uint32_t  s_epoch = 0xFFFFFFFFu;

static int mt_stage(const MiniTurbo *m) {
    if (!m->sliding || m->dir == 0) return 0;
    if (m->charge >= MT_ORANGE) return 2;
    if (m->charge >= MT_YELLOW) return 1;
    return 0;
}

static void mini_turbo(int player, uint32_t rec) {
    MiniTurbo *m = &s_mt[player];
    uint16_t pad = recomp_input_read_joypad(player);
    uint16_t speed = bus_wram_read16(rec + SMK_RACER_SPEED);
    uint16_t heading = bus_wram_read16(rec + SMK_RACER_HEADING);
    bool sliding = bus_wram_read16(rec + SMK_RACER_SLIDE_TIME) != 0;
    int steer = (pad & SNES_BTN_LEFT) ? -1 : (pad & SNES_BTN_RIGHT) ? 1 : 0;

    if (sliding && !m->sliding) {                    /* slide starts */
        memset(m, 0, offsetof(MiniTurbo, boost_left));
        m->sliding = true;
        m->dir = steer;
        m->heading = heading;
        m->last_steer = steer;
        m->left_outside = -1000;
        m->last_tap = -1000;
        m->pre_speed = speed;
    } else if (sliding) {                            /* charging */
        m->turn += (int16_t)(uint16_t)(heading - m->heading);   /* clockwise = right */
        m->heading = heading;
        if (m->dir == 0) {
            if (steer != 0) m->dir = steer;
            else if (m->turn >= MT_TURN_DIR) m->dir = 1;
            else if (m->turn <= -MT_TURN_DIR) m->dir = -1;
        }
        if (m->dir != 0) {
            bool into = steer == m->dir, outside = steer == -m->dir;
            m->charge += into ? MT_CHARGE_INTO : MT_CHARGE_OTHER;
            if (m->last_steer == -m->dir && !outside) m->left_outside = s_frame;
            if (into && m->last_steer != m->dir &&
                s_frame - m->left_outside <= MT_TAP_WINDOW && s_frame - m->last_tap >= MT_TAP_GAP) {
                m->charge += MT_TAP_BONUS;
                m->last_tap = s_frame;
                m->left_outside = -1000;
            }
            if (m->charge > MT_ORANGE) m->charge = MT_ORANGE;
        }
        m->last_steer = steer;
    } else if (m->sliding) {                         /* slide released */
        if (mt_stage(m) == 2) {
            uint32_t base = m->pre_speed > speed ? m->pre_speed : speed;
            uint32_t target = base * MT_BOOST_NUM / MT_BOOST_DEN;
            m->boost_target = (uint16_t)(target > MT_BOOST_CAP ? MT_BOOST_CAP : target);
            m->boost_left = MT_BOOST_FRAMES;
        }
        m->sliding = false;
        m->dir = 0;
        m->charge = 0;
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
        neo_items_tick(false, s_frame);
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
    neo_items_tick(s_opt.items != 0, s_frame);
}

void neo_gameplay_spark_state(int player, int *stage, int *dir, int *boosting) {
    const MiniTurbo *m = &s_mt[player & 1];
    if (stage)    *stage = mt_stage(m);
    if (dir)      *dir = m->dir;
    if (boosting) *boosting = m->boost_left > 0;
}

void neo_gameplay_register_options(void) {
    /* gameplay-changing options: netplay peers use the host's values */
    snesrecomp_netplay_share(&s_opt, (int)sizeof(s_opt));
    menu_overlay_add_option("NeoSMK", "MK64 mini-turbo", "neo_miniturbo", &s_miniturbo, 1, NULL,
                            "Power-slide (hop with R) and hold it: sparks turn yellow,\n"
                            "then orange. Steering into the turn charges faster, and\n"
                            "MK64 taps (outward, then back in) add more. Release on\n"
                            "orange for a boost. Netplay: the host's setting is used.");
    menu_overlay_add_option("NeoSMK", "MK64 items", "neo_items", &s_opt.items, 1, NULL,
                            "Mario Kart 64 item variants: triple mushrooms, golden\n"
                            "mushroom, triple green / red shells and banana bunches come\n"
                            "out of SMK's item boxes too (player 1). Gameplay change;\n"
                            "netplay uses the host's setting.");
}

/*
 * neo_world.c — read racer state out of WRAM (see smk_ram.h).
 */
#include "neo.h"

#include <snesrecomp/bus.h>
#include <math.h>
#include <string.h>

static int16_t s16(uint16_t v) { return (int16_t)v; }

void neo_world_read(NeoWorld *w) {
    memset(w, 0, sizeof(*w));
    w->in_race = bus_wram_read16(SMK_RAM_GAME_STATE) == 0x0002;
    for (int i = 0; i < SMK_RACER_COUNT; i++) {
        uint32_t b = SMK_RAM_RACERS + (uint32_t)i * SMK_RACER_STRIDE;
        NeoRacer *r = &w->racer[i];
        r->x = bus_wram_read16(b + SMK_RACER_X) + (bus_wram_read16(b + SMK_RACER_X_SUB) >> 8) / 256.0f;
        r->y = bus_wram_read16(b + SMK_RACER_Y) + (bus_wram_read16(b + SMK_RACER_Y_SUB) >> 8) / 256.0f;
        r->vx = s16(bus_wram_read16(b + SMK_RACER_VEL_X)) / 256.0f;
        r->vy = s16(bus_wram_read16(b + SMK_RACER_VEL_Y)) / 256.0f;
        r->heading_raw = bus_wram_read16(b + SMK_RACER_HEADING);
        r->heading = r->heading_raw * (6.2831853f / 65536.0f);
        r->character = (bus_wram_read16(b + SMK_RACER_CHARACTER) >> 1) & 7;
        r->speed = bus_wram_read16(b + SMK_RACER_SPEED) / 256.0f;
    }
}

/*
 * smk_ram.h — Super Mario Kart (US) WRAM map used by the NeoSMK layer.
 *
 * Confirmed against live races in this emulator (snapshot diffs while driving
 * and steering; see docs/ram_map.md for the evidence). Offsets marked (?) are
 * unconfirmed. Start points: the Yoshifanatic disassembly's RAM_Map_SMK.asm
 * (player X/Y/speed) and upstream docs/smk_flow_re.md (game state).
 */
#ifndef NEOSMK_SMK_RAM_H
#define NEOSMK_SMK_RAM_H

/* Game-state sequencer: $02 = race, $04 = title, $06 = driver select,
 * $08 = class/cup select. */
#define SMK_RAM_GAME_STATE      0x0036

/* Mode-7 tile behaviour table (256 bytes, one per tile id, set per track). */
#define SMK_RAM_TILE_BEHAVIOUR  0x0B00
#define SMK_TILE_ITEM_PANEL     0x14   /* "?" item panel ($C4-$C7 on Mario Circuit) */
#define SMK_TILE_COIN           0x1A   /* track coin ($FE) */
#define SMK_TILE_ROAD           0x40   /* plain road; used panels become this */
#define SMK_TILE_BARRIER        0x80   /* wall / barrier block ($F0-$F3 on Mario Circuit) */

/* Racer records: 8 x $100 bytes. Record 0 is player 1. */
#define SMK_RAM_RACERS          0x1000
#define SMK_RACER_STRIDE        0x0100
#define SMK_RACER_COUNT         8

/* Offsets within a racer record (16-bit little-endian unless noted). */
#define SMK_RACER_FLAGS         0x10   /* $8000 set on the human player (?) */
#define SMK_RACER_CHARACTER     0x12   /* character id * 2 (0..$0E) */
#define SMK_RACER_X_SUB         0x16   /* X subpixel (high byte) */
#define SMK_RACER_X             0x18   /* X, Mode-7 map pixels 0..1023 */
#define SMK_RACER_Y_SUB         0x1A
#define SMK_RACER_Y             0x1C   /* Y, map pixels, grows down the map */
#define SMK_RACER_VEL_X         0x22   /* signed 8.8 per frame */
#define SMK_RACER_VEL_Y         0x24
#define SMK_RACER_HOP_Z         0x20   /* hop height (integer), +$1E fraction */
#define SMK_RACER_HEADING       0x2A   /* $0000 north, $4000 east, $8000 south */
#define SMK_RACER_SLIDE_STEER   0xC4   /* $8000 + $0100 steering right / $0200 left while sliding */
#define SMK_RACER_SLIDE_TIME    0xCA   /* counts up during a power-slide, 0 otherwise */
#define SMK_RACER_LAP           0xC1   /* 8-bit, player 1 (disassembly) (?) */
#define SMK_RACER_SPEED         0xEA   /* |velocity|, 8.8 */

#endif /* NEOSMK_SMK_RAM_H */

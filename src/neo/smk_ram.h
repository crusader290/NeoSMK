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
#define SMK_RACER_HEADING       0x2A   /* $0000 north, $4000 east, $8000 south */
#define SMK_RACER_LAP           0xC1   /* 8-bit, player 1 (disassembly) (?) */
#define SMK_RACER_SPEED         0xEA   /* |velocity|, 8.8 */

#endif /* NEOSMK_SMK_RAM_H */

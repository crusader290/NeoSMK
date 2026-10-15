# Super Mario Kart (US) RAM map — NeoSMK findings

Addresses used by the NeoSMK layer (`src/neo/smk_ram.h`). Each is confirmed against live
races in this emulator unless marked unconfirmed.

## How these were found

1. Starting points: [Yoshifanatic's disassembly](https://github.com/Yoshifanatic1/Super-Mario-Kart-Disassembly)
   `RAM_Map_SMK.asm` (player 1 X/Y around $1016–$101D, speed $10EA, lap $10C1) and upstream
   `docs/smk_flow_re.md` (game state $36).
2. Scripted races (`SMK_SCRIPT`, `SMK_SNAPSHOT_PREFIX`/`SMK_SNAPSHOT_EVERY`) dump WRAM every
   N frames. Comparing frames on the starting grid, while driving straight, and after steering
   identifies the fields.
3. Cross-check: projecting each racer's position through the camera reconstructed from Mode 7
   (`m7view.h`) lands on its kart sprite (Debug → 3D camera markers).

## Racer table: `$1000 + n * $100`, n = 0..7 (record 0 = player 1)

| Offset | Size | Meaning | Evidence |
|---|---|---|---|
| +$10 | 16 | flags; $8000 set on player 1 | $8002 on record 0 during the race, $2000 on CPU racers |
| +$12 | 16 | character × 2 | values 0, 2, … $0E, one per racer; marker colours match the sprites |
| +$16 | 16 | X subpixel (high byte) | changes every frame while moving |
| +$18 | 16 | X position, Mode-7 map pixels (0–1023) | grid: 56/104/152/200 columns; tracks the sprite |
| +$1A | 16 | Y subpixel (high byte) | as X |
| +$1C | 16 | Y position (grows down the map) | grid rows 112/177 on the menu demo, 588–756 on Mario Circuit 1 |
| +$22 | 16 | X velocity, signed 8.8 per frame | 0 while driving due north |
| +$24 | 16 | Y velocity, signed 8.8 per frame | −$251 driving north at speed $252 |
| +$2A | 16 | heading: $0000 north, $4000 east, $8000 south (clockwise) | $8000 with vx=0, vy>0; $4480 with vx≫0, vy slightly >0 |
| +$C1 | 8 | current lap (player 1) | from the disassembly; **unconfirmed** |
| +$EA | 16 | speed, \|velocity\| in 8.8 | equals the velocity magnitude |

## Game state

| Address | Meaning |
|---|---|
| $36 | sequencer state: $02 race, $04 title, $06 driver select, $08 class/cup select |

## Camera

Not read from RAM. `m7view.c` reconstructs it from the PPU's per-scanline Mode-7 state, so it
always matches what was drawn. On Mario Circuit 1 the 1-player view fits a horizontal-axis pinhole:
horizon at scanline ~22.6, focal length ~270 SNES pixels, camera height ~18.3 map units, sitting
~63 units behind the player. Fit error is ~0.4 px rms vertically and ~1.5 px rms horizontally.
The camera heading lags the kart's heading slightly while turning (the game smooths it).

## Still to find

- item slot and roulette state; item-box positions and their "taken" timers
- power-slide / hop state and the per-frame speed update (for the mini-turbo)
- object table for shells, bananas and other track objects

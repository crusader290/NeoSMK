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
| +$1E / +$20 | 16 | hop height: fraction / integer | 1 → 3 → 1 over the frames after pressing R |
| +$26 | 16 | hop vertical velocity | $00AC, $0010, $FF74 across the hop |
| +$2A | 16 | heading: $0000 north, $4000 east, $8000 south (clockwise) | $8000 with vx=0, vy>0; $4480 with vx≫0, vy slightly >0 |
| +$9E | 16 | power-slide lean (4–7 while sliding) | 0 outside slides |
| +$B2 | 16 | slide angle, ramps to $0B00, decays after the slide | |
| +$C4 | 16 | $8000, plus $0100 while steering right / $0200 left in a slide | flips to $82xx on every outside flick |
| +$CA | 16 | **power-slide timer**: counts up while sliding, 0 otherwise | nonzero for exactly the slide (used by the mini-turbo) |
| +$C1 | 8 | current lap (player 1) | from the disassembly; **unconfirmed** |
| +$EA | 16 | speed, \|velocity\| in 8.8 | equals the velocity magnitude; ~$2A0–$2CE top speed at 50cc. Writing a higher value boosts: the game bleeds it back down (≈$10/frame), like a mushroom |

## Track tiles

| Address | Meaning |
|---|---|
| VRAM words $0000–$3FFF, low byte | Mode-7 tilemap, 128 × 128 tiles of 8 × 8 px |
| WRAM $0B00–$0BFF | tile behaviour table, one byte per tile id (per track) |

Behaviour codes seen on Mario Circuit 1: $40 road (7914 cells), $54, $26, $80, **$14 item
panel** (tiles $C4–$C7 as 2 × 2 groups: top-left, top-right, bottom-left, bottom-right; 12
panels), **$1A coin** (tile $FE, 35 coins). Driving over a panel with no item held rewrites
its four tiles to $D4–$D7 with behaviour $40. With an item held, the panel stays.
CPU racers never consume panels.

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

- item slot and roulette state
- object table for shells, bananas and other track objects

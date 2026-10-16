# Roadmap: Mario Kart 64 systems and 3D objects

Phase 1 (done) ported the recompilation to SDL3 + OpenGL ES 3.0 with a fixed-timestep loop
and widescreen. This document scopes phase 2: bringing over **Mario Kart 64 gameplay
systems** and **replacing selected sprites with real 3D objects**, within the platform targets
(Raspberry Pi 4, GLES 3.0, 640×480-class internal resolution, 60 FPS, < 512 MB).

Status: **B1 camera reconstruction**, the **mini-turbo**, **3D item boxes** and
**full-screen single-player races** with a circuit map are implemented (see below);
the rest is still a plan.

## Ground rules

1. **Game logic stays deterministic.** Netplay, save-states and the snapshot-diff harness
   rely on it. Gameplay changes go through `RECOMP_PATCH` functions or explicit WRAM writes at
   tick boundaries, never through the renderer. Anything visual-only reads WRAM and never
   writes it.
2. **Every gameplay change is optional.** A "Mode: Classic / NeoSMK" switch keeps the original
   game intact.
3. **Prerequisite for most items: RAM map.** We need confirmed WRAM addresses for kart
   position, velocity, heading, drift/hop state, item slot, item roulette and object tables.
   The source is the [Yoshifanatic disassembly](https://github.com/Yoshifanatic1/Super-Mario-Kart-Disassembly),
   cross-checked live with `SMK_TRACE_STATE`-style watches and snapshot diffs. None of these
   addresses are pinned down in this repo yet.

## A. Mario Kart 64 systems

| System | What it involves | Effort |
|---|---|---|
| **Mini-turbo drift** (MK64 yellow → orange sparks) | **Done** (`src/neo/neo_gameplay.c`). While the slide timer (+$CA) runs, flicks of the D-pad to the outside of the turn charge it; releasing on orange holds speed at 125% of the pre-slide speed for 30 frames, then SMK's physics bleeds it off. Sparks and the boost flame are 3D particles. Deterministic, so netplay-safe if both peers enable it. | — |
| **Mirror mode** | Presentation: flip the output horizontally and swap Left/Right input. Needs extra work to un-mirror HUD text (HUD sprites can be drawn from an unflipped pass). | Small for a first version |
| **MK64 item set** (triple shells, spiny/blue shell, fake item box, triple mushroom, golden mushroom, Boo steal) | New behaviours on top of SMK's item engine: roulette table patch + new object handlers. Triple shells/mushrooms reuse existing objects; spiny needs a homing-to-leader routine. | Large; needs item and object RE |
| **Time-trial ghosts** | Record per-tick inputs (deterministic) and replay a second simulation copy, or record kart pose. Render the ghost as a translucent 3D or billboard kart through the scene hook. | Medium; pairs with 3D karts |
| **150cc / extra cups unlock** | Flags in SRAM/WRAM. | Small |
| **Rubber-band / AI tuning, item probabilities by position** | Table patches in ROM data via RECOMP_PATCH. | Small–medium |

## B. 3D objects

The renderer composes into a depth-buffered internal target and exposes
`renderer_set_scene_hook()` (`ext/snesrecomp/src/gfx/renderer.h`). That hook is where 3D draws.

### B1. Camera reconstruction (prerequisite) — done

Implemented in `ext/snesrecomp/src/m7view.c`. The PPU's per-scanline Mode-7 state is fitted to a
horizontal-axis pinhole (≤1.2 px max error vertically, ~1.5 px rms horizontally), with no RAM
reverse-engineering needed. Debug → "3D camera markers" shows a marker over every racer.
The original plan follows:

To place 3D geometry consistently with the Mode-7 floor, we need the projection SMK uses:

- **From game state:** camera position and heading from WRAM (follows the player kart), plus
  SMK's fixed pitch / horizon line / focal length, which can be derived from the per-scanline
  Mode-7 matrix HDMA tables (scale per line ∝ 1/depth).
- **Validation tool:** a debug overlay that draws 3D markers at every kart's world position
  should land exactly on the kart sprites. That proves the camera before any art exists.

### Full-screen single-player races — done

Graphics → Full-screen 1P races. A frame is shown full-screen when its only "chase" Mode-7 band
is at the top: a perspective band with its horizon at the band's top edge. SMK's overview map is
also perspective (slightly tilted) but its horizon is far above it, so it doesn't count; two
chase bands (2-player) keep the native split. The band is drawn 1:1, shifted down so the horizon
sits at ~30% of the screen. The rows below are ray-cast onto the ground plane on the GPU
(`kFloorFS` in `renderer.c`: tilemap → character → palette, like the PPU, with per-line
brightness). The overview map is replaced by a circuit map built from the tile behaviours
(`src/neo/neo_minimap.c`). The 3D layer follows the shifted view through `M7View.line_offset`.

Shifting the band down uncovers lines that the SNES never drew, and the game's scenery strip
and Lakitu were cut at the old top edge. The PPU extends the picture upward (presentation
only, `ppu_setAbove` in LakeSnes): when it reaches the band's first visible line, it renders
up to 64 virtual lines above it with that line's registers. Each BG layer is extended only
until one of its rows is fully transparent, so its wrapped tilemap rows never show. Sprites
count only if they wrap past line 255, which leaves out sprites parked off the bottom and the
lower screen's sprites. HUD layers are left out, and the CPU-visible sprite flags are put back.
The renderer draws these lines directly above the band, and the sky colour fills what's left.

### Mario Kart 8-style HUD — done

In the full-screen layout the PPU separates SMK's HUD from the picture, presentation-only
(`Ppu.hud*` in LakeSnes, `SnesHudConfig` in snesrecomp). Pixels from HUD sprites, a HUD BG layer,
or window-shaped colour maths go to a HUD layer tagged by class, and the picture is
re-composited without them. For SMK (`src/neo/neo_overlay.c`):

| HUD element | How SMK draws it | Placed |
|---|---|---|
| lives, coins | OAM 16–23 (8×8, palette 4, fixed position) | bottom-left, 1.25× |
| timer, lap box | BG2 of the Mode-0 scenery strip (BG3/BG4 are hills and trees) | top-right |
| position numeral | fixed colour (14,14,14) added inside colour windows 1 \| 2, reshaped per line by HDMA | bottom-right, 2×, still additive |

The circuit map sits above the position numeral. Banners such as "ROUND 1" stay centred.
SMK briefly zooms the numeral when the place changes and blows it up at the finish; NeoSMK
draws it at its steady height instead, so it stays in the corner under the circuit map. Where
the numeral passes under the lives / coins sprites, those pixels are tagged (class bit 6) so
the numeral is still drawn whole and leaves nothing behind in the picture. The HUD is also
described to the PPU when the MK8 layout is off, so the upward extension never extends the
timer's BG2 layer.

### Widescreen UI anchoring — done

On widened frames the PPU can move UI out to the screen edges (presentation only,
`Ppu.anchor*`, `snesrecomp_set_ui_anchors`). OAM entries get a per-entry side (-1 / 0 / +1);
they're drawn shifted by the extension width but counted for range / time-over at their
hardware position. BG layers shift from a given scanline down. SMK's BG2 lines 1–3 are a
full-width black border, so the shift starts at line 4. The colour window shifts by the same
amount. SMK: race-position icons (OAM 0–7) go left; lives / coins (16–23), the timer and item
box (BG2) and the position number (colour window) go right.

### HUD over the 3D layer — done

The 3D layer is clipped from the top of the player's view (not the band's first line), so walls
and objects near the horizon stick up past it, and the flat barrier tiles under 3D walls are
painted over with the surrounding ground. Walls are no longer cut near the camera: the player's
own kart is drawn in front of all 3D instead (a picture box passed to the sprite layer).
Screen-space sprites (Lakitu with the lap / reverse signs) are separated from line 1 and kept in
the upward extension, so they aren't covered or cut while flying up. Palette-4 sprites (SMK's
text: pause menu, results) are always in front. A HUD BG layer (SMK's BG2) shows nothing in the
widescreen side columns, where its tilemap holds unrelated graphics (red stripes on Ghost
Valley). The upward extension also stops a BG layer at rows using colours its real top lines
don't.

The HUD is separated from the picture on every racing frame, not only in the MK8 layout.
NeoSMK draws it after the 3D objects and sprites, in place when the MK8 layout is off.
Window-shaped colour maths covering most of a line (SMK dims the whole view on "ranked out",
leaving the position number as a hole) is a screen effect, not HUD. The native layout keeps
it; full-screen drops it, because the GPU floor couldn't match it. HUD rows spanning almost
the whole width (borders) stay out of the HUD boxes. Border detection reads the HUD layer as
well, so a separated border can't stop being detected.

### Depth-sorted sprites — done

3D objects drawn over the SNES picture would cover karts that are in front of them. The PPU
therefore separates track sprites on the racing bands (presentation only, `Ppu.objSep*`):
each such pixel goes to a sprite layer with its colour and the bottom scanline of its sprite,
and the picture is re-composited without it. The renderer draws that layer after the 3D
objects, giving each pixel the ground depth under its sprite's bottom line,
`depth = focal · height / (bottom − horizon)`, through `gl_FragDepth`. Karts and 3D objects
then occlude each other correctly. Screen-space sprites (SMK's OAM 0–27 and 120–127: position
icons, Lakitu and the start lights, lives / coins, banner text, Lakitu's shadow) are marked
"in front" (bottom line 255) and drawn over all 3D. SMK hands out the remaining entries to
karts and objects dynamically.

In the full-screen layout SMK stops drawing a kart once it drops below the band's bottom edge
(the split-screen border on the SNES). `src/neo/neo_karts.c` captures each CPU kart's sprite
from the sprite layer while it is inside the band, then draws that capture as a
camera-facing billboard (`scene3d_billboard`) with a blob shadow once the kart is below the
edge.

### B2. HD Mode 7 floor (optional, high payoff)

The full-screen layout already renders the floor this way below the SNES band. The remaining step
is to replace the band's floor as well (needs the sprites separated from the PPU's Mode-7 layer).

Decode the 1024×1024 Mode-7 tilemap and character data from VRAM into a GL texture and draw
the track as one real textured plane with the reconstructed camera. Benefits:

- full 480-line (and up) perspective instead of 224 scanlines; no shimmering
- widescreen and the floor come for free, with no wrapped-edge artifacts
- 3D objects get correct depth against the floor (blob shadows sit on it)

The SNES layers are still composited for the sky/scenery band, sprites and HUD.

### B3. Promoting objects to 3D

Order by visual payoff vs risk:

1. **Item boxes** (MK64-style rotating "?" cubes): **done** (`src/neo/neo_itembox.c`). SMK's
   boxes are 2 × 2 "?" panels in the Mode-7 tilemap (behaviour $14). Each frame NeoSMK finds them
   and floats a translucent rainbow cube with a blob shadow over each; a used panel loses its cube
2. **Coins**: **done** (`src/neo/neo_track.c`). Coin tiles (behaviour $1A, $FE on Mario
   Circuit) get a spinning gold coin with a blob shadow; a collected coin is rewritten to road
   by the game, so its coin disappears.
3. **Walls**: **done** (`src/neo/neo_track.c`). Barrier tiles (behaviour $80) are extruded into
   5-unit blocks with a studded top. The colour is the tile's average colour through the
   palette, and only faces next to open ground are emitted. The result is one static mesh,
   split at 60k vertices for 16-bit indices and rebuilt when the barrier layout changes. Wall
   pixels nearer than 75% of the kart's depth are discarded (`scene3d_near_cut`), so a wall
   between the camera and the kart doesn't block the view.
4. **Flat originals hidden**: a presentation-only Mode-7 tile override (`Ppu.m7Override`,
   `snesrecomp_set_m7_override`) paints each coin / panel cell with the road around it, on the
   racing bands only (also used for the GPU floor). Stripes such as the start grid are
   continued from the nearest visible cells left / right or up / down. VRAM is untouched.
5. **Pipes**: **done** (`src/neo/neo_pipes.c`). The pipe sprite palette differs per track; it is learnt each race by voting over sprites that stand on the pipes' projected positions, and the 3D colours are sampled from the largest pipe sprite (green on Mario Circuit, copper on Donut Plains). Pipes are objects in SMK's slots (WRAM $1800 +
   n·$80, handler +$04 = $E4E7). Each is drawn as a green 3D pipe. Before every frame
   (`snesrecomp_set_preframe_hook`), its palette-7 sprites near the pipe's projected base are
   hidden (`Ppu.objHide`: not drawn, but still counted like hardware). Used "?" panels (tiles
   $D4–$D7, road behaviour) are hidden through the tile override, which remembers every cell
   that held a panel this race.
6. **Other track obstacles** (Thwomps, moles, Cheep Cheeps)
7. **Shells, bananas** (dynamic but simple)
8. **Karts** (8 drivers × 12 rotation sprites today). A 300–800-triangle kart with a 128–256 px
   texture each; the driver can stay a camera-facing billboard at first

For each promoted object:

- read its position/rotation from WRAM each frame (presentation only)
- draw the mesh in the scene hook with **vertex lighting + simple per-pixel diffuse/specular**
- draw a **blob shadow** (projected quad, alpha-blended on the floor plane)
- suppress the original sprite: a presentation-only filter in the PPU sprite evaluator that
  skips OAM entries whose tile range belongs to that object (same pattern as the widescreen
  extension: never affects the CPU-visible sprite flags)

### Budgets (Pi 4)

| | |
|---|---|
| Triangles per frame | ≲ 20k (V3D fill rate, not vertex rate, is the limit at 720p+) |
| Textures | 32–256 px, RGBA8 or ETC2 (ETC2 is core in GLES 3.0) |
| Draw calls | ≲ 100 (instancing for item boxes and coins) |
| Lighting | 1 directional light + ambient; per-pixel only on karts |
| Shadows | blob/projected first; shadow maps out of scope for the Pi |
| AA | FXAA pass already in place |

### Assets

The game's own sprites aren't 3D, and NeoSMK can't ship Nintendo's assets, so meshes have to be
original. Proposed format: glTF 2.0 loaded with `cgltf` (single header, MIT), from an
`assets/` directory next to the binary, with the 2D sprite as fallback when a model is missing.

## Suggested order

1. RAM map for karts, camera and objects, plus the marker overlay (B1)
2. Mini-turbo drift (A): first gameplay feature, exercises the physics RAM map
3. Item boxes as 3D cubes + blob shadows (B3.1): first 3D object, exercises the full pipeline
4. HD Mode 7 floor (B2)
5. MK64 items, ghosts, karts in 3D

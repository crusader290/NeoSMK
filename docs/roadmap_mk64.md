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
brightness), and the sky colour fills the top. The overview map is replaced by a circuit map
built from the tile behaviours (`src/neo/neo_minimap.c`). The 3D layer follows the shifted
view through `M7View.line_offset`.

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
2. **Pipes / track obstacles** (static): instanced meshes
3. **Shells, bananas** (dynamic but simple)
4. **Karts** (8 drivers × 12 rotation sprites today). A 300–800-triangle kart with a 128–256 px
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

# NeoSMK

**Super Mario Kart** (SNES, 1992) static recompilation, rebuilt on SDL3 + OpenGL ES 3.0
for **Raspberry Pi 4 (ARM64)**, **Windows x64** and **Linux x64** — with a fixed-timestep
game loop, a GPU compositor, and true **16:9 widescreen** races.

NeoSMK is a fork of [sp00nznet/mk](https://github.com/sp00nznet/mk) (65C816 → C recompilation
on the [LakeSnes](https://github.com/elzo-d/LakeSnes)-based `snesrecomp` hardware backend).
The game logic and hardware emulation are unchanged; this fork replaces the platform layer
and is the base for the Mario Kart 64-style work in [`docs/roadmap_mk64.md`](docs/roadmap_mk64.md).

## Targets

| | |
|---|---|
| Minimum hardware | Raspberry Pi 4, 2 GB |
| CPU architectures | ARM64 / AArch64, x86-64 |
| Rendering API | OpenGL ES 3.0 (desktop OpenGL 3.3 core fallback) |
| Window / input / audio | SDL 3.2 (static, built from the pinned release) |
| Internal resolution | 640×480 (4:3) · 856×480 (16:9) · 770×480 (16:10) |
| Performance | locked 60 FPS on a 60 Hz display (vsync-snapped ticks) |
| Frame budget | 16.67 ms |
| Output | 480p / 720p / 1080p, windowed, exclusive fullscreen or borderless |
| Upscale | sharp-bilinear (default), nearest, linear; optional scanlines |
| AA | none or cheap FXAA (9 taps at internal resolution) |
| Post-processing | minimal |
| RAM | ~15 MB resident for emulation (well under the 512 MB budget) |
| Game logic | fixed 60.0988 Hz timestep (one NTSC SNES frame), independent renderer |

Geometry, lighting, shadows and texture-size targets apply to the 3D-object work in the
roadmap; the renderer already composes into a depth-buffered internal target for it.

## What's new vs upstream

- **SDL3 + OpenGL ES 3.0 renderer.** PPU frame → internal 640×480 render target (with depth,
  ready for 3D objects) → optional FXAA → aspect-fit output pass. Everything is GPU triangles;
  no CPU scaling. Falls back to desktop GL 3.3 core where a driver has no ES contexts.
- **Fixed-timestep loop, independent renderer.** Logic runs in 1/60.0988 s ticks; the
  renderer presents the latest frame. On a 60 Hz vsync'd display, ticks snap 1:1 to refreshes,
  so there's no judder from the 60.0988 vs 60.000 Hz mismatch. A ±0.5% audio-stream ratio
  controller absorbs the drift, so there are no crackles or overruns.
- **Widescreen (16:9 / 16:10).** The PPU renders up to 64 extra columns each side of the
  256-pixel line: Mode-7 and the wrapping scenery layers extend for real, with no stretching.
  It's presentation only: sprite limits are still counted over the hardware line and save-states
  never see it. WRAM/VRAM/CGRAM snapshots are byte-identical to 4:3 through a full race.
  By default only Mode-7 frames (races) are widened; menus keep 4:3.
- **Portable, deterministic builds.** One CMake project for all three targets; static SDL; CI
  produces Linux x64, Raspberry Pi 4 (ARM64) and Windows x64 packages. The ARM64 build's
  emulated state is byte-identical to x64, so netplay works across them.

## Downloads

Prebuilt **v0.3.0** builds for Raspberry Pi 4 (ARM64), Linux x64 and Windows x64 are in
[`releases/`](releases/README.md).

## Building

The ROM is not included. Supply your own **US v1.0** dump
(MD5 `7f25ce5a283d902694c52fb1152fa61a`) as `Super Mario Kart (USA).sfc` next to the binary,
or pass its path as an argument.

CMake ≥ 3.20 and a C17/C++17 compiler are required. SDL 3.2.30 is downloaded and built
statically at configure time (`-DNEOSMK_SYSTEM_SDL3=ON` uses an installed SDL3 instead;
`-DNEOSMK_SDL3_SOURCE=<dir|tarball>` builds offline).

### Linux x64 / Raspberry Pi 4 (native)

```sh
sudo scripts/install-deps-debian.sh            # Debian, Ubuntu, Raspberry Pi OS
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
# on the Pi add: -DNEOSMK_TUNE_PI4=ON  (Cortex-A72 code generation)
cmake --build build
./build/neosmk "Super Mario Kart (USA).sfc"
```

### Raspberry Pi 4 (cross-compile from x86-64 Debian Bookworm)

```sh
sudo scripts/install-deps-debian.sh arm64
cmake -B build-pi4 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake -DNEOSMK_TUNE_PI4=ON
cmake --build build-pi4
```

Built against Bookworm's glibc, the binary runs on 64-bit Raspberry Pi OS Bookworm and newer.

### Windows x64

Visual Studio 2022 (MSVC), no package manager needed:

```sh
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\neosmk.exe
```

The exe is self-contained (static SDL and CRT). MinGW-w64 cross builds from Linux also work:
`-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake`.

### CI

`.github/workflows/build.yml` builds all three targets on every push and smoke-tests each
binary (the ARM64 one under qemu). Pushing a `v*` tag publishes a release with
`neosmk-linux-x64.tar.gz`, `neosmk-linux-arm64-pi4.tar.gz` and `neosmk-windows-x64.zip`.

## Running on a Raspberry Pi 4

- **Desktop (X11/Wayland):** run as normal; `--borderless` for fullscreen.
- **Console / no desktop (best performance):** `SDL_VIDEO_DRIVER=kmsdrm ./neosmk --fullscreen --720p`
  drives the display directly through KMS/DRM. `--fullscreen` switches the display to the chosen
  preset, which also suits 4K TVs.
- Use the 64-bit OS with the default `vc4-kms-v3d` overlay. The V3D driver provides
  OpenGL ES 3.1.
- `SMK_STATS=1` prints the present rate, logic rate and frame time every 2 s. See
  [Performance](#performance).

## Usage

```
neosmk [--480p|--720p|--1080p] [--windowed|--fullscreen|--borderless] [rom.sfc]
```

| Key | |
|---|---|
| Arrows | D-pad |
| Z / X / A / S | B / Y / A / X |
| Q / W | L / R |
| Enter / Right Shift | Start / Select |
| F1 (or gamepad Guide) | show / hide the menu bar |
| F5 / F8 | save / load state |
| F11, Alt+Enter | toggle borderless fullscreen |
| Esc | quit |

Xbox-style gamepads work out of the box for both players (SDL3 gamepad database). Everything
is rebindable under **Controller**. The **Graphics** menu has output resolution, display mode,
aspect ratio (4:3 / 16:10 / 16:9, and whether menus are widened too), V-Sync, upscale filter,
FXAA, scanlines and an FPS overlay. Settings save to `smk_config.ini`.

**NeoSMK** menu (Mario Kart 64-style additions, saved per machine):

- **MK64 mini-turbo**, on by default. Power-slide (hop with R while steering), then flick
  the D-pad toward the *outside* of the turn: one flick gives yellow sparks, three give orange.
  Release R on orange for a short boost.
- **MK8-style HUD**, on by default. In full-screen races the HUD moves to the corners: timer
  and lap top-right, lives and coins bottom-left, a big position number bottom-right. The game's
  HUD is separated from the picture by the PPU (see `docs/roadmap_mk64.md`), so what was behind
  it shows through.
- **Circuit map**, on by default. In full-screen races (see below), a Mario Kart 8-style
  circuit outline on the right above the position number, with a dot per racer in its
  character's colour (player 1 ringed in white).
- **3D item boxes**, on by default. Spinning, translucent Mario Kart 64-style boxes with blob
  shadows float over the "?" panels; a box disappears once its panel is used. The flat "?"
  panels are no longer drawn on the track.
- **3D coins**, on by default. Spinning gold coins with blob shadows replace the flat coin
  tiles; a coin disappears once collected.
- **3D walls**, on by default. The track's barrier blocks (the coloured studded blocks on
  Mario Circuit) are raised into 3D walls. Walls between the camera and your kart are cut away
  so they never block the view. Collisions are unchanged.

Karts, shells and other track sprites are depth-sorted against these 3D objects, so a kart in
front of a wall or coin hides it, and one behind is hidden by it.

**Graphics → Full-screen 1P races** (on by default) makes single-player races fill the screen
instead of using the top half. The game's race view is shifted down (horizon at ~30% of the
screen), and the track below it is rendered on the GPU straight from the Mode-7 tilemap through
the reconstructed camera, so the perspective continues seamlessly. Above the race view, the PPU
renders the extra lines the shift uncovers, so clouds, scenery and Lakitu with the start lights
are no longer cut off at the top, and the sky colour fills the rest. The split-screen overview
map is replaced by the circuit map. Two-player races keep the native
split.

**Debug → 3D camera markers** floats a 3D marker over every racer. It checks the camera that
NeoSMK reconstructs from Mode 7 and that its 3D objects use.

**Multiplayer** is lockstep netplay over TCP (host = P1, client = P2), from upstream. Both
players need the same NeoSMK settings.

### Environment variables

| Variable | Effect |
|---|---|
| `SMK_GL=es` / `gl` | force OpenGL ES 3.0 or desktop GL 3.3 core |
| `SMK_FAST=1` | unthrottled (no pacing, no vsync), for benchmarks / automation |
| `SMK_HEADLESS=1` | hidden window, no menu; runs without a GPU if none is available |
| `SMK_STATS=1` | log fps / logic rate / frame time every 120 frames |
| `SMK_SCRIPT=…`, `SMK_MAX_FRAMES=N` | scripted input (`frame:BTN`, `a-b:BTN`) and auto-exit |
| `SMK_SNAPSHOT_PREFIX`, `SMK_SNAPSHOT_EVERY` | WRAM/VRAM/CGRAM snapshots for diffing |
| `SMK_SHOT_AT="frame:file.bmp,…"` | screenshot the frame presented after logic frame N |
| `SMK_POKE16="frame:addr:value,…"` | write a WRAM word (hex) after logic frame N, for test setups like a cheat code |
| `SMK_DUMP_M7=<prefix>` | per-scanline Mode-7 state every 60 frames (camera debugging) |
| `SMK_SHELLS=1`, `SMK_RECOMP=1`, … | upstream recompilation development modes (see `docs/`) |

Example: boot to a race and benchmark the emulation with no GPU work:

```sh
SDL_VIDEO_DRIVER=dummy SMK_HEADLESS=1 SMK_FAST=1 SMK_MAX_FRAMES=2400 \
SMK_SCRIPT="420:START,421:Y,600:START,660:START,720:START,780:START,840:START,900:START,960:START,1020:START,1080:START,1140:START,1200:START,1260:START,1320:START,1380:START,1440:START,1500:START,1550-2400:B" \
./neosmk
```

## Performance

On a 2.1 GHz Xeon core, emulation costs **3.5 ms per frame** at 4:3 and **4.0 ms** in 16:9
races, with ~15 MB resident. Under the CPU-only llvmpipe rasterizer at 1080p the game holds
60 FPS.

The Raspberry Pi 4 hasn't been measured on hardware yet. A Cortex-A72 is roughly 3–4× slower
per core, which puts emulation at an estimated 11–16 ms of the 16.67 ms budget. Profiling shows
the PPU's per-pixel compositor is about half the cost and the 65C816 core about a fifth. If the
Pi misses 60, the plan is a per-scanline PPU fast path and/or moving emulation off the render
thread (the Pi has 4 cores). Please run `SMK_STATS=1` on a Pi and report the numbers.

## Known limitations

- **Widescreen edges:** the game only spawns and animates objects inside its 256-pixel view, so
  karts and items can appear or disappear at the extended edges. The 1–2 black lines the game
  draws across the top of the 4:3 picture don't extend into the sides.
- **Windows:** without a driver ES extension or ANGLE, SDL can't create an ES context and NeoSMK
  uses desktop GL 3.3 (same renderer, same output).

## Project structure

```
├── src/main/main.c           entry point, CLI, fixed-timestep loop, test/RE hooks
├── src/recomp/               recompiled game functions (upstream)
├── include/smk/              smk_XXXXXX declarations
├── ext/snesrecomp/           SNES backend library
│   ├── src/platform_sdl3.c   SDL3 window / GL context / audio / scheduler
│   ├── src/gfx/              GL ES loader + renderer (internal target, FXAA, output)
│   ├── src/menu_overlay.cpp  ImGui menu + GL ES draw backend
│   ├── ext/LakeSnes/snes/    hardware emulation (PPU has the widescreen extension)
│   └── third_party/imgui/    Dear ImGui 1.91.9b (+ SDL3 backend)
├── cmake/                    SDL3 dependency, cross toolchains
├── scripts/                  dependency install
├── docs/                     upstream RE notes + roadmap_mk64.md
└── tools/                    upstream RE tooling (disassembler, harnesses)
```

## Credits and license

MIT. Built on sp00nznet/mk, snesrecomp and LakeSnes (MIT), Dear ImGui (MIT) and SDL (zlib).
See [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md). This project contains no Nintendo
copyrighted material.

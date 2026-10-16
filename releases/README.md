# NeoSMK builds

Prebuilt binaries, one folder per version. Each archive holds the executable, `README.md`,
`LICENSE` and `THIRD_PARTY_LICENSES.md`. SDL3 is linked statically, so nothing else needs
installing. **No ROM is included:** put your own US v1.0 dump
(MD5 `7f25ce5a283d902694c52fb1152fa61a`) next to the executable as
`Super Mario Kart (USA).sfc`, or pass its path as the first argument.

## v0.3.0 (latest)

Built from commit `b454f86` (SDL 3.2.30, Dear ImGui 1.91.9b). Checksums are in
[`v0.3.0/SHA256SUMS`](v0.3.0/SHA256SUMS). Same platforms and toolchains as v0.1.0 below.

| File | Platform |
|---|---|
| [`neosmk-linux-arm64-pi4.tar.gz`](v0.3.0/neosmk-linux-arm64-pi4.tar.gz) | Raspberry Pi 4 (and newer), 64-bit Raspberry Pi OS Bookworm or later |
| [`neosmk-linux-x64.tar.gz`](v0.3.0/neosmk-linux-x64.tar.gz) | x86-64 Linux with glibc ≥ 2.35 (Debian 12, Ubuntu 22.04+, Fedora 36+) |
| [`neosmk-windows-x64.zip`](v0.3.0/neosmk-windows-x64.zip) | Windows 10/11 x64 |

**New since v0.2.0:**

- **Full-screen single-player races** (Graphics → Full-screen 1P races, on by default). The race
  fills the screen instead of the top half: the game's view is shifted down, the track below it
  is rendered on the GPU from the Mode-7 tilemap, and the lines above it (clouds, scenery, Lakitu
  with the start lights) are rendered by the PPU instead of being cut off. Two-player races keep
  the split screen.
- **Circuit map** (NeoSMK menu, on by default). A Mario Kart 8-style map at the bottom right with
  every racer in their character colour, replacing SMK's overview map in full-screen races.
- **MK8-style HUD** (NeoSMK menu, on by default). In full-screen races the HUD moves to the
  corners: timer and lap top-right, lives and coins bottom-left, position bottom-right under the
  circuit map. The PPU separates the HUD from the picture, so what was behind it is shown.
- Visual only: game state is unchanged by all of the above.

**Tested:** Linux x64 and the Pi 4 build (under qemu-aarch64) ran the ROM title → menus → race and
produced WRAM/VRAM/CGRAM snapshots byte-identical to the reference run. The Linux package's
screenshots show the full-screen layout, circuit map and HUD. Windows was checked under Wine
(`--version`). **Not yet run on real Raspberry Pi or native Windows hardware.**

## v0.2.0

Built from commit `0451d5f` (SDL 3.2.30, Dear ImGui 1.91.9b). Checksums are in
[`v0.2.0/SHA256SUMS`](v0.2.0/SHA256SUMS). Same platforms and toolchains as v0.1.0
below. The bundled README repeats the "3D item boxes" bullet (fixed in the repository).

| File | Platform |
|---|---|
| [`neosmk-linux-arm64-pi4.tar.gz`](v0.2.0/neosmk-linux-arm64-pi4.tar.gz) | Raspberry Pi 4 (and newer), 64-bit Raspberry Pi OS Bookworm or later |
| [`neosmk-linux-x64.tar.gz`](v0.2.0/neosmk-linux-x64.tar.gz) | x86-64 Linux with glibc ≥ 2.35 (Debian 12, Ubuntu 22.04+, Fedora 36+) |
| [`neosmk-windows-x64.zip`](v0.2.0/neosmk-windows-x64.zip) | Windows 10/11 x64 |

**New since v0.1.0** (the first Mario Kart 64-style features; see the **NeoSMK** menu):

- **MK64 mini-turbo** (on by default). Power-slide with R, flick the D-pad toward the outside of the
  turn (yellow sparks, then orange after three flicks), release on orange for a boost. Gameplay
  change; netplay peers need the same setting.
- **3D item boxes** (on by default). Spinning, translucent rainbow boxes with blob shadows over
  the "?" panels; a box vanishes once its panel is used. Visual only.
- **Debug → 3D camera markers.** A marker over every racer, placed with the camera that NeoSMK
  reconstructs from Mode 7.
- Developer tools: `SMK_SHOT_AT`, `SMK_POKE16`, `SMK_DUMP_M7` (see the main README).

**Tested:** all three packages ran the ROM title → menus → race. Linux x64 and the Pi 4 build
(under qemu-aarch64) produced WRAM/VRAM/CGRAM snapshots byte-identical to the reference run;
the Linux build's screenshots show the item boxes. Windows was checked under Wine. **Not yet run
on real Raspberry Pi or native Windows hardware.**

## v0.1.0

Built from commit `c10ee17` (SDL 3.2.30, Dear ImGui 1.91.9b). Checksums are in
[`v0.1.0/SHA256SUMS`](v0.1.0/SHA256SUMS).

| File | Platform | Built with |
|---|---|---|
| [`neosmk-linux-arm64-pi4.tar.gz`](v0.1.0/neosmk-linux-arm64-pi4.tar.gz) | Raspberry Pi 4 (and newer), 64-bit Raspberry Pi OS Bookworm or later | GCC 12 cross-compiler on Debian Bookworm, `-mcpu=cortex-a72` |
| [`neosmk-linux-x64.tar.gz`](v0.1.0/neosmk-linux-x64.tar.gz) | x86-64 Linux with glibc ≥ 2.34 (Debian 12, Ubuntu 22.04+, Fedora 35+, …) | GCC 12 on Debian Bookworm |
| [`neosmk-windows-x64.zip`](v0.1.0/neosmk-windows-x64.zip) | Windows 10/11 x64 | MinGW-w64 (GCC 13), static runtime |

### Running

```sh
# Linux / Raspberry Pi
tar xzf neosmk-linux-arm64-pi4.tar.gz      # or neosmk-linux-x64.tar.gz
cd neosmk-linux-arm64-pi4
./neosmk "Super Mario Kart (USA).sfc"

# Pi without a desktop (KMS/DRM, best performance)
SDL_VIDEO_DRIVER=kmsdrm ./neosmk --fullscreen --720p
```

On Windows, unzip and run `neosmk.exe` (drag the ROM onto it, or keep it next to the exe).

### What was tested

- **Linux x64:** the packaged binary booted the ROM through title → menus → race.
  WRAM/VRAM/CGRAM snapshots match the development build byte-for-byte; 60 FPS at 1080p on a
  software GL renderer.
- **Raspberry Pi 4:** the same binary ran the ROM to the race under qemu-aarch64, with
  snapshots byte-identical to x64. **Not yet run on real Pi hardware.** Please report
  `SMK_STATS=1 ./neosmk` output.
- **Windows x64:** ran to the 16:9 race at 60 FPS under Wine (desktop GL 3.3 fallback there).
  Not yet run on native Windows.

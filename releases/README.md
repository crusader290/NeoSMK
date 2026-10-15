# NeoSMK builds

Prebuilt binaries, one folder per version. Each archive holds the executable, `README.md`,
`LICENSE` and `THIRD_PARTY_LICENSES.md`. SDL3 is linked statically, so nothing else needs
installing. **No ROM is included:** put your own US v1.0 dump
(MD5 `7f25ce5a283d902694c52fb1152fa61a`) next to the executable as
`Super Mario Kart (USA).sfc`, or pass its path as the first argument.

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

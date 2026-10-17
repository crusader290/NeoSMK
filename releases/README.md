# NeoSMK builds

Prebuilt Windows binaries. Each archive holds `neosmk.exe`, `README.md`, `LICENSE` and
`THIRD_PARTY_LICENSES.md`. SDL3 and the C runtime are linked statically, so nothing else needs
installing. **No ROM is included:** put your own US v1.0 dump
(MD5 `7f25ce5a283d902694c52fb1152fa61a`) next to the executable as
`Super Mario Kart (USA).sfc`, or drag it onto `neosmk.exe`.

## v0.8.2 (latest)

Built from commit `81fb36c` (SDL 3.2.30, Dear ImGui 1.91.9b, MinGW-w64 GCC 13, static runtime).
Checksum in [`v0.8.2/SHA256SUMS`](v0.8.2/SHA256SUMS). Linux x64 and Raspberry Pi 4 are built
from source (see the main README).

| File | Platform |
|---|---|
| [`neosmk-windows-x64.zip`](v0.8.2/neosmk-windows-x64.zip) | Windows 10/11 x64 |

**Changes since v0.8.1** (the full feature list is in the main README):

- **Widescreen HUD always at the edges.** The "Widescreen UI at the edges" option is gone: in
  widescreen races SMK's timer, item box, lives / coins and position move to the screen edges.
- **Karts you pass look right.** Karts drawn below the race view no longer pick up pieces of a
  kart they're touching (doubled Toad, bits of Bowser, stray slivers).
- **Your kart stays in front of walls.** Scraping along a barrier no longer cuts off a wheel,
  and a crashed kart at the bottom of the split view stays in front of the wall it hit.
- **Readable position number** in the MK8-style HUD: solid white with a dark outline, instead
  of washing out over white lines and bright walls.

**Not tested:** this package was built but not run. Netplay peers should both use v0.8.2.

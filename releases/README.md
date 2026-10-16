# NeoSMK builds

Prebuilt Windows binaries. Each archive holds `neosmk.exe`, `README.md`, `LICENSE` and
`THIRD_PARTY_LICENSES.md`. SDL3 and the C runtime are linked statically, so nothing else needs
installing. **No ROM is included:** put your own US v1.0 dump
(MD5 `7f25ce5a283d902694c52fb1152fa61a`) next to the executable as
`Super Mario Kart (USA).sfc`, or drag it onto `neosmk.exe`.

## v0.8.1 (latest)

Built from commit `4badf07` (SDL 3.2.30, Dear ImGui 1.91.9b, MinGW-w64 GCC 13, static runtime).
Checksum in [`v0.8.1/SHA256SUMS`](v0.8.1/SHA256SUMS). Linux x64 and Raspberry Pi 4 are built
from source (see the main README).

| File | Platform |
|---|---|
| [`neosmk-windows-x64.zip`](v0.8.1/neosmk-windows-x64.zip) | Windows 10/11 x64 |

**Changes since v0.8.0** (the full feature list is in the main README):

- **MK64 mini-turbo reworked.** Sparks now charge while you hold the power-slide: yellow, then
  orange. Holding the D-pad into the turn charges fastest (orange in about a second), and
  MK64-style taps (outward, then back into the turn) add more. The old version only counted
  flicks toward the outside, and a slide started without a direction held never charged.
  Release R on orange for the boost, as before.
- **Custom race music removed** (the Sound menu options and the `music/` folder are gone).

**Not tested:** this package was built but not run. Netplay peers both need v0.8.1 (the
mini-turbo rules changed).

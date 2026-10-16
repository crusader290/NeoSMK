# NeoSMK builds

Prebuilt Windows binaries. Each archive holds `neosmk.exe`, `README.md`, `LICENSE` and
`THIRD_PARTY_LICENSES.md`. SDL3 and the C runtime are linked statically, so nothing else needs
installing. **No ROM is included:** put your own US v1.0 dump
(MD5 `7f25ce5a283d902694c52fb1152fa61a`) next to the executable as
`Super Mario Kart (USA).sfc`, or drag it onto `neosmk.exe`.

## v0.8.0 (latest)

Built from commit `b76d11e` (SDL 3.2.30, Dear ImGui 1.91.9b, dr_mp3 0.6.39, MinGW-w64 GCC 13,
static runtime). Checksum in [`v0.8.0/SHA256SUMS`](v0.8.0/SHA256SUMS). Linux x64 and
Raspberry Pi 4 are built from source (see the main README).

| File | Platform |
|---|---|
| [`neosmk-windows-x64.zip`](v0.8.0/neosmk-windows-x64.zip) | Windows 10/11 x64 |

**Latest changes** (the full feature list is in the main README):

- **Custom race music** (Sound menu, off by default): put your own `.mp3` files in the `music/`
  folder next to `neosmk.exe` (included, with a short README). A different song plays each race
  instead of the course music; the engine, item and Lakitu sounds stay. **Race music volume**
  sets the level. Songs can be added while the game runs.
- README rewrite with original-vs-NeoSMK comparison screenshots.

**Not tested:** this package was built but not run. Netplay peers both need v0.8.0 (custom music
itself doesn't affect sync; each player hears their own folder).

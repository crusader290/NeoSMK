# NeoSMK builds

Prebuilt Windows binaries. Each archive holds `neosmk.exe`, `README.md`, `LICENSE` and
`THIRD_PARTY_LICENSES.md`. SDL3 and the C runtime are linked statically, so nothing else needs
installing. **No ROM is included:** put your own US v1.0 dump
(MD5 `7f25ce5a283d902694c52fb1152fa61a`) next to the executable as
`Super Mario Kart (USA).sfc`, or drag it onto `neosmk.exe`.

## v0.7.0 (latest)

Built from commit `f343eee` (SDL 3.2.30, Dear ImGui 1.91.9b, MinGW-w64 GCC 13, static runtime).
Checksum in [`v0.7.0/SHA256SUMS`](v0.7.0/SHA256SUMS). Linux x64 and Raspberry Pi 4 are
built from source (see the main README).

| File | Platform |
|---|---|
| [`neosmk-windows-x64.zip`](v0.7.0/neosmk-windows-x64.zip) | Windows 10/11 x64 |

**Latest changes** (the full feature list is in the main README):

- **MK64 items** (NeoSMK menu, on by default; gameplay change, netplay uses the host's setting):
  item boxes can give a triple mushroom, golden mushroom (7.5 s of unlimited boosts), triple
  green or red shells, or a banana bunch (5). Dots under the item box show the uses left.
  Getting hit loses the rest. Player 1 only.
- **Lap-time board at the finish:** shown whole in full-screen and in front of 3D objects
  (it was cut off).
- **Pipes:** no flicker of the flat pipe when crashing into one.
- **Circuit map:** Donut Plains and Bowser Castle now have one, Ghost Valley's is solid, and
  Time Trial no longer shows dots for absent CPU racers.

**Not tested:** this package was built but not run. Netplay peers both need v0.7.0.

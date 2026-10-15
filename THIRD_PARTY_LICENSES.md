# Third-party components

NeoSMK is MIT-licensed (see `LICENSE`). It builds on the following projects;
their licenses travel with the source and every binary release.

| Component | Location | License | Source |
|-----------|----------|---------|--------|
| sp00nznet/mk (SMK recompilation, tools, docs) | `src/`, `include/`, `tools/`, `docs/` | MIT — `LICENSE` | https://github.com/sp00nznet/mk @ `6207e3c` |
| snesrecomp | `ext/snesrecomp/` | MIT — `ext/snesrecomp/LICENSE` | https://github.com/sp00nznet/snesrecomp @ `41c5603` |
| LakeSnes (SNES hardware emulation core) | `ext/snesrecomp/ext/LakeSnes/` | MIT — `ext/snesrecomp/ext/LakeSnes/LICENSE.txt` | https://github.com/sp00nznet/LakeSnes @ `7df883d` (fork of angelo-wf/LakeSnes) |
| Dear ImGui 1.91.9b | `ext/snesrecomp/third_party/imgui/` | MIT — `ext/snesrecomp/third_party/imgui/LICENSE.txt` | https://github.com/ocornut/imgui |
| SDL 3.2.30 | downloaded at configure time, linked statically | zlib — https://github.com/libsdl-org/SDL/blob/release-3.2.30/LICENSE.txt | https://github.com/libsdl-org/SDL |

No Nintendo copyrighted material is included. Super Mario Kart is a trademark
of Nintendo; you must supply your own legally obtained ROM.

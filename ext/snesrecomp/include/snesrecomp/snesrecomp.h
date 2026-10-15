/*
 * snesrecomp — SNES Hardware Backend Library
 * https://github.com/sp00nznet/snesrecomp
 *
 * Drop-in SNES hardware for your static recompilation project.
 * Powered by LakeSnes (MIT) — real PPU rendering, SPC700 audio,
 * DMA, Mode 7, the whole deal. You bring the recompiled game code,
 * we bring the hardware.
 *
 * Usage:
 *   #include <snesrecomp/snesrecomp.h>
 *
 *   snesrecomp_init("My SNES Recomp", 3);
 *   snesrecomp_load_rom("game.sfc");
 *   while (snesrecomp_poll()) {               // events, once per display frame
 *       int n = snesrecomp_ticks_due();       // fixed 60.0988 Hz logic ticks
 *       for (int i = 0; i < n; i++) {
 *           snesrecomp_begin_frame();         // input
 *           // ... run recompiled functions ...
 *           bus_write8(0x00, 0x2100, value);  // writes hit real PPU
 *           snesrecomp_end_frame();           // PPU scanlines + audio
 *       }
 *       snesrecomp_present();                 // GL ES compose + swap
 *   }
 *   snesrecomp_shutdown();
 */

#ifndef SNESRECOMP_H
#define SNESRECOMP_H

#include "cpu.h"
#include "cpu_ops.h"
#include "bus.h"
#include "func_table.h"
#include "recomp_patch.h"
#include "platform.h"
#include "input.h"

#include <stdbool.h>

/*
 * Initialize all SNES hardware subsystems and the SDL3 platform layer
 * (window, OpenGL ES 3.0 context, audio). Call once before anything else.
 *
 * window_title: displayed in the window title bar
 * scale:        unused (window size comes from the output-resolution setting)
 */
bool snesrecomp_init(const char *window_title, int scale);

/*
 * Load a ROM file (.sfc / .smc). Handles copier headers, LoROM/HiROM/ExHiROM
 * auto-detection, and PAL/NTSC region detection automatically.
 * Returns true on success.
 */
bool snesrecomp_load_rom(const char *path);

/*
 * Frame scheduling — rendering is independent of the fixed-timestep logic.
 *   poll()      pump window/input events; false once quit is requested
 *   ticks_due() block until >=1 logic tick is due, return how many to run
 *   present()   compose the latest frame + menu and swap buffers
 */
bool snesrecomp_poll(void);
int  snesrecomp_ticks_due(void);
void snesrecomp_present(void);

/*
 * Call at the start of each logic tick before running recompiled game code.
 * Updates input and the joypad auto-read registers. Always returns true
 * (quit is reported by snesrecomp_poll).
 */
bool snesrecomp_begin_frame(void);

/*
 * Call at the end of each logic tick after running recompiled game code.
 * Renders all PPU scanlines, extracts audio samples and hands the frame to
 * the renderer (drawn by the next snesrecomp_present).
 */
void snesrecomp_end_frame(void);

/*
 * Real-frame path — run the genuine ROM via LakeSnes's full cycle-accurate
 * frame (renders everything, incl. the Mode-7 race, like tools/lakesnes_ref).
 * Use instead of begin_frame/recompiled-shells/end_frame to play gameplay the
 * recomp's per-frame shells can't yet drive. begin reads input (+ netplay
 * exchange); end runs the frame and submits video/audio.
 */
bool snesrecomp_realframe_begin(void);
void snesrecomp_realframe_end(void);

/*
 * Active picture region within the 512x478 framebuffer (excludes the black
 * overscan bands the PPU centers the image with). The platform crops to this
 * so the game fills the window without top/bottom black borders.
 */
void snesrecomp_active_video_rect(int *x, int *y, int *w, int *h);

/* Widescreen aspect settings (menu Graphics -> Aspect ratio). The PPU renders
 * extra columns either side of the 256-pixel line — Mode-7 and wrapping BG
 * layers extend naturally; only presentation changes, never emulated state. */
#define SNESRECOMP_ASPECT_4_3   0
#define SNESRECOMP_ASPECT_16_10 1
#define SNESRECOMP_ASPECT_16_9  2

/* Emulator controls (menu File -> New / Save / Load). reset() hard-resets and
 * restarts the ROM; save/load_state serialize the full machine state to/from a
 * file (LakeSnes save-state). */
void snesrecomp_reset(void);
bool snesrecomp_save_state(const char *path);
bool snesrecomp_load_state(const char *path);

/* FNV-1a hash of WRAM (netplay desync detection / lockstep verification). */
uint32_t snesrecomp_wram_checksum(void);

/*
 * Trigger VBlank processing — NMI flag, PPU vblank handling.
 * Call this when your recompiled code reaches the NMI/VBlank point.
 */
void snesrecomp_trigger_vblank(void);

/*
 * Get a pointer to the rendered framebuffer (512x478 RGBX8888).
 * Valid after snesrecomp_end_frame().
 */
const uint8_t *snesrecomp_get_framebuffer(void);

/*
 * Read a VRAM word at the given word address (0-32767).
 */
uint16_t snesrecomp_read_vram(uint16_t word_addr);

/*
 * Dump PPU diagnostic info to a file. For debugging rendering issues.
 */
void snesrecomp_dump_ppu(const char *filepath);

/*
 * Dump a compact binary snapshot of guest WRAM + VRAM for lockstep
 * divergence analysis (see tools/diff_snapshots.py). Used to compare a
 * pure-interpreter reference run against a recompiled run frame-by-frame.
 */
void snesrecomp_dump_snapshot(const char *filepath);

/*
 * Clean shutdown — frees all SNES hardware and closes SDL3.
 */
void snesrecomp_shutdown(void);

#endif /* SNESRECOMP_H */

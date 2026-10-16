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

/* HUD separation (m7view.h). A game describes its HUD; on frames with a racing
 * view at the top, the PPU renders those pixels to a separate HUD layer (and
 * the picture without them), so the HUD can be laid out freely and drawn over
 * 3D objects (scene3d_ui_hud). Presentation only. */
typedef struct {
    unsigned char obj_mask[16];   /* OAM entries that are HUD (bit per entry) */
    unsigned char bg_mask;        /* BG layers (bit n = BG n+1) that are HUD on the
                                   * race band's non-Mode-7 lines */
    bool          window_math;    /* window-shaped colour math on the band is HUD */
    bool          separate;       /* false: only describe the HUD (the full-screen
                                   * layout's upward extension leaves it out) and
                                   * keep it in the picture; true: the caller
                                   * draws the HUD layer every such frame */
} SnesHudConfig;

#define SNES_HUD_BG    1          /* classes, as tagged by the PPU */
#define SNES_HUD_OBJ   2
#define SNES_HUD_MATH  3

typedef struct {
    bool valid;                   /* this frame has a separated HUD */
    int  cols, lines;             /* HUD layer size (presented picture) */
    struct { bool present; int x0, y0, x1, y1; } box[4];   /* per class, inclusive */
    int  math_flags;              /* SNES_HUD_MATH blend: bit 0 subtract, bit 1 half */
    uint8_t math_rgb[3];          /* SNES_HUD_MATH colour (the fixed colour it applies) */
} SnesHudFrame;

void snesrecomp_set_hud_config(const SnesHudConfig *cfg);   /* NULL = off */
const SnesHudFrame *snesrecomp_hud(void);

/* Mode-7 tile override (presentation only): on the racing (chase) bands, map
 * cell i (ty * 128 + tx) shows tile cells[i] instead of the tilemap's, unless
 * cells[i] == 0xFFFF. Used to hide flat track objects that are drawn in 3D.
 * The array must stay valid; NULL disables. VRAM is never changed. */
void snesrecomp_set_m7_override(const uint16_t *cells);

/* Sprite layer (presentation only): sprites on the racing bands are taken out
 * of the picture and drawn after the 3D layer at the ground depth under each
 * sprite, so karts and 3D objects occlude each other correctly. `exclude` has
 * a bit per OAM entry to keep in the picture (separated HUD sprites are always
 * kept); entries in `front` (screen-space sprites such as Lakitu, banners or
 * the HUD) are drawn in front of all 3D. Either mask may be NULL. */
void snesrecomp_set_sprite_layer(bool enabled, const uint8_t exclude[16], const uint8_t front[16]);
/* Sprites using these palettes (bit per OBJ palette) are drawn in front of
 * all 3D as well, whatever OAM entry they sit in (screen text, menus). */
void snesrecomp_set_sprite_front_palettes(uint8_t mask);

/* The separated sprite layer of the most recently submitted frame (cols x
 * lines RGBA: rgb + the sprite's bottom scanline, 0 = no sprite, 255 = in
 * front), or NULL when the frame had none. */
const uint8_t *snesrecomp_sprite_layer(int *cols, int *lines);

/* One OAM entry as the PPU will draw it. */
typedef struct {
    int  x, y;          /* x: -256..255 (9-bit, sign-extended); y: 0..255 */
    int  tile;          /* 0..511 (name-table bit included) */
    int  palette;       /* 0..7 */
    int  priority;      /* 0..3 */
    bool hflip, vflip;
    int  size;          /* width = height in pixels */
} SnesOamEntry;
void snesrecomp_read_oam(int index, SnesOamEntry *out);
/* Colour index (0..15) of pixel (x, y) of a sprite whose top-left tile is
 * `tile` (0..511), as the PPU addresses it. */
int snesrecomp_obj_pixel(int tile, int x, int y);

/* Called before every frame is rendered (presentation set-up: OAM then holds
 * what that frame will show). */
typedef void (*SnesPreFrameHook)(void *user);
void snesrecomp_set_preframe_hook(SnesPreFrameHook hook, void *user);

/* OAM entries the next frame leaves out of the picture (bit per entry;
 * presentation only, e.g. objects drawn in 3D instead). NULL clears. */
void snesrecomp_set_obj_hide(const uint8_t mask[16]);

/* Widescreen UI anchoring (presentation only): on widened 1-player frames,
 * OAM entry n is moved obj[n] * extension columns (-1 = to the left edge,
 * +1 = to the right edge, 0 = stays), BG layer n's content by bg[n] from
 * scanline bg_min_line down (full-width borders above it stay put) and the
 * colour window (window-shaped colour math) by color_window. NULL disables.
 * Sprite range / time-over flags still follow the hardware positions. */
void snesrecomp_set_ui_anchors(const int8_t obj[128], const int8_t bg[4], int bg_min_line,
                               int color_window);

/* Emulator controls (menu File -> New / Save / Load). reset() hard-resets and
 * restarts the ROM; save/load_state serialize the full machine state to/from a
 * file (LakeSnes save-state). */
void snesrecomp_reset(void);
bool snesrecomp_save_state(const char *path);
bool snesrecomp_load_state(const char *path);

/* Incremented whenever emulated state jumps (reset, load-state, netplay
 * sync). Code that keeps state alongside the emulation (e.g. gameplay mods)
 * resets it when this changes, so every peer / replay stays in step. */
uint32_t snesrecomp_state_epoch(void);

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

/* Read a CGRAM colour (BGR555). */
uint16_t snesrecomp_read_cgram(uint8_t index);

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
 * Dump the last frame's per-scanline Mode-7 state (start position + per-pixel
 * step) as text, one line per mode-7 scanline. Camera-reconstruction debugging.
 */
void snesrecomp_dump_m7(const char *filepath);

/* Dump the 128 OAM entries (x, y, tile, palette, priority, flips, size). */
void snesrecomp_dump_oam(const char *filepath);

/*
 * Clean shutdown — frees all SNES hardware and closes SDL3.
 */
void snesrecomp_shutdown(void);

#endif /* SNESRECOMP_H */

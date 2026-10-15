/*
 * menu_overlay — Dear ImGui menu system for the SMK launcher.
 *
 * Persistent main menu bar (File / Graphics / Sound / Controller / Multiplayer /
 * Help) over the game, modelled on gb-recompiled's menu_gui. Supports keyboard
 * AND gamepad (SDL3 SDL_Gamepad) bindings for BOTH players. Rendered with the
 * renderer's OpenGL ES 3.0 context at native output resolution.
 *
 * The C++ implementation (menu_overlay.cpp) exposes this C API so the C SDL
 * platform + input layers can drive it. Disabled automatically in
 * headless/scripted runs (SMK_HEADLESS) so it never adds overhead or
 * intercepts input during automated flows.
 */
#ifndef SNESRECOMP_MENU_OVERLAY_H
#define SNESRECOMP_MENU_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

/* Lifecycle. init() loads smk_config.ini (also when disabled, so settings
 * apply to headless runs) and is otherwise a no-op when SMK_HEADLESS /
 * SMK_SCRIPT / SMK_MAX_FRAMES is set or `gl_context` is NULL. */
void menu_overlay_init(struct SDL_Window *window, void *gl_context);
void menu_overlay_shutdown(void);

/* Feed an SDL_Event (void* to keep this header SDL-free).
 * Returns 1 if the menu captured the event (game should ignore it). */
int  menu_overlay_process_event(const void *sdl_event);

/* Build + render the menu for this frame into the current GL framebuffer
 * (after the game frame is drawn, before the buffer swap). */
void menu_overlay_render(void);

/* 1 while the menu is capturing input — the game must ignore keyboard/pad. */
int  menu_overlay_is_active(void);

/* Height of the main menu bar in window coordinates (0 when the menu is
 * disabled or hidden with F1). The game is drawn below it. */
int  menu_overlay_get_menubar_height(void);

/* Menu-bar background colour (0-255 RGB; 0,0,0 when disabled). */
void menu_overlay_get_bar_color(int *r, int *g, int *b);

/* Set by File -> Quit. */
int  menu_overlay_quit_requested(void);

/* One-shot emulator-control requests from the File menu (return 1 once and
 * clear). The host consumes these between frames:
 *   New        -> reset/restart the emulator
 *   Save State -> snesrecomp_save_state()
 *   Load State -> snesrecomp_load_state()
 */
int  menu_overlay_take_reset(void);
int  menu_overlay_take_save_state(void);
int  menu_overlay_take_load_state(void);

/* ---- Settings accessors (polled by the platform each frame) ---- */
int   menu_overlay_get_resolution(void);    /* PLATFORM_RES_* */
int   menu_overlay_get_display_mode(void);  /* PLATFORM_WINDOWED/FULLSCREEN/BORDERLESS */
void  menu_overlay_set_resolution(int res);
void  menu_overlay_set_display_mode(int mode);
int   menu_overlay_get_vsync(void);
int   menu_overlay_get_aspect(void);        /* SNESRECOMP_ASPECT_* (4:3, 16:10, 16:9) */
int   menu_overlay_get_wide_menus(void);    /* 1 = widen non-Mode-7 screens too */
int   menu_overlay_get_filter(void);        /* SMK_FILTER_* (0 nearest, 1 linear, 2 sharp) */
int   menu_overlay_get_fxaa(void);
int   menu_overlay_get_scanlines(void);
int   menu_overlay_get_show_fps(void);
float menu_overlay_get_volume(void);        /* 0.0..1.0 (already 0 when muted) */

/* ---- Per-player input bindings (player = 1 or 2) ---- */
/* Keyboard SDL_Scancode bound to a SNES button (SNES_BTN_IDX_*), or -1. */
int menu_overlay_key_for_button(int player, int snes_btn_idx);
/* SDL_GamepadButton bound to a SNES button, or -1. */
int menu_overlay_pad_button_for_button(int player, int snes_btn_idx);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_MENU_OVERLAY_H */

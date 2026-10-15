/*
 * menu_overlay_stub.c — no-op implementation used when SNESRECOMP_MENU is OFF
 * (e.g. consumers that don't want the ImGui dependency). Keeps platform_sdl3.c
 * and input.c linking with sensible defaults (no overlay, default bindings).
 */
#include "snesrecomp/menu_overlay.h"
#include "snesrecomp/input.h"
#include "snesrecomp/platform.h"

static int s_res  = PLATFORM_RES_720P;
static int s_mode = PLATFORM_WINDOWED;

void  menu_overlay_init(struct SDL_Window *w, void *ctx) { (void)w; (void)ctx; }
void  menu_overlay_shutdown(void) {}
int   menu_overlay_process_event(const void *e) { (void)e; return 0; }
void  menu_overlay_render(void) {}
int   menu_overlay_is_active(void) { return 0; }
int   menu_overlay_get_menubar_height(void) { return 0; }
void  menu_overlay_get_bar_color(int *r, int *g, int *b) { *r = 0; *g = 0; *b = 0; }
int   menu_overlay_quit_requested(void) { return 0; }
int   menu_overlay_take_reset(void) { return 0; }
int   menu_overlay_take_save_state(void) { return 0; }
int   menu_overlay_take_load_state(void) { return 0; }
int   menu_overlay_get_resolution(void) { return s_res; }
int   menu_overlay_get_display_mode(void) { return s_mode; }
void  menu_overlay_set_resolution(int res) { s_res = res; }
void  menu_overlay_set_display_mode(int mode) { s_mode = mode; }
int   menu_overlay_get_vsync(void) { return 1; }
int   menu_overlay_get_aspect(void) { return 2; }   /* SNESRECOMP_ASPECT_16_9 */
int   menu_overlay_get_wide_menus(void) { return 0; }
int   menu_overlay_get_filter(void) { return 2; }   /* SMK_FILTER_SHARP */
int   menu_overlay_get_fxaa(void) { return 0; }
int   menu_overlay_get_scanlines(void) { return 0; }
int   menu_overlay_get_show_fps(void) { return 0; }
float menu_overlay_get_volume(void) { return 1.0f; }

/* Default keyboard bindings (P1 snes9x-style; P2 right-hand cluster). */
int menu_overlay_key_for_button(int player, int idx) {
    /* SDL scancodes — avoid including SDL here; values are stable ABI. */
    static const int p1[12] = { 29,27,229,40,82,81,80,79,4,22,20,26 };  /* B,Y,Sel,Start,U,D,L,R,A,X,L,R */
    static const int p2[12] = { 14,13,228,88,96,93,92,94,15,12,24,18 };
    if (idx < 0 || idx >= 12) return -1;
    if (player == 1) return p1[idx];
    if (player == 2) return p2[idx];
    return -1;
}
int menu_overlay_pad_button_for_button(int player, int idx) {
    /* SDL_GamepadButton mapping: B=South, A=East, Y=West, X=North, L/R
     * shoulders, Select=Back, Start=Start, dpad U/D/L/R. SDL3's values match
     * SDL2's SDL_GameControllerButton, so old configs keep working. */
    static const int pad[12] = { 0,2,4,6,11,12,13,14,1,3,9,10 };
    (void)player;
    if (idx < 0 || idx >= 12) return -1;
    return pad[idx];
}
void menu_overlay_add_option(const char *menu, const char *label, const char *key,
                             int *value, int max, const char *const *choices,
                             const char *tooltip) {
    (void)menu; (void)label; (void)key; (void)value; (void)max; (void)choices; (void)tooltip;
}

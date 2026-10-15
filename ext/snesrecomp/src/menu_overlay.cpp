/*
 * menu_overlay.cpp — Dear ImGui menu system for the SMK launcher.
 *
 * Modelled on sp00nznet/gb-recompiled's menu_gui: a persistent main menu bar
 * (File / Graphics / Sound / Controller / Multiplayer / Help) drawn over the
 * game via the ImGui SDL3 platform backend and a small OpenGL ES 3.0 draw
 * backend (below) that shares the renderer's GL loader. Supports keyboard
 * AND gamepad (Xbox-style) bindings for both players. Config (settings +
 * bindings) saves to smk_config.ini. F1 / gamepad Guide hides the menu bar.
 */

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "snesrecomp/menu_overlay.h"
#include "snesrecomp/input.h"   /* SNES_BTN_IDX_* */
#include "snesrecomp/mp_session.h"
#include "snesrecomp/platform.h"
#include "snesrecomp/snesrecomp.h"   /* SNESRECOMP_ASPECT_* */
#include "gfx/gl.h"
#include "gfx/renderer.h"

#define SMK_BTN_COUNT 12
#define SMK_CONFIG_PATH "smk_config.ini"
#define SMK_REPO_URL "https://github.com/crusader290/NeoSMK"
#define SMK_UPSTREAM_URL "https://github.com/sp00nznet/mk"

/* Button display names, indexed by SNES_BTN_IDX_* (B,Y,Select,Start,Up,Down,
 * Left,Right,A,X,L,R). */
static const char *kBtnNames[SMK_BTN_COUNT] = {
    "B", "Y", "Select", "Start", "Up", "Down",
    "Left", "Right", "A", "X", "L", "R"
};

static const char *kResNames[3] = { "480p (640x480)", "720p (1280x720)", "1080p (1920x1080)" };

struct MenuState {
    bool enabled        = false;
    bool config_loaded  = false;
    bool quit_requested = false;
    bool hidden         = false;   /* F1: hide the menu bar */

    /* Graphics */
    int  resolution = PLATFORM_RES_720P;
    int  display    = PLATFORM_WINDOWED;
    int  vsync      = 1;
    int  filter     = SMK_FILTER_SHARP;
    int  fxaa       = 0;
    int  scanlines  = 0;
    int  show_fps   = 0;
    int  aspect     = SNESRECOMP_ASPECT_16_9;
    int  wide_menus = 0;   /* widen non-Mode-7 screens too */

    /* Sound */
    float volume = 1.0f;
    bool  mute   = false;

    /* Controller bindings: [player 0..1][SNES button 0..11] */
    int  key[2][SMK_BTN_COUNT];   /* SDL_Scancode, -1 = unset */
    int  pad[2][SMK_BTN_COUNT];   /* SDL_GamepadButton, -1 = unset */

    /* Windows */
    bool show_about      = false;
    bool show_controller = false;
    bool show_mp         = false;

    /* Multiplayer (netplay) connection fields. */
    char mp_ip[64] = "127.0.0.1";
    int  mp_port   = 7878;

    /* Rebind capture. rebind_btn = -1 when idle; otherwise awaiting input for
     * (rebind_player, rebind_btn); rebind_pad selects keyboard(0) vs gamepad(1). */
    int  rebind_player = 0;
    int  rebind_btn    = -1;
    int  rebind_pad    = 0;

    /* Height of the main menu bar in window units (captured each frame). */
    int  menubar_h = 0;

    /* One-shot File-menu emulator requests (consumed by the host). */
    bool reset_req = false;
    bool save_req  = false;
    bool load_req  = false;
};

static MenuState g;

/* Game-registered options (menu_overlay_add_option): shown under one extra
 * menu and persisted in smk_config.ini next to the built-in settings. */
#define MAX_GAME_OPTIONS 32
struct GameOption {
    const char *menu, *label, *key, *tooltip;
    int *value;
    int max;           /* 1 = checkbox, >1 = 0..max choices */
    const char *const *choices;
};
static GameOption g_opts[MAX_GAME_OPTIONS];
static int g_nopts = 0;

/* ================= OpenGL ES 3.0 ImGui draw backend ================= */

static struct {
    GLuint prog, vao, vbo, ibo, font;
    GLint  u_proj, u_tex;
} gr;

static const char *kImguiVS =
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in vec4 a_col;\n"
    "uniform mat4 u_proj;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_col = a_col;\n"
    "    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *kImguiFS =
    "in vec2 v_uv;\n"
    "in vec4 v_col;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "    o_color = v_col * texture(u_tex, v_uv);\n"
    "}\n";

static bool gr_init(void) {
    static const char *const attribs[] = { "a_pos", "a_uv", "a_col", NULL };
    gr.prog = smkgl_build_program("imgui", kImguiVS, kImguiFS, attribs);
    if (!gr.prog) return false;
    gr.u_proj = glGetUniformLocation(gr.prog, "u_proj");
    gr.u_tex  = glGetUniformLocation(gr.prog, "u_tex");

    glGenVertexArrays(1, &gr.vao);
    glGenBuffers(1, &gr.vbo);
    glGenBuffers(1, &gr.ibo);
    glBindVertexArray(gr.vao);
    glBindBuffer(GL_ARRAY_BUFFER, gr.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gr.ibo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, pos));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, uv));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (void *)offsetof(ImDrawVert, col));
    glBindVertexArray(0);

    ImGuiIO &io = ImGui::GetIO();
    unsigned char *pixels = NULL;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    glGenTextures(1, &gr.font);
    glBindTexture(GL_TEXTURE_2D, gr.font);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)gr.font);
    io.BackendRendererName = "neosmk_gles3";
    return true;
}

static void gr_shutdown(void) {
    if (gr.font) glDeleteTextures(1, &gr.font);
    if (gr.vbo)  glDeleteBuffers(1, &gr.vbo);
    if (gr.ibo)  glDeleteBuffers(1, &gr.ibo);
    if (gr.vao)  glDeleteVertexArrays(1, &gr.vao);
    if (gr.prog) glDeleteProgram(gr.prog);
    memset(&gr, 0, sizeof(gr));
}

static void gr_render(ImDrawData *dd) {
    int fb_w = (int)(dd->DisplaySize.x * dd->FramebufferScale.x);
    int fb_h = (int)(dd->DisplaySize.y * dd->FramebufferScale.y);
    if (fb_w <= 0 || fb_h <= 0 || dd->CmdListsCount == 0) return;

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glViewport(0, 0, fb_w, fb_h);

    float L = dd->DisplayPos.x, R = dd->DisplayPos.x + dd->DisplaySize.x;
    float T = dd->DisplayPos.y, B = dd->DisplayPos.y + dd->DisplaySize.y;
    const float proj[16] = {
        2.0f / (R - L),    0.0f,              0.0f, 0.0f,
        0.0f,              2.0f / (T - B),    0.0f, 0.0f,
        0.0f,              0.0f,             -1.0f, 0.0f,
        (R + L) / (L - R), (T + B) / (B - T), 0.0f, 1.0f,
    };
    glUseProgram(gr.prog);
    glUniformMatrix4fv(gr.u_proj, 1, GL_FALSE, proj);
    glUniform1i(gr.u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(gr.vao);
    glBindBuffer(GL_ARRAY_BUFFER, gr.vbo);

    ImVec2 off = dd->DisplayPos, sc = dd->FramebufferScale;
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList *cl = dd->CmdLists[n];
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cl->VtxBuffer.Size * sizeof(ImDrawVert),
                     cl->VtxBuffer.Data, GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)cl->IdxBuffer.Size * sizeof(ImDrawIdx),
                     cl->IdxBuffer.Data, GL_STREAM_DRAW);
        for (int c = 0; c < cl->CmdBuffer.Size; c++) {
            const ImDrawCmd *cmd = &cl->CmdBuffer[c];
            if (cmd->UserCallback) {
                if (cmd->UserCallback != ImDrawCallback_ResetRenderState)
                    cmd->UserCallback(cl, cmd);
                continue;
            }
            float x0 = (cmd->ClipRect.x - off.x) * sc.x, y0 = (cmd->ClipRect.y - off.y) * sc.y;
            float x1 = (cmd->ClipRect.z - off.x) * sc.x, y1 = (cmd->ClipRect.w - off.y) * sc.y;
            if (x1 <= x0 || y1 <= y0) continue;
            glScissor((GLint)x0, (GLint)(fb_h - y1), (GLsizei)(x1 - x0), (GLsizei)(y1 - y0));
            glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd->GetTexID());
            glDrawElements(GL_TRIANGLES, (GLsizei)cmd->ElemCount,
                           sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                           (void *)(intptr_t)(cmd->IdxOffset * sizeof(ImDrawIdx)));
        }
    }
    glBindVertexArray(0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
}

/* ================= bindings + config ================= */

/* Standard SNES->Xbox face mapping: SNES B(bottom)=South, A(right)=East,
 * Y(left)=West, X(top)=North; shoulders L/R; Back/Start; dpad. */
static void set_default_pad(MenuState &m, int p) {
    m.pad[p][SNES_BTN_IDX_B]      = SDL_GAMEPAD_BUTTON_SOUTH;
    m.pad[p][SNES_BTN_IDX_A]      = SDL_GAMEPAD_BUTTON_EAST;
    m.pad[p][SNES_BTN_IDX_Y]      = SDL_GAMEPAD_BUTTON_WEST;
    m.pad[p][SNES_BTN_IDX_X]      = SDL_GAMEPAD_BUTTON_NORTH;
    m.pad[p][SNES_BTN_IDX_L]      = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    m.pad[p][SNES_BTN_IDX_R]      = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    m.pad[p][SNES_BTN_IDX_SELECT] = SDL_GAMEPAD_BUTTON_BACK;
    m.pad[p][SNES_BTN_IDX_START]  = SDL_GAMEPAD_BUTTON_START;
    m.pad[p][SNES_BTN_IDX_UP]     = SDL_GAMEPAD_BUTTON_DPAD_UP;
    m.pad[p][SNES_BTN_IDX_DOWN]   = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    m.pad[p][SNES_BTN_IDX_LEFT]   = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    m.pad[p][SNES_BTN_IDX_RIGHT]  = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
}

static void set_default_keys(MenuState &m) {
    /* Player 1 — classic snes9x-style layout. */
    m.key[0][SNES_BTN_IDX_B]      = SDL_SCANCODE_Z;
    m.key[0][SNES_BTN_IDX_Y]      = SDL_SCANCODE_X;
    m.key[0][SNES_BTN_IDX_SELECT] = SDL_SCANCODE_RSHIFT;
    m.key[0][SNES_BTN_IDX_START]  = SDL_SCANCODE_RETURN;
    m.key[0][SNES_BTN_IDX_UP]     = SDL_SCANCODE_UP;
    m.key[0][SNES_BTN_IDX_DOWN]   = SDL_SCANCODE_DOWN;
    m.key[0][SNES_BTN_IDX_LEFT]   = SDL_SCANCODE_LEFT;
    m.key[0][SNES_BTN_IDX_RIGHT]  = SDL_SCANCODE_RIGHT;
    m.key[0][SNES_BTN_IDX_A]      = SDL_SCANCODE_A;
    m.key[0][SNES_BTN_IDX_X]      = SDL_SCANCODE_S;
    m.key[0][SNES_BTN_IDX_L]      = SDL_SCANCODE_Q;
    m.key[0][SNES_BTN_IDX_R]      = SDL_SCANCODE_W;
    /* Player 2 — right-hand cluster (gamepad is the expected P2 device). */
    m.key[1][SNES_BTN_IDX_B]      = SDL_SCANCODE_K;
    m.key[1][SNES_BTN_IDX_Y]      = SDL_SCANCODE_J;
    m.key[1][SNES_BTN_IDX_SELECT] = SDL_SCANCODE_RCTRL;
    m.key[1][SNES_BTN_IDX_START]  = SDL_SCANCODE_KP_ENTER;
    m.key[1][SNES_BTN_IDX_UP]     = SDL_SCANCODE_KP_8;
    m.key[1][SNES_BTN_IDX_DOWN]   = SDL_SCANCODE_KP_5;
    m.key[1][SNES_BTN_IDX_LEFT]   = SDL_SCANCODE_KP_4;
    m.key[1][SNES_BTN_IDX_RIGHT]  = SDL_SCANCODE_KP_6;
    m.key[1][SNES_BTN_IDX_A]      = SDL_SCANCODE_L;
    m.key[1][SNES_BTN_IDX_X]      = SDL_SCANCODE_I;
    m.key[1][SNES_BTN_IDX_L]      = SDL_SCANCODE_U;
    m.key[1][SNES_BTN_IDX_R]      = SDL_SCANCODE_O;
}

static void set_default_binds(MenuState &m) { set_default_keys(m); set_default_pad(m, 0); set_default_pad(m, 1); }

static void reset_defaults(MenuState &m) {
    m.resolution = PLATFORM_RES_720P; m.display = PLATFORM_WINDOWED;
    m.vsync = 1; m.filter = SMK_FILTER_SHARP; m.fxaa = 0; m.scanlines = 0; m.show_fps = 0;
    m.aspect = SNESRECOMP_ASPECT_16_9; m.wide_menus = 0;
    m.volume = 1.0f; m.mute = false;
    set_default_binds(m);
}

/* ---- Config persistence (simple key=value ini) ---- */

static void config_save(void) {
    FILE *f = fopen(SMK_CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "# NeoSMK settings\n");
    fprintf(f, "resolution=%d\n", g.resolution);
    fprintf(f, "display=%d\n",    g.display);
    fprintf(f, "vsync=%d\n",      g.vsync);
    fprintf(f, "filter=%d\n",     g.filter);
    fprintf(f, "fxaa=%d\n",       g.fxaa);
    fprintf(f, "scanlines=%d\n",  g.scanlines);
    fprintf(f, "show_fps=%d\n",   g.show_fps);
    fprintf(f, "aspect=%d\n",     g.aspect);
    fprintf(f, "wide_menus=%d\n", g.wide_menus);
    fprintf(f, "menu_hidden=%d\n", g.hidden ? 1 : 0);
    for (int i = 0; i < g_nopts; i++) fprintf(f, "%s=%d\n", g_opts[i].key, *g_opts[i].value);
    fprintf(f, "volume=%d\n",     (int)(g.volume * 100.0f + 0.5f));
    fprintf(f, "mute=%d\n",       g.mute ? 1 : 0);
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < SMK_BTN_COUNT; i++) {
            fprintf(f, "p%d_key_%s=%d\n", p + 1, kBtnNames[i], g.key[p][i]);
            fprintf(f, "p%d_pad_%s=%d\n", p + 1, kBtnNames[i], g.pad[p][i]);
        }
    fclose(f);
}

static int btn_index_by_name(const char *name) {
    for (int i = 0; i < SMK_BTN_COUNT; i++)
        if (strcmp(name, kBtnNames[i]) == 0) return i;
    return -1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void config_load(void) {
    FILE *f = fopen(SMK_CONFIG_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char keybuf[80]; int val;
        if (sscanf(line, "%79[^=]=%d", keybuf, &val) != 2) continue;
        bool game_opt = false;
        for (int i = 0; i < g_nopts; i++)
            if (!strcmp(keybuf, g_opts[i].key)) { *g_opts[i].value = clampi(val, 0, g_opts[i].max); game_opt = true; }
        if (game_opt) continue;
        if      (!strcmp(keybuf, "resolution"))  g.resolution = clampi(val, 0, 2);
        else if (!strcmp(keybuf, "display"))     g.display = clampi(val, 0, 2);
        else if (!strcmp(keybuf, "vsync"))       g.vsync = val ? 1 : 0;
        else if (!strcmp(keybuf, "filter"))      g.filter = clampi(val, 0, 2);
        else if (!strcmp(keybuf, "fxaa"))        g.fxaa = val ? 1 : 0;
        else if (!strcmp(keybuf, "scanlines"))   g.scanlines = val ? 1 : 0;
        else if (!strcmp(keybuf, "show_fps"))    g.show_fps = val ? 1 : 0;
        else if (!strcmp(keybuf, "aspect"))      g.aspect = clampi(val, 0, 2);
        else if (!strcmp(keybuf, "wide_menus"))  g.wide_menus = val ? 1 : 0;
        else if (!strcmp(keybuf, "menu_hidden")) g.hidden = val != 0;
        else if (!strcmp(keybuf, "volume"))      g.volume = clampi(val, 0, 100) / 100.0f;
        else if (!strcmp(keybuf, "mute"))        g.mute = val != 0;
        else if (keybuf[0] == 'p' && (keybuf[1] == '1' || keybuf[1] == '2') && keybuf[2] == '_') {
            int p = keybuf[1] - '1';
            const char *rest = keybuf + 3;
            if      (!strncmp(rest, "key_", 4)) { int bi = btn_index_by_name(rest + 4); if (bi >= 0) g.key[p][bi] = val; }
            else if (!strncmp(rest, "pad_", 4)) { int bi = btn_index_by_name(rest + 4); if (bi >= 0) g.pad[p][bi] = val; }
        }
    }
    fclose(f);
}

/* ---- Theme ---- */

static void apply_theme(void) {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 4.0f;
    s.FrameRounding  = 3.0f;
    s.GrabRounding   = 3.0f;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_MenuBarBg]     = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);  /* opaque (matches frame clear) */
    c[ImGuiCol_Header]        = ImVec4(0.20f, 0.45f, 0.85f, 0.65f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.55f, 0.95f, 0.80f);
}

/* ================= public API ================= */

extern "C" void menu_overlay_init(struct SDL_Window *window, void *gl_context) {
    if (!g.config_loaded) {
        g.config_loaded = true;
        set_default_binds(g);
        config_load();
    }
    if (g.enabled || !window || !gl_context) return;
    if (getenv("SMK_HEADLESS") || getenv("SMK_SCRIPT") || getenv("SMK_MAX_FRAMES")) return;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    apply_theme();
    if (!ImGui_ImplSDL3_InitForOpenGL((SDL_Window *)window, gl_context) || !gr_init()) {
        fprintf(stderr, "menu: ImGui init failed — menu disabled\n");
        gr_shutdown();
        ImGui::DestroyContext();
        return;
    }
    g.enabled = true;
}

extern "C" void menu_overlay_shutdown(void) {
    if (!g.enabled) return;
    config_save();
    gr_shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    g.enabled = false;
}

extern "C" int menu_overlay_process_event(const void *sdl_event) {
    if (!g.enabled) return 0;
    const SDL_Event *ev = (const SDL_Event *)sdl_event;

    /* Rebind capture takes precedence so the menu doesn't swallow it first. */
    if (g.rebind_btn >= 0) {
        if (ev->type == SDL_EVENT_KEY_DOWN) {
            if (ev->key.scancode == SDL_SCANCODE_ESCAPE) g.rebind_btn = -1;
            else if (g.rebind_pad == 0) { g.key[g.rebind_player][g.rebind_btn] = ev->key.scancode; g.rebind_btn = -1; }
            return 1;
        }
        if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && g.rebind_pad == 1) {
            g.pad[g.rebind_player][g.rebind_btn] = ev->gbutton.button;
            g.rebind_btn = -1;
            return 1;
        }
    }

    /* F1 / gamepad Guide: show or hide the menu bar (console-style setups). */
    if ((ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_F1 && !ev->key.repeat) ||
        (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && ev->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
        g.hidden = !g.hidden;
        return 1;
    }

    ImGui_ImplSDL3_ProcessEvent(ev);
    ImGuiIO &io = ImGui::GetIO();
    return (io.WantCaptureKeyboard || io.WantCaptureMouse) ? 1 : 0;
}

static void bind_button_row(int player, int btn) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("%s", kBtnNames[btn]);

    /* Keyboard column */
    ImGui::TableNextColumn();
    bool kb_rebinding = (g.rebind_btn == btn && g.rebind_player == player && g.rebind_pad == 0);
    const char *kn = kb_rebinding ? "<press a key>" : SDL_GetScancodeName((SDL_Scancode)g.key[player][btn]);
    if (!kn || !kn[0]) kn = "<unset>";
    char kl[64]; snprintf(kl, sizeof(kl), "%s##k%d_%d", kn, player, btn);
    if (ImGui::Button(kl, ImVec2(-1, 0))) { g.rebind_player = player; g.rebind_btn = btn; g.rebind_pad = 0; }

    /* Gamepad column */
    ImGui::TableNextColumn();
    bool pad_rebinding = (g.rebind_btn == btn && g.rebind_player == player && g.rebind_pad == 1);
    const char *pn = pad_rebinding ? "<press a pad button>"
                   : (g.pad[player][btn] >= 0 ? SDL_GetGamepadStringForButton((SDL_GamepadButton)g.pad[player][btn]) : "<unset>");
    if (!pn || !pn[0]) pn = "<unset>";
    char pl[64]; snprintf(pl, sizeof(pl), "%s##p%d_%d", pn, player, btn);
    if (ImGui::Button(pl, ImVec2(-1, 0))) { g.rebind_player = player; g.rebind_btn = btn; g.rebind_pad = 1; }
}

static void draw_player_tab(int player) {
    if (ImGui::BeginTable("binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Button");
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableSetupColumn("Gamepad");
        ImGui::TableHeadersRow();
        for (int i = 0; i < SMK_BTN_COUNT; i++) bind_button_row(player, i);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    char id[32]; snprintf(id, sizeof(id), "Reset P%d defaults", player + 1);
    if (ImGui::Button(id)) { set_default_keys(g); set_default_pad(g, player); }
}

static void draw_controller_window(void) {
    ImGui::SetNextWindowSize(ImVec2(440, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Controller", &g.show_controller)) {
        int npads = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&npads);
        SDL_free(ids);
        ImGui::Text("Gamepads connected: %d", npads);
        ImGui::TextDisabled("Click a cell, then press a key / pad button. Esc cancels.");
        ImGui::Separator();
        if (ImGui::BeginTabBar("players")) {
            if (ImGui::BeginTabItem("Player 1")) { draw_player_tab(0); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Player 2")) { draw_player_tab(1); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        if (ImGui::Button("Save")) config_save();
    }
    ImGui::End();
}

static void draw_multiplayer_window(void) {
    ImGui::SetNextWindowSize(ImVec2(380, 280), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Multiplayer (netplay)", &g.show_mp)) {
        ImGui::TextWrapped("Lockstep netplay over TCP. The host shares its game "
            "state, then both machines play in sync. Host = Player 1, "
            "client = Player 2. Pick 2-player in-game once connected.");
        ImGui::Separator();
        ImGui::Text("Status: %s", mp_status_text());
        ImGui::Separator();

        MpState st = mp_get_state();
        if (st == MP_IDLE || st == MP_DISCONNECTED) {
            int delay = mp_get_delay();
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderInt("Input delay (frames)", &delay, 0, 10)) mp_set_delay(delay);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Higher absorbs more network lag but adds input delay.\n"
                                  "Host sets this; the client adopts it. ~2-3 for LAN.");
            ImGui::Separator();
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("Port", &g.mp_port);
            if (g.mp_port < 1)     g.mp_port = 1;
            if (g.mp_port > 65535) g.mp_port = 65535;
            if (ImGui::Button("Host  (Player 1)", ImVec2(-1, 0))) mp_host(g.mp_port);
            ImGui::Spacing();
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("Host IP", g.mp_ip, sizeof(g.mp_ip));
            if (ImGui::Button("Join  (Player 2)", ImVec2(-1, 0))) mp_join(g.mp_ip, g.mp_port);
        } else {
            if (st == MP_HOSTING)    ImGui::TextDisabled("Waiting for a peer to connect...");
            if (st == MP_CONNECTING) ImGui::TextDisabled("Connecting...");
            if (st == MP_CONNECTED)  ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "Connected — playing in sync.");
            ImGui::Spacing();
            if (ImGui::Button("Disconnect", ImVec2(-1, 0))) mp_disconnect();
        }
    }
    ImGui::End();
}

static void draw_about_window(void) {
    ImGui::SetNextWindowSize(ImVec2(420, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About", &g.show_about)) {
        ImGui::Text("NeoSMK — Super Mario Kart");
        ImGui::TextDisabled("Static Recompilation (SNES, 1992)");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::BulletText("Backend: snesrecomp (LakeSnes)");
        ImGui::BulletText("CPU: WDC 65C816 + DSP-1");
        ImGui::BulletText("Video: %s", (const char *)glGetString(GL_VERSION));
        ImGui::BulletText("GPU: %s", (const char *)glGetString(GL_RENDERER));
        int iw = 0, ih = 0;
        renderer_get_internal_size(&iw, &ih);
        ImGui::BulletText("Internal: %dx%d, logic %.4f Hz fixed", iw, ih, SNES_TICK_HZ);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", SMK_REPO_URL);
        if (ImGui::Button("Open repository")) SDL_OpenURL(SMK_REPO_URL);
        ImGui::SameLine();
        if (ImGui::Button("Upstream (sp00nznet/mk)")) SDL_OpenURL(SMK_UPSTREAM_URL);
    }
    ImGui::End();
}

/* MenuItem toggle over an int setting. */
static void menu_toggle(const char *label, const char *shortcut, int *v) {
    bool b = *v != 0;
    if (ImGui::MenuItem(label, shortcut, &b)) *v = b ? 1 : 0;
}

static void draw_menubar(void) {
    if (!ImGui::BeginMainMenuBar()) return;
    g.menubar_h = (int)(ImGui::GetWindowSize().y + 0.5f);
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "restart")) g.reset_req = true;
        if (ImGui::MenuItem("Save", "F5"))     g.save_req  = true;
        if (ImGui::MenuItem("Load", "F8"))     g.load_req  = true;
        ImGui::Separator();
        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Reset settings")) reset_defaults(g);
            if (ImGui::MenuItem("Save settings"))  config_save();
            if (ImGui::MenuItem("Load settings"))  config_load();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit"))           g.quit_requested = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Graphics")) {
        if (ImGui::BeginMenu("Output resolution")) {
            for (int r = 0; r < 3; r++)
                if (ImGui::MenuItem(kResNames[r], NULL, g.resolution == r)) g.resolution = r;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Display")) {
            if (ImGui::MenuItem("Windowed", "F11", g.display == PLATFORM_WINDOWED))   g.display = PLATFORM_WINDOWED;
            if (ImGui::MenuItem("Fullscreen (switch mode)", NULL, g.display == PLATFORM_FULLSCREEN)) g.display = PLATFORM_FULLSCREEN;
            if (ImGui::MenuItem("Borderless desktop", "F11", g.display == PLATFORM_BORDERLESS)) g.display = PLATFORM_BORDERLESS;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Aspect ratio")) {
            if (ImGui::MenuItem("4:3 (original)", NULL, g.aspect == SNESRECOMP_ASPECT_4_3))  g.aspect = SNESRECOMP_ASPECT_4_3;
            if (ImGui::MenuItem("16:10 widescreen", NULL, g.aspect == SNESRECOMP_ASPECT_16_10)) g.aspect = SNESRECOMP_ASPECT_16_10;
            if (ImGui::MenuItem("16:9 widescreen", NULL, g.aspect == SNESRECOMP_ASPECT_16_9))  g.aspect = SNESRECOMP_ASPECT_16_9;
            ImGui::Separator();
            menu_toggle("Widen menus too", NULL, &g.wide_menus);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Widescreen always applies to Mode-7 scenes (races).\n"
                                  "Menus and title screens only have 256 columns of\n"
                                  "real content, so widening them shows wrapped tiles.");
            ImGui::EndMenu();
        }
        menu_toggle("V-Sync", NULL, &g.vsync);
        if (ImGui::BeginMenu("Upscale filter")) {
            if (ImGui::MenuItem("Nearest",        NULL, g.filter == SMK_FILTER_NEAREST)) g.filter = SMK_FILTER_NEAREST;
            if (ImGui::MenuItem("Linear",         NULL, g.filter == SMK_FILTER_LINEAR))  g.filter = SMK_FILTER_LINEAR;
            if (ImGui::MenuItem("Sharp bilinear", NULL, g.filter == SMK_FILTER_SHARP))   g.filter = SMK_FILTER_SHARP;
            ImGui::EndMenu();
        }
        menu_toggle("FXAA", NULL, &g.fxaa);
        menu_toggle("Scanlines", NULL, &g.scanlines);
        menu_toggle("Show FPS", NULL, &g.show_fps);
        ImGui::Separator();
        if (ImGui::MenuItem("Hide menu bar", "F1")) g.hidden = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Sound")) {
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("Volume", &g.volume, 0.0f, 1.0f, "%.2f");
        ImGui::MenuItem("Mute", NULL, &g.mute);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Controller")) {
        ImGui::MenuItem("Configure (P1 / P2)...", NULL, &g.show_controller);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Multiplayer")) {
        ImGui::MenuItem("Connect (Host / Join)...", NULL, &g.show_mp);
        ImGui::Separator();
        ImGui::TextDisabled("%s", mp_status_text());
        ImGui::EndMenu();
    }
    /* game-registered menus, in registration order */
    for (int i = 0; i < g_nopts; i++) {
        bool first = true;
        for (int j = 0; j < i; j++) if (!strcmp(g_opts[j].menu, g_opts[i].menu)) first = false;
        if (!first || !ImGui::BeginMenu(g_opts[i].menu)) continue;
        for (int j = i; j < g_nopts; j++) {
            GameOption &o = g_opts[j];
            if (strcmp(o.menu, g_opts[i].menu)) continue;
            if (o.max <= 1) {
                menu_toggle(o.label, NULL, o.value);
            } else if (ImGui::BeginMenu(o.label)) {
                for (int c = 0; c <= o.max; c++)
                    if (ImGui::MenuItem(o.choices[c], NULL, *o.value == c)) *o.value = c;
                ImGui::EndMenu();
            }
            if (o.tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", o.tooltip);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("About", NULL, &g.show_about);
        if (ImGui::MenuItem("Source repository")) SDL_OpenURL(SMK_REPO_URL);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

extern "C" void menu_overlay_render(void) {
    if (!g.enabled) return;

    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (g.hidden) {
        g.menubar_h = 0;
    } else {
        draw_menubar();
        if (g.show_controller) draw_controller_window();
        if (g.show_mp)         draw_multiplayer_window();
        if (g.show_about)      draw_about_window();
    }

    if (g.show_fps) {
        float fps = 0, tps = 0, ms = 0;
        platform_get_stats(&fps, &tps, &ms);
        ImGuiIO &io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 8, (float)g.menubar_h + 8),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("##fps", NULL,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%.0f FPS  %.1f ms", fps, ms);
            ImGui::TextDisabled("logic %.2f Hz", tps);
        }
        ImGui::End();
    }

    ImGui::Render();
    gr_render(ImGui::GetDrawData());
}

extern "C" int menu_overlay_is_active(void) {
    if (!g.enabled) return 0;
    ImGuiIO &io = ImGui::GetIO();
    return (g.rebind_btn >= 0 || io.WantCaptureKeyboard || io.WantCaptureMouse) ? 1 : 0;
}
extern "C" int   menu_overlay_get_menubar_height(void) { return (g.enabled && !g.hidden) ? g.menubar_h : 0; }
extern "C" void  menu_overlay_get_bar_color(int *r, int *gg, int *b) {
    /* The MenuBarBg colour (so the platform can clear the frame to it — any strip
     * the menu bar doesn't paint then matches the menu instead of going black). */
    if (g.enabled) {
        ImVec4 c = ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg];
        *r = (int)(c.x * 255 + 0.5f); *gg = (int)(c.y * 255 + 0.5f); *b = (int)(c.z * 255 + 0.5f);
    } else { *r = 0; *gg = 0; *b = 0; }
}
extern "C" int   menu_overlay_quit_requested(void) { return g.quit_requested; }
extern "C" int   menu_overlay_take_reset(void)      { int r = g.reset_req; g.reset_req = false; return r; }
extern "C" int   menu_overlay_take_save_state(void) { int r = g.save_req;  g.save_req  = false; return r; }
extern "C" int   menu_overlay_take_load_state(void) { int r = g.load_req;  g.load_req  = false; return r; }
extern "C" int   menu_overlay_get_resolution(void)   { return g.resolution; }
extern "C" int   menu_overlay_get_display_mode(void) { return g.display; }
extern "C" void  menu_overlay_set_resolution(int r)  { g.resolution = clampi(r, 0, 2); }
extern "C" void  menu_overlay_set_display_mode(int m){ g.display = clampi(m, 0, 2); }
extern "C" int   menu_overlay_get_vsync(void)      { return g.vsync; }
extern "C" int   menu_overlay_get_aspect(void)     { return g.aspect; }
extern "C" int   menu_overlay_get_wide_menus(void) { return g.wide_menus; }
extern "C" int   menu_overlay_get_filter(void)     { return g.filter; }
extern "C" int   menu_overlay_get_fxaa(void)       { return g.fxaa; }
extern "C" int   menu_overlay_get_scanlines(void)  { return g.scanlines; }
extern "C" int   menu_overlay_get_show_fps(void)   { return g.show_fps; }
extern "C" float menu_overlay_get_volume(void)     { return g.mute ? 0.0f : g.volume; }

extern "C" int menu_overlay_key_for_button(int player, int idx) {
    if (player < 1 || player > 2 || idx < 0 || idx >= SMK_BTN_COUNT) return -1;
    return g.key[player - 1][idx];
}
extern "C" int menu_overlay_pad_button_for_button(int player, int idx) {
    if (player < 1 || player > 2 || idx < 0 || idx >= SMK_BTN_COUNT) return -1;
    return g.pad[player - 1][idx];
}

extern "C" void menu_overlay_add_option(const char *menu, const char *label, const char *key,
                                        int *value, int max, const char *const *choices,
                                        const char *tooltip) {
    if (g_nopts >= MAX_GAME_OPTIONS || !value) return;
    GameOption &o = g_opts[g_nopts++];
    o.menu = menu; o.label = label; o.key = key; o.value = value;
    o.max = (max > 1 && choices) ? max : 1;
    o.choices = choices; o.tooltip = tooltip;
}

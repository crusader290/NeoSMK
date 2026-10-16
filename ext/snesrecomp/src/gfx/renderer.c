/*
 * renderer.c — OpenGL ES 3.0 frame compositor (see renderer.h).
 */
#include "renderer.h"
#include "gl.h"
#include "scene3d_internal.h"

#include <stdio.h>
#include <string.h>

#define SNES_TEX_W 768   /* (256 + 2 * 64 widescreen columns) * 2 */
#define SNES_TEX_H 480   /* PPU writes up to 480 rows (see snesrecomp.c) */

/* Full-screen triangle from gl_VertexID (no vertex buffer). p ∈ {0,2}², so
 * the triangle covers the [0,1]² viewport. u_rect maps p to source UVs;
 * u_flip_y selects top-down (CPU framebuffer) vs GL bottom-up sampling. */
static const char *kFullscreenVS =
    "uniform vec4 u_rect;\n"
    "uniform float u_flip_y;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    float ty = mix(p.y, 1.0 - p.y, u_flip_y);\n"
    "    v_uv = u_rect.xy + vec2(p.x, ty) * u_rect.zw;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* Sharp-bilinear: snap to the nearest integer prescale, then let the
 * hardware bilinear filter only blend the fractional seam between texels.
 * Crisp pixels at non-integer scales (e.g. 640x480 → 1440x1080 = 2.25x). */
#define SHARP_BILINEAR_GLSL \
    "vec2 sharp_uv(vec2 uv, vec2 src, vec2 dst) {\n" \
    "    vec2 texel = uv * src;\n" \
    "    vec2 scale = max(floor(dst / src), vec2(1.0));\n" \
    "    vec2 range = 0.5 - 0.5 / scale;\n" \
    "    vec2 d = fract(texel) - 0.5;\n" \
    "    vec2 f = (d - clamp(d, -range, range)) * scale + 0.5;\n" \
    "    return (floor(texel) + f) / src;\n" \
    "}\n"

/* LakeSnes emits XBGR bytes (R in byte 3); uploaded as RGBA that lands as
 * .r=X .g=B .b=G .a=R, so swizzle back. */
static const char *kSnesFS =
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "    o_color = vec4(texture(u_tex, v_uv).wzy, 1.0);\n"
    "}\n";

static const char *kFxaaFS =
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_rcp;\n"
    "void main() {\n"
    "    const vec3 L = vec3(0.299, 0.587, 0.114);\n"
    "    vec3 nw = texture(u_tex, v_uv + vec2(-1.0, -1.0) * u_rcp).rgb;\n"
    "    vec3 ne = texture(u_tex, v_uv + vec2( 1.0, -1.0) * u_rcp).rgb;\n"
    "    vec3 sw = texture(u_tex, v_uv + vec2(-1.0,  1.0) * u_rcp).rgb;\n"
    "    vec3 se = texture(u_tex, v_uv + vec2( 1.0,  1.0) * u_rcp).rgb;\n"
    "    vec3 m  = texture(u_tex, v_uv).rgb;\n"
    "    float lnw = dot(nw, L), lne = dot(ne, L), lsw = dot(sw, L), lse = dot(se, L), lm = dot(m, L);\n"
    "    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse)));\n"
    "    float lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));\n"
    "    vec2 dir = vec2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));\n"
    "    float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);\n"
    "    float rcp_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
    "    dir = clamp(dir * rcp_min, vec2(-8.0), vec2(8.0)) * u_rcp;\n"
    "    vec3 a = 0.5 * (texture(u_tex, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +\n"
    "                    texture(u_tex, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);\n"
    "    vec3 b = a * 0.5 + 0.25 * (texture(u_tex, v_uv - dir * 0.5).rgb +\n"
    "                               texture(u_tex, v_uv + dir * 0.5).rgb);\n"
    "    float lb = dot(b, L);\n"
    "    o_color = vec4((lb < lmin || lb > lmax) ? a : b, 1.0);\n"
    "}\n";

static const char *kOutputFS =
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_src;\n"
    "uniform vec2 u_dst;\n"
    "uniform int u_sharp;\n"
    "uniform float u_scanlines;\n"
    SHARP_BILINEAR_GLSL
    "void main() {\n"
    "    vec2 uv = (u_sharp != 0) ? sharp_uv(v_uv, u_src, u_dst) : v_uv;\n"
    "    vec3 c = texture(u_tex, uv).rgb;\n"
    "    if (u_scanlines > 0.0) {\n"
    "        float l = fract(v_uv.y * 224.0);\n"
    "        c *= mix(1.0, 0.6 + 0.4 * sin(l * 3.14159265), u_scanlines);\n"
    "    }\n"
    "    o_color = vec4(c, 1.0);\n"
    "}\n";

/* Full-screen layout floor: every fragment below the horizon is a ray onto
 * the ground plane through the reconstructed camera; the hit point indexes the
 * Mode-7 tilemap -> character -> palette exactly like the PPU, but at the
 * internal target's resolution and over lines the SNES never drew. */
static const char *kFloorFS =
    "out vec4 o_color;\n"
    "uniform sampler2D u_tilemap;\n"
    "uniform sampler2D u_chars;\n"
    "uniform sampler2D u_pal;\n"
    "uniform vec2  u_target;\n"     /* internal target px */
    "uniform vec2  u_pic;\n"        /* presented cols, lines */
    "uniform float u_offset;\n"
    "uniform vec2  u_cam;\n"
    "uniform vec2  u_fwd;\n"
    "uniform vec2  u_right;\n"
    "uniform vec3  u_fhy;\n"        /* focal, height, horizon */
    "uniform float u_ext;\n"
    "uniform int   u_outside;\n"
    "int idx(sampler2D t, ivec2 p) { return int(texelFetch(t, p, 0).r * 255.0 + 0.5); }\n"
    "void main() {\n"
    "    float U = gl_FragCoord.x / u_target.x * u_pic.x;\n"
    "    float V = (u_target.y - gl_FragCoord.y) / u_target.y * u_pic.y - u_offset;\n"
    "    float dv = V - u_fhy.z + 0.5;\n"
    "    if (dv <= 0.05) discard;\n"
    "    float depth = u_fhy.x * u_fhy.y / dv;\n"
    "    float lat = (U - 128.5 - u_ext) * depth / u_fhy.x;\n"
    "    vec2 w = u_cam + u_fwd * depth + u_right * lat;\n"
    "    ivec2 wi = ivec2(floor(w));\n"
    "    bool outside = wi.x < 0 || wi.y < 0 || wi.x > 1023 || wi.y > 1023;\n"
    "    int tile = 0;\n"
    "    if (outside && u_outside == 1) { o_color = vec4(texelFetch(u_pal, ivec2(0, 0), 0).rgb, 1.0); return; }\n"
    "    if (outside && u_outside == 0) wi = wi & 1023;\n"
    "    if (!outside || u_outside == 0) tile = idx(u_tilemap, wi >> 3);\n"
    "    ivec2 c = ivec2((tile & 15) * 8, (tile >> 4) * 8) + (wi & 7);\n"
    "    o_color = vec4(texelFetch(u_pal, ivec2(idx(u_chars, c), 0), 0).rgb, 1.0);\n"
    "}\n";

typedef struct {
    GLuint prog;
    GLint  u_rect, u_flip_y, u_tex;
} Pass;

typedef struct {
    GLuint fbo, color, depth;
} Target;

static struct {
    bool   ready;
    GLuint vao;              /* empty VAO — required by core profile */
    GLuint snes_tex;
    float  snes_rect[4];     /* active picture, in UV units */
    Pass   snes, fxaa, out;
    GLint  fxaa_rcp;
    GLint  out_src, out_dst, out_sharp, out_scan;
    Target internal, post;
    int    int_w, int_h;     /* internal target size */
    Pass   floor;
    GLint  f_tilemap, f_chars, f_pal, f_target, f_pic, f_offset, f_cam, f_fwd, f_right,
           f_fhy, f_ext, f_outside;
    GLuint tex_tilemap, tex_chars, tex_pal;
} R;

/* Full-screen frame data, copied at submit (outside R: survives GL re-init). */
static RendererM7Frame s_m7;
static bool s_m7_valid = false;
static bool s_m7_dirty = false;

void renderer_set_m7_frame(const RendererM7Frame *f) {
    s_m7_valid = f != NULL;
    if (f) { s_m7 = *f; s_m7_dirty = true; }
}

static bool make_pass(Pass *p, const char *name, const char *fs) {
    p->prog = smkgl_build_program(name, kFullscreenVS, fs, NULL);
    if (!p->prog) return false;
    p->u_rect   = glGetUniformLocation(p->prog, "u_rect");
    p->u_flip_y = glGetUniformLocation(p->prog, "u_flip_y");
    p->u_tex    = glGetUniformLocation(p->prog, "u_tex");
    return true;
}

static GLuint make_texture(int w, int h, GLint filter, const void *pixels) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return t;
}

static GLuint make_r8_texture(int w, int h) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return t;
}

static bool make_target(Target *t, int w, int h, bool with_depth) {
    t->color = make_texture(w, h, GL_LINEAR, NULL);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);
    t->depth = 0;
    if (with_depth) {
        glGenRenderbuffers(1, &t->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "renderer: framebuffer incomplete (0x%04X)\n", st);
        return false;
    }
    return true;
}

static void free_target(Target *t) {
    if (t->fbo)   glDeleteFramebuffers(1, &t->fbo);
    if (t->color) glDeleteTextures(1, &t->color);
    if (t->depth) glDeleteRenderbuffers(1, &t->depth);
    memset(t, 0, sizeof(*t));
}

static bool make_targets(int w, int h) {
    free_target(&R.internal);
    free_target(&R.post);
    R.int_w = w;
    R.int_h = h;
    return make_target(&R.internal, w, h, true) && make_target(&R.post, w, h, false);
}

bool renderer_init(void) {
    memset(&R, 0, sizeof(R));
    glGenVertexArrays(1, &R.vao);

    if (!make_pass(&R.snes, "snes", kSnesFS) ||
        !make_pass(&R.fxaa, "fxaa", kFxaaFS) ||
        !make_pass(&R.out,  "output", kOutputFS)) {
        renderer_shutdown();
        return false;
    }
    R.fxaa_rcp  = glGetUniformLocation(R.fxaa.prog, "u_rcp");
    R.out_src   = glGetUniformLocation(R.out.prog, "u_src");
    R.out_dst   = glGetUniformLocation(R.out.prog, "u_dst");
    R.out_sharp = glGetUniformLocation(R.out.prog, "u_sharp");
    R.out_scan  = glGetUniformLocation(R.out.prog, "u_scanlines");

    if (!make_pass(&R.floor, "m7floor", kFloorFS)) {
        renderer_shutdown();
        return false;
    }
    R.f_tilemap = glGetUniformLocation(R.floor.prog, "u_tilemap");
    R.f_chars   = glGetUniformLocation(R.floor.prog, "u_chars");
    R.f_pal     = glGetUniformLocation(R.floor.prog, "u_pal");
    R.f_target  = glGetUniformLocation(R.floor.prog, "u_target");
    R.f_pic     = glGetUniformLocation(R.floor.prog, "u_pic");
    R.f_offset  = glGetUniformLocation(R.floor.prog, "u_offset");
    R.f_cam     = glGetUniformLocation(R.floor.prog, "u_cam");
    R.f_fwd     = glGetUniformLocation(R.floor.prog, "u_fwd");
    R.f_right   = glGetUniformLocation(R.floor.prog, "u_right");
    R.f_fhy     = glGetUniformLocation(R.floor.prog, "u_fhy");
    R.f_ext     = glGetUniformLocation(R.floor.prog, "u_ext");
    R.f_outside = glGetUniformLocation(R.floor.prog, "u_outside");
    R.tex_tilemap = make_r8_texture(128, 128);
    R.tex_chars   = make_r8_texture(128, 128);
    R.tex_pal     = make_texture(256, 1, GL_NEAREST, NULL);
    s_m7_dirty = true;

    R.snes_tex = make_texture(SNES_TEX_W, SNES_TEX_H, GL_LINEAR, NULL);
    R.snes_rect[0] = 0.0f;
    R.snes_rect[1] = 0.0f;
    R.snes_rect[2] = 512.0f / SNES_TEX_W;
    R.snes_rect[3] = 448.0f / SNES_TEX_H;

    if (!make_targets(SMK_INTERNAL_W, SMK_INTERNAL_H)) {
        renderer_shutdown();
        return false;
    }
    if (!scene3d_init()) {
        renderer_shutdown();
        return false;
    }
    R.ready = true;
    return true;
}

void renderer_shutdown(void) {
    scene3d_shutdown();
    if (R.snes.prog) glDeleteProgram(R.snes.prog);
    if (R.floor.prog) glDeleteProgram(R.floor.prog);
    if (R.tex_tilemap) glDeleteTextures(1, &R.tex_tilemap);
    if (R.tex_chars)   glDeleteTextures(1, &R.tex_chars);
    if (R.tex_pal)     glDeleteTextures(1, &R.tex_pal);
    if (R.fxaa.prog) glDeleteProgram(R.fxaa.prog);
    if (R.out.prog)  glDeleteProgram(R.out.prog);
    if (R.snes_tex)  glDeleteTextures(1, &R.snes_tex);
    if (R.vao)       glDeleteVertexArrays(1, &R.vao);
    free_target(&R.internal);
    free_target(&R.post);
    memset(&R, 0, sizeof(R));
}

void renderer_upload_snes(const uint8_t *fb, int width, int y, int h) {
    if (!R.ready || !fb || width <= 0 || width > SNES_TEX_W) return;
    if (h > SNES_TEX_H) h = SNES_TEX_H;

    /* Internal target follows the frame's aspect: 2.5 px per SNES column
     * (256 → 640 for 4:3), always 480 lines. */
    int iw = ((width / 2) * 5 + 2) / 4 * 2;   /* cols * 2.5, rounded to even */
    if (iw > SMK_INTERNAL_MAX_W) iw = SMK_INTERNAL_MAX_W;
    if (iw != R.int_w && !make_targets(iw, SMK_INTERNAL_H)) {
        fprintf(stderr, "renderer: resizing internal target to %dx%d failed\n", iw, SMK_INTERNAL_H);
        R.ready = false;
        return;
    }

    glBindTexture(GL_TEXTURE_2D, R.snes_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    /* Rows are exactly width*4 bytes, so the active band is one block. */
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, h, GL_RGBA, GL_UNSIGNED_BYTE,
                    fb + (size_t)y * width * 4);
    R.snes_rect[0] = 0.0f;
    R.snes_rect[1] = 0.0f;
    R.snes_rect[2] = (float)width / SNES_TEX_W;
    R.snes_rect[3] = (float)h / SNES_TEX_H;
}

void renderer_get_internal_size(int *w, int *h) {
    if (w) *w = R.int_w;
    if (h) *h = R.int_h;
}

static void draw_pass(const Pass *p, GLuint tex, const float rect[4], bool flip_y) {
    glUseProgram(p->prog);
    glUniform4f(p->u_rect, rect[0], rect[1], rect[2], rect[3]);
    glUniform1f(p->u_flip_y, flip_y ? 1.0f : 0.0f);
    glUniform1i(p->u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* Full-screen Mode-7 composition into the bound internal target. */
static void draw_fullscreen_m7(void) {
    const RendererM7Frame *m = &s_m7;
    glClearColor(m->sky[0], m->sky[1], m->sky[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (s_m7_dirty) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, R.tex_tilemap);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RED, GL_UNSIGNED_BYTE, m->tilemap);
        glBindTexture(GL_TEXTURE_2D, R.tex_chars);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RED, GL_UNSIGNED_BYTE, m->chars);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, R.tex_pal);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, m->palette);
        s_m7_dirty = false;
    }

    /* ground below the horizon (the race band is drawn over its part) */
    glUseProgram(R.floor.prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, R.tex_tilemap);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, R.tex_chars);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, R.tex_pal);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(R.f_tilemap, 0);
    glUniform1i(R.f_chars, 1);
    glUniform1i(R.f_pal, 2);
    glUniform2f(R.f_target, (float)R.int_w, (float)R.int_h);
    glUniform2f(R.f_pic, (float)m->cols, (float)m->lines);
    glUniform1f(R.f_offset, (float)m->line_offset);
    glUniform2f(R.f_cam, m->cam[0], m->cam[1]);
    glUniform2f(R.f_fwd, m->fwd[0], m->fwd[1]);
    glUniform2f(R.f_right, m->right[0], m->right[1]);
    glUniform3f(R.f_fhy, m->focal, m->height, m->horizon);
    glUniform1f(R.f_ext, (float)((m->cols - 256) / 2));
    glUniform1i(R.f_outside, m->outside);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* the SNES race band, 1:1, shifted down by line_offset */
    int top = (int)((float)(m->band_first - 1 + m->line_offset) / m->lines * R.int_h + 0.5f);
    int bot = (int)((float)(m->band_last + m->line_offset) / m->lines * R.int_h + 0.5f);
    if (bot > top) {
        /* inset half a texel top and bottom: the bilinear filter must not
         * pull in the blank rows just outside the band */
        float half = 0.5f / SNES_TEX_H;
        float rect[4] = {
            R.snes_rect[0],
            R.snes_rect[1] + R.snes_rect[3] * (float)(m->band_first - 1) / m->lines + half,
            R.snes_rect[2],
            R.snes_rect[3] * (float)(m->band_last - m->band_first + 1) / m->lines - 2.0f * half,
        };
        glViewport(0, R.int_h - bot, R.int_w, bot - top);
        draw_pass(&R.snes, R.snes_tex, rect, true);
        glViewport(0, 0, R.int_w, R.int_h);
    }
}

void renderer_draw(int out_w, int out_h, int top_inset,
                   float bar_r, float bar_g, float bar_b,
                   const RendererSettings *s) {
    if (!R.ready) return;
    static const float kFull[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

    glBindVertexArray(R.vao);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    /* [1] internal 640x480 target */
    glBindFramebuffer(GL_FRAMEBUFFER, R.internal.fbo);
    glViewport(0, 0, R.int_w, R.int_h);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    if (s_m7_valid) {
        draw_fullscreen_m7();
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_pass(&R.snes, R.snes_tex, R.snes_rect, true);
    }
    if (scene3d_has_hook()) {
        scene3d_run_hook(R.int_w, R.int_h);
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(R.vao);
    }

    /* [2] FXAA */
    GLuint final_tex = R.internal.color;
    if (s && s->fxaa) {
        glBindFramebuffer(GL_FRAMEBUFFER, R.post.fbo);
        glViewport(0, 0, R.int_w, R.int_h);
        glUseProgram(R.fxaa.prog);
        glUniform2f(R.fxaa_rcp, 1.0f / R.int_w, 1.0f / R.int_h);
        draw_pass(&R.fxaa, R.internal.color, kFull, false);
        final_tex = R.post.color;
    }

    /* [3] output: aspect-fit the internal target inside the area below the
     * menu bar (pillarbox / letterbox only when the aspects differ) */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, out_w, out_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (top_inset > 0) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, out_h - top_inset, out_w, top_inset);
        glClearColor(bar_r, bar_g, bar_b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    }
    int avail_h = out_h - top_inset;
    if (avail_h < 1) avail_h = 1;
    int vw = out_w, vh = avail_h;
    if ((long)out_w * R.int_h > (long)avail_h * R.int_w)
        vw = (int)((long)avail_h * R.int_w / R.int_h);              /* pillarbox */
    else
        vh = (int)((long)out_w * R.int_h / R.int_w);                /* letterbox */
    int vx = (out_w - vw) / 2;
    int vy = (avail_h - vh) / 2;
    glViewport(vx, vy, vw, vh);

    int filter = s ? s->filter : SMK_FILTER_SHARP;
    glBindTexture(GL_TEXTURE_2D, final_tex);
    GLint f = (filter == SMK_FILTER_NEAREST) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);

    glUseProgram(R.out.prog);
    glUniform2f(R.out_src, (float)R.int_w, (float)R.int_h);
    glUniform2f(R.out_dst, (float)vw, (float)vh);
    glUniform1i(R.out_sharp, filter == SMK_FILTER_SHARP ? 1 : 0);
    glUniform1f(R.out_scan, (s && s->scanlines) ? 1.0f : 0.0f);
    draw_pass(&R.out, final_tex, kFull, false);
    glViewport(0, 0, out_w, out_h);
}

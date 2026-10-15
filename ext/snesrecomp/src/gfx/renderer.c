/*
 * renderer.c — OpenGL ES 3.0 frame compositor (see renderer.h).
 */
#include "renderer.h"
#include "gl.h"

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
    RendererSceneFn scene_fn;
    void  *scene_user;
} R;

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

    R.snes_tex = make_texture(SNES_TEX_W, SNES_TEX_H, GL_LINEAR, NULL);
    R.snes_rect[0] = 0.0f;
    R.snes_rect[1] = 0.0f;
    R.snes_rect[2] = 512.0f / SNES_TEX_W;
    R.snes_rect[3] = 448.0f / SNES_TEX_H;

    if (!make_targets(SMK_INTERNAL_W, SMK_INTERNAL_H)) {
        renderer_shutdown();
        return false;
    }
    R.ready = true;
    return true;
}

void renderer_shutdown(void) {
    if (R.snes.prog) glDeleteProgram(R.snes.prog);
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

void renderer_set_scene_hook(RendererSceneFn fn, void *user) {
    R.scene_fn = fn;
    R.scene_user = user;
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
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_pass(&R.snes, R.snes_tex, R.snes_rect, true);
    if (R.scene_fn) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        R.scene_fn(R.scene_user);
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

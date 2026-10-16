/*
 * renderer.c — OpenGL ES 3.0 frame compositor (see renderer.h).
 */
#include "renderer.h"
#include "gl.h"
#include "scene3d_internal.h"
#include "snesrecomp/m7view.h"

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

/* Separated sprite layer: drawn after the 3D layer, each sprite pixel at the
 * ground depth under the sprite's bottom line, so 3D objects and sprites
 * occlude each other. Texels: rgb, alpha = sprite bottom line (0 = none);
 * filtered by hand (bilinear coverage) to match the SNES layer. */
static const char *kSpriteFS =
    "out vec4 o_color;\n"
    "uniform highp sampler2D u_spr;\n"
    "uniform vec2 u_target;\n"
    "uniform vec2 u_pic;\n"           /* cols, lines */
    "uniform float u_offset;\n"       /* presented line = picture line + offset */
    "uniform vec4 u_band[4];\n"       /* line0, line1, horizon, focal * height */
    "uniform int u_nbands;\n"
    "uniform vec2 u_depth;\n"         /* ndc z = x - y / depth */
    "uniform vec4 u_front[4];\n"      /* per band: the player's kart box (picture) */
    "uniform vec4 u_front_bottom;\n"  /* ... and its bottom line (0 = none) */
    "void main() {\n"
    "    vec2 q = vec2(gl_FragCoord.x / u_target.x * u_pic.x,\n"
    "                  (u_target.y - gl_FragCoord.y) / u_target.y * u_pic.y - u_offset) - 0.5;\n"
    "    ivec2 i0 = ivec2(floor(q));\n"
    "    vec2 f = q - floor(q);\n"
    "    ivec2 hi = textureSize(u_spr, 0) - 1;\n"
    "    vec3 rgb = vec3(0.0);\n"
    "    float cov = 0.0, bottom = 0.0;\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "        ivec2 o = ivec2(k & 1, k >> 1);\n"
    "        vec4 t = texelFetch(u_spr, clamp(i0 + o, ivec2(0), hi), 0);\n"
    "        if (t.a == 0.0) continue;\n"
    "        float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);\n"
    "        rgb += t.rgb * w; cov += w;\n"
    "        bottom = max(bottom, t.a * 255.0);\n"
    "    }\n"
    "    if (cov < 0.02) discard;\n"
    "    float z = -1.0;\n"                /* in front unless on a band's ground */
    "    bool player = false;\n"            /* the player's own kart: never hidden */
    "    for (int b = 0; b < 4; b++) {\n"
    "        if (b >= u_nbands) break;\n"
    "        vec4 F = u_front[b];\n"
    "        if (u_front_bottom[b] > 0.0 && q.x >= F.x && q.x <= F.z && q.y >= F.y && q.y <= F.w &&\n"
    "            bottom >= F.y && bottom <= u_front_bottom[b] + 4.0) player = true;\n"
    "    }\n"
    "    for (int b = 0; b < 4; b++) {\n"
    "        if (b >= u_nbands) break;\n"
    "        vec4 B = u_band[b];\n"
    "        float dv = bottom - B.z;\n"
    "        if (!player && bottom >= B.x && bottom <= B.y + 16.0 && dv > 0.5) {\n"
    "            z = u_depth.x - u_depth.y / (B.w / dv);\n"
    "            break;\n"
    "        }\n"
    "    }\n"
    "    gl_FragDepth = clamp(z * 0.5 + 0.5, 0.0, 1.0);\n"
    "    o_color = vec4(rgb / cov, cov);\n"
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
    Pass   spr;
    GLint  s_spr, s_target, s_pic, s_offset, s_band, s_nbands, s_depth, s_front, s_front_bottom;
    GLuint tex_spr;
} R;

/* Separated sprite layer (copied at submit, uploaded at present). */
static uint8_t s_spr_px[(256 + 2 * 64) * 4 * 239];
static int     s_spr_cols = 0, s_spr_lines = 0, s_spr_offset = 0, s_spr_nbands = 0;
static RendererSpriteBand s_spr_band[4];
static bool    s_spr_dirty = false, s_spr_drawn = false;
static float   s_front_box[4][4];       /* per band: player's kart box, picture coords */
static float   s_front_bottom[4];

void renderer_set_sprite_front_box(int band, float x0, float y0, float x1, float y1, float bottom) {
    if (band < 0 || band > 3) return;
    s_front_box[band][0] = x0; s_front_box[band][1] = y0;
    s_front_box[band][2] = x1; s_front_box[band][3] = y1;
    s_front_bottom[band] = bottom;
}

void renderer_set_sprites(const uint8_t *pixels, int cols, int lines, int line_offset,
                          const RendererSpriteBand *bands, int nbands) {
    if (!pixels || cols <= 0 || lines <= 0 || cols > 256 + 2 * 64 || lines > 239) {
        s_spr_cols = s_spr_lines = 0;
        return;
    }
    memcpy(s_spr_px, pixels, (size_t)cols * lines * 4);
    s_spr_cols = cols;
    s_spr_lines = lines;
    s_spr_offset = line_offset;
    s_spr_nbands = nbands < 0 ? 0 : (nbands > 4 ? 4 : nbands);
    if (s_spr_nbands) memcpy(s_spr_band, bands, sizeof(*bands) * (size_t)s_spr_nbands);
    s_spr_dirty = true;
}

/* Separated HUD layer (copied at submit, uploaded at present). */
static uint8_t s_hud_px[(256 + 2 * 64) * 4 * 239];
static int     s_hud_cols = 0, s_hud_lines = 0;
static bool    s_hud_dirty = false;
static GLuint  s_hud_tex = 0;

void renderer_set_hud(const uint8_t *pixels, int cols, int lines) {
    if (!pixels || cols <= 0 || lines <= 0 || cols > 256 + 2 * 64 || lines > 239) {
        s_hud_cols = s_hud_lines = 0;
        return;
    }
    memcpy(s_hud_px, pixels, (size_t)cols * lines * 4);
    s_hud_cols = cols;
    s_hud_lines = lines;
    s_hud_dirty = true;
}

unsigned renderer_hud_texture(int *cols, int *lines) {
    if (!R.ready || s_hud_cols == 0) return 0;
    if (!s_hud_tex) {
        glGenTextures(1, &s_hud_tex);
        glBindTexture(GL_TEXTURE_2D, s_hud_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256 + 2 * 64, 240, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        s_hud_dirty = true;
    }
    if (s_hud_dirty) {
        glBindTexture(GL_TEXTURE_2D, s_hud_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s_hud_cols, s_hud_lines, GL_RGBA, GL_UNSIGNED_BYTE, s_hud_px);
        s_hud_dirty = false;
    }
    if (cols) *cols = s_hud_cols;
    if (lines) *lines = s_hud_lines;
    return s_hud_tex;
}

/* Lines above the race band (full-screen layout), copied at submit. */
static uint8_t s_above_px[(256 + 2 * 64) * 8 * RENDERER_ABOVE_MAX];
static int     s_above_cols = 0, s_above_lines = 0;
static bool    s_above_dirty = false;
static GLuint  s_above_tex = 0;

void renderer_set_above(const uint8_t *pixels, int cols, int lines) {
    if (!pixels || cols <= 0 || lines <= 0 || cols > 256 + 2 * 64 || lines > RENDERER_ABOVE_MAX) {
        s_above_cols = s_above_lines = 0;
        return;
    }
    memcpy(s_above_px, pixels, (size_t)cols * lines * 8);
    s_above_cols = cols;
    s_above_lines = lines;
    s_above_dirty = true;
}

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
    if (!make_pass(&R.spr, "sprites", kSpriteFS)) {
        renderer_shutdown();
        return false;
    }
    R.s_spr    = glGetUniformLocation(R.spr.prog, "u_spr");
    R.s_target = glGetUniformLocation(R.spr.prog, "u_target");
    R.s_pic    = glGetUniformLocation(R.spr.prog, "u_pic");
    R.s_offset = glGetUniformLocation(R.spr.prog, "u_offset");
    R.s_band   = glGetUniformLocation(R.spr.prog, "u_band");
    R.s_nbands = glGetUniformLocation(R.spr.prog, "u_nbands");
    R.s_depth  = glGetUniformLocation(R.spr.prog, "u_depth");
    R.s_front  = glGetUniformLocation(R.spr.prog, "u_front");
    R.s_front_bottom = glGetUniformLocation(R.spr.prog, "u_front_bottom");
    R.tex_spr  = make_texture(256 + 2 * 64, 240, GL_NEAREST, NULL);
    s_spr_dirty = true;
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
    if (s_hud_tex) { glDeleteTextures(1, &s_hud_tex); s_hud_tex = 0; }
    if (s_above_tex) { glDeleteTextures(1, &s_above_tex); s_above_tex = 0; }
    if (R.snes.prog) glDeleteProgram(R.snes.prog);
    if (R.floor.prog) glDeleteProgram(R.floor.prog);
    if (R.tex_spr) glDeleteTextures(1, &R.tex_spr);
    if (R.spr.prog) glDeleteProgram(R.spr.prog);
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

    /* the PPU's lines above the band (scenery and sprites the SNES screen
     * cut off), over the sky fill */
    if (s_above_lines > 0) {
        if (!s_above_tex) {
            s_above_tex = make_texture(SNES_TEX_W, RENDERER_ABOVE_MAX, GL_LINEAR, NULL);
            s_above_dirty = true;
        }
        if (s_above_dirty) {
            glBindTexture(GL_TEXTURE_2D, s_above_tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s_above_cols * 2, s_above_lines,
                            GL_RGBA, GL_UNSIGNED_BYTE, s_above_px);
            s_above_dirty = false;
        }
        int first = m->band_first - 1 + m->line_offset - s_above_lines;   /* 0-based */
        int atop = (int)((float)first / m->lines * R.int_h + 0.5f);
        int abot = (int)((float)(first + s_above_lines) / m->lines * R.int_h + 0.5f);
        if (atop < 0) atop = 0;
        if (abot > atop) {
            float half = 0.5f / RENDERER_ABOVE_MAX;
            float rect[4] = {
                0.0f, half,
                (float)(s_above_cols * 2) / SNES_TEX_W,
                (float)s_above_lines / RENDERER_ABOVE_MAX - 2.0f * half,
            };
            glViewport(0, R.int_h - abot, R.int_w, abot - atop);
            draw_pass(&R.snes, s_above_tex, rect, true);
            glViewport(0, 0, R.int_w, R.int_h);
        }
    }

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

void renderer_draw_sprites(void) {
    if (!R.ready || s_spr_drawn || s_spr_cols == 0) return;
    s_spr_drawn = true;
    if (s_spr_dirty) {
        glBindTexture(GL_TEXTURE_2D, R.tex_spr);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s_spr_cols, s_spr_lines, GL_RGBA, GL_UNSIGNED_BYTE, s_spr_px);
        s_spr_dirty = false;
    }
    glBindVertexArray(R.vao);
    glDisable(GL_SCISSOR_TEST);
    if (s_spr_nbands > 0) {       /* only the rows that can hold separated sprites */
        int l0 = s_spr_band[0].line0, l1 = s_spr_band[0].line1;
        for (int i = 1; i < s_spr_nbands; i++) {
            if (s_spr_band[i].line0 < l0) l0 = s_spr_band[i].line0;
            if (s_spr_band[i].line1 > l1) l1 = s_spr_band[i].line1;
        }
        l1 += 9;                  /* native layout: karts overhang the band */
        if (s_spr_offset > 0) l1 = s_spr_lines;   /* full-screen: front sprites anywhere */
        (void)l0;                 /* screen-space sprites above the band count too */
        int top = (int)((float)(s_spr_offset) / s_spr_lines * R.int_h);
        int bot = (int)((float)(l1 + s_spr_offset) / s_spr_lines * R.int_h + 1.0f);
        if (top < 0) top = 0;
        if (bot > R.int_h) bot = R.int_h;
        if (bot <= top) return;
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, R.int_h - bot, R.int_w, bot - top);
    }
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(R.spr.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, R.tex_spr);
    glUniform1i(R.s_spr, 0);
    glUniform2f(R.s_target, (float)R.int_w, (float)R.int_h);
    /* the texture is wider than the picture: scale columns to the texture */
    glUniform2f(R.s_pic, (float)s_spr_cols, (float)s_spr_lines);
    glUniform1f(R.s_offset, (float)s_spr_offset);
    float band[16] = { 0 };
    for (int i = 0; i < s_spr_nbands; i++) {
        band[i * 4 + 0] = (float)s_spr_band[i].line0;
        band[i * 4 + 1] = (float)s_spr_band[i].line1;
        band[i * 4 + 2] = s_spr_band[i].horizon;
        band[i * 4 + 3] = s_spr_band[i].focal_height;
    }
    glUniform4fv(R.s_band, 4, band);
    glUniform4fv(R.s_front, 4, &s_front_box[0][0]);
    glUniform4f(R.s_front_bottom, s_front_bottom[0], s_front_bottom[1], s_front_bottom[2], s_front_bottom[3]);
    memset(s_front_bottom, 0, sizeof(s_front_bottom));   /* set again each frame */
    glUniform1i(R.s_nbands, s_spr_nbands);
    const float n = M7VIEW_NEAR_Z, f = M7VIEW_FAR_Z;
    glUniform2f(R.s_depth, (f + n) / (f - n), 2.0f * f * n / (f - n));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
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
    s_spr_drawn = false;
    if (scene3d_has_hook()) {
        scene3d_run_hook(R.int_w, R.int_h);
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(R.vao);
    }
    renderer_draw_sprites();   /* if the hook didn't place it under its 2D overlay */
    glBindVertexArray(R.vao);

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

/*
 * scene3d.c — 3D layer over the SNES picture (see include/snesrecomp/scene3d.h).
 */
#include "snesrecomp/scene3d.h"
#include "snesrecomp/m7view.h"
#include "scene3d_internal.h"
#include "renderer.h"
#include "gl.h"
#include "snesrecomp/snesrecomp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Scene3DMesh {
    GLuint vao, vbo, ibo;
    int    nindices;
};

static const char *kMeshVS =
    "in vec3 a_pos;\n"
    "in vec3 a_nrm;\n"
    "in vec3 a_col;\n"
    "uniform mat4 u_clip;\n"
    "uniform mat4 u_model;\n"
    "uniform vec3 u_light;\n"
    "uniform vec4 u_tint;\n"
    "out vec3 v_col;\n"
    "out vec3 v_nrm;\n"
    "void main() {\n"
    "    vec3 n = normalize(mat3(u_model) * a_nrm);\n"
    "    float diff = max(dot(n, -u_light), 0.0);\n"
    "    v_col = a_col * u_tint.rgb * (0.42 + 0.68 * diff);\n"
    "    v_nrm = n;\n"
    "    gl_Position = u_clip * (u_model * vec4(a_pos, 1.0));\n"
    "}\n";

static const char *kMeshFS =
    "in vec3 v_col;\n"
    "in vec3 v_nrm;\n"
    "out vec4 o_color;\n"
    "uniform vec3 u_light;\n"
    "uniform vec3 u_eye;\n"
    "uniform vec4 u_tint;\n"
    "void main() {\n"
    "    vec3 n = normalize(v_nrm);\n"
    "    float spec = pow(max(dot(reflect(u_light, n), u_eye), 0.0), 24.0);\n"
    "    o_color = vec4(v_col + vec3(0.30 * spec), u_tint.a);\n"
    "}\n";

static const char *kBlobVS =
    "in vec2 a_pos;\n"
    "uniform mat4 u_clip;\n"
    "uniform vec4 u_blob;\n"            /* x, y, radius, z */
    "out vec2 v_q;\n"
    "void main() {\n"
    "    v_q = a_pos;\n"
    "    gl_Position = u_clip * vec4(u_blob.xy + a_pos * u_blob.z, u_blob.w, 1.0);\n"
    "}\n";

static const char *kBlobFS =
    "in vec2 v_q;\n"
    "out vec4 o_color;\n"
    "uniform float u_alpha;\n"
    "void main() {\n"
    "    float a = u_alpha * (1.0 - smoothstep(0.35, 1.0, length(v_q)));\n"
    "    o_color = vec4(0.0, 0.0, 0.0, a);\n"
    "}\n";

static const char *kUiVS =
    "in vec2 a_pos;\n"
    "uniform vec4 u_rect;\n"        /* centre xy, half-size zw, target px (top-left origin) */
    "uniform vec2 u_target;\n"
    "uniform vec4 u_uv;\n"          /* uv centre xy, centre + half-extent zw */
    "out vec2 v_uv;\n"
    "out vec2 v_q;\n"
    "void main() {\n"
    "    vec2 p = u_rect.xy + a_pos * u_rect.zw;\n"
    "    v_uv = mix(u_uv.xy, u_uv.zw, a_pos);\n"
    "    v_q = a_pos;\n"
    "    gl_Position = vec4(p.x / u_target.x * 2.0 - 1.0, 1.0 - p.y / u_target.y * 2.0, 0.0, 1.0);\n"
    "}\n";

static const char *kUiFS =
    "in vec2 v_uv;\n"
    "in vec2 v_q;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_mask;\n"
    "uniform int u_mode;\n"         /* 0 mask, 1 disc, 2 HUD class */
    "uniform int u_class;\n"
    "uniform vec4 u_fill;\n"
    "uniform vec4 u_edge;\n"
    "uniform float u_ring;\n"       /* disc: ring width as a fraction of radius */
    "uniform float u_aa;\n"         /* disc: one pixel as a fraction of radius */
    "void main() {\n"
    "    if (u_mode == 2) {\n"
    "        vec4 t = texture(u_mask, v_uv);\n"
    "        if ((int(t.a * 255.0 + 0.5) & 15) != u_class) discard;\n"
    "        o_color = vec4(t.rgb, u_fill.a);\n"
    "        return;\n"
    "    }\n"
    "    if (u_mode == 0) {\n"
    "        float m = texture(u_mask, v_uv).r;\n"
    "        float inside = smoothstep(0.42, 0.58, m);\n"
    "        float outline = smoothstep(0.08, 0.28, m) * (1.0 - inside);\n"
    "        float t = inside / max(inside + outline, 1e-4);\n"
    "        o_color = vec4(mix(u_edge.rgb, u_fill.rgb, t), u_fill.a * inside + u_edge.a * outline);\n"
    "    } else {\n"
    "        float d = length(v_q);\n"
    "        float a = 1.0 - smoothstep(1.0 - u_aa, 1.0, d);\n"
    "        float inner = 1.0 - smoothstep(1.0 - u_ring - u_aa, 1.0 - u_ring, d);\n"
    "        vec4 c = mix(u_edge, u_fill, inner);\n"
    "        o_color = vec4(c.rgb, c.a * a);\n"
    "    }\n"
    "}\n";

struct Scene3DTex { GLuint tex; int w, h; };

static struct {
    GLuint ui_prog;
    GLint  u_rect, u_target, u_uv, u_mask, u_mode, u_fill, u_edge, u_ring, u_aa, u_class;
    float  tw, th;
} U;

static struct {
    bool   ready;
    GLuint mesh_prog, blob_prog;
    GLint  m_clip, m_model, m_light, m_tint, m_eye;
    GLint  b_clip, b_blob, b_alpha;
    GLuint blob_vao, blob_vbo;
    float  clip[16];
    Scene3DHook hook;
    void  *hook_user;
} S;

bool scene3d_init(void) {
    static const char *const mesh_attr[] = { "a_pos", "a_nrm", "a_col", NULL };
    static const char *const blob_attr[] = { "a_pos", NULL };
    S.mesh_prog = smkgl_build_program("scene3d-mesh", kMeshVS, kMeshFS, mesh_attr);
    S.blob_prog = smkgl_build_program("scene3d-blob", kBlobVS, kBlobFS, blob_attr);
    if (!S.mesh_prog || !S.blob_prog) return false;
    S.m_clip  = glGetUniformLocation(S.mesh_prog, "u_clip");
    S.m_model = glGetUniformLocation(S.mesh_prog, "u_model");
    S.m_light = glGetUniformLocation(S.mesh_prog, "u_light");
    S.m_tint  = glGetUniformLocation(S.mesh_prog, "u_tint");
    S.m_eye   = glGetUniformLocation(S.mesh_prog, "u_eye");
    S.b_clip  = glGetUniformLocation(S.blob_prog, "u_clip");
    S.b_blob  = glGetUniformLocation(S.blob_prog, "u_blob");
    S.b_alpha = glGetUniformLocation(S.blob_prog, "u_alpha");

    static const float quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glGenVertexArrays(1, &S.blob_vao);
    glGenBuffers(1, &S.blob_vbo);
    glBindVertexArray(S.blob_vao);
    glBindBuffer(GL_ARRAY_BUFFER, S.blob_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glBindVertexArray(0);
    static const char *const ui_attr[] = { "a_pos", NULL };
    U.ui_prog = smkgl_build_program("scene3d-ui", kUiVS, kUiFS, ui_attr);
    if (!U.ui_prog) return false;
    U.u_rect   = glGetUniformLocation(U.ui_prog, "u_rect");
    U.u_target = glGetUniformLocation(U.ui_prog, "u_target");
    U.u_uv     = glGetUniformLocation(U.ui_prog, "u_uv");
    U.u_mask   = glGetUniformLocation(U.ui_prog, "u_mask");
    U.u_mode   = glGetUniformLocation(U.ui_prog, "u_mode");
    U.u_fill   = glGetUniformLocation(U.ui_prog, "u_fill");
    U.u_edge   = glGetUniformLocation(U.ui_prog, "u_edge");
    U.u_ring   = glGetUniformLocation(U.ui_prog, "u_ring");
    U.u_aa     = glGetUniformLocation(U.ui_prog, "u_aa");
    U.u_class  = glGetUniformLocation(U.ui_prog, "u_class");
    S.ready = true;
    return true;
}

void scene3d_shutdown(void) {
    if (S.mesh_prog) glDeleteProgram(S.mesh_prog);
    if (S.blob_prog) glDeleteProgram(S.blob_prog);
    if (U.ui_prog)   glDeleteProgram(U.ui_prog);
    memset(&U, 0, sizeof(U));
    if (S.blob_vbo)  glDeleteBuffers(1, &S.blob_vbo);
    if (S.blob_vao)  glDeleteVertexArrays(1, &S.blob_vao);
    Scene3DHook hook = S.hook;
    void *user = S.hook_user;
    memset(&S, 0, sizeof(S));
    S.hook = hook;            /* the hook survives GL re-initialisation */
    S.hook_user = user;
}

void scene3d_set_hook(Scene3DHook hook, void *user) {
    S.hook = hook;
    S.hook_user = user;
}

bool scene3d_has_hook(void) { return S.ready && S.hook != NULL; }

void scene3d_run_hook(int target_w, int target_h) {
    if (!scene3d_has_hook()) return;
    const M7View *v = snesrecomp_m7_view();
    Scene3DFrame f;
    f.target_w = target_w;
    f.target_h = target_h;
    f.pic_cols = v->cols > 0 ? v->cols : 256;
    f.pic_lines = v->lines > 0 ? v->lines : 224;
    S.hook(&f, S.hook_user);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

Scene3DMesh *scene3d_mesh_create(const float *verts, int nverts,
                                 const uint16_t *indices, int nindices) {
    Scene3DMesh *m = (Scene3DMesh *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    glGenVertexArrays(1, &m->vao);
    glGenBuffers(1, &m->vbo);
    glGenBuffers(1, &m->ibo);
    glBindVertexArray(m->vao);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nverts * 9 * sizeof(float), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)nindices * sizeof(uint16_t), indices, GL_STATIC_DRAW);
    for (int i = 0; i < 3; i++) {
        glEnableVertexAttribArray((GLuint)i);
        glVertexAttribPointer((GLuint)i, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float),
                              (void *)(intptr_t)(i * 3 * sizeof(float)));
    }
    glBindVertexArray(0);
    m->nindices = nindices;
    return m;
}

void scene3d_mesh_destroy(Scene3DMesh *m) {
    if (!m) return;
    glDeleteBuffers(1, &m->vbo);
    glDeleteBuffers(1, &m->ibo);
    glDeleteVertexArrays(1, &m->vao);
    free(m);
}

/* Append a flat-shaded quad (a, b, c, d counter-clockwise from outside). */
static void quad(float *v, int *nv, uint16_t *ix, int *ni, const float p[4][3],
                 const float n[3], float r, float g, float b) {
    int base = *nv;
    for (int k = 0; k < 4; k++) {
        float *o = v + (*nv) * 9;
        o[0] = p[k][0]; o[1] = p[k][1]; o[2] = p[k][2];
        o[3] = n[0];    o[4] = n[1];    o[5] = n[2];
        o[6] = r;       o[7] = g;       o[8] = b;
        (*nv)++;
    }
    static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
    for (int k = 0; k < 6; k++) ix[(*ni)++] = (uint16_t)(base + tri[k]);
}

Scene3DMesh *scene3d_mesh_cube(float r, float g, float b) {
    float v[24 * 9];
    uint16_t ix[36];
    int nv = 0, ni = 0;
    const float h = 0.5f;
    const float faces[6][4][3] = {
        { {-h,-h,1}, { h,-h,1}, { h, h,1}, {-h, h,1} },   /* top    +Z */
        { {-h, h,0}, { h, h,0}, { h,-h,0}, {-h,-h,0} },   /* bottom -Z */
        { { h,-h,0}, { h, h,0}, { h, h,1}, { h,-h,1} },   /* +X */
        { {-h, h,0}, {-h,-h,0}, {-h,-h,1}, {-h, h,1} },   /* -X */
        { { h, h,0}, {-h, h,0}, {-h, h,1}, { h, h,1} },   /* +Y */
        { {-h,-h,0}, { h,-h,0}, { h,-h,1}, {-h,-h,1} },   /* -Y */
    };
    const float normals[6][3] = { {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
    for (int f = 0; f < 6; f++) quad(v, &nv, ix, &ni, faces[f], normals[f], r, g, b);
    return scene3d_mesh_create(v, nv, ix, ni);
}

Scene3DMesh *scene3d_mesh_box(const float face_rgb[6][3]) {
    float v[24 * 9];
    uint16_t ix[36];
    int nv = 0, ni = 0;
    const float h = 0.5f;
    const float faces[6][4][3] = {
        { {-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h} },
        { {-h, h,-h}, { h, h,-h}, { h,-h,-h}, {-h,-h,-h} },
        { { h,-h,-h}, { h, h,-h}, { h, h, h}, { h,-h, h} },
        { {-h, h,-h}, {-h,-h,-h}, {-h,-h, h}, {-h, h, h} },
        { { h, h,-h}, {-h, h,-h}, {-h, h, h}, { h, h, h} },
        { {-h,-h,-h}, { h,-h,-h}, { h,-h, h}, {-h,-h, h} },
    };
    const float normals[6][3] = { {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0} };
    for (int f = 0; f < 6; f++)
        quad(v, &nv, ix, &ni, faces[f], normals[f], face_rgb[f][0], face_rgb[f][1], face_rgb[f][2]);
    return scene3d_mesh_create(v, nv, ix, ni);
}

Scene3DMesh *scene3d_mesh_pyramid(float r, float g, float b) {
    /* inverted square pyramid: tip at the origin, 1 x 1 cap at Z = 1 */
    const float h = 0.5f;
    const float tip[3] = { 0, 0, 0 };
    const float cap[4][3] = { {-h,-h,1}, { h,-h,1}, { h, h,1}, {-h, h,1} };
    float v[16 * 9];
    uint16_t ix[18];
    int nv = 0, ni = 0;
    for (int s = 0; s < 4; s++) {
        const float *a = cap[s], *c = cap[(s + 1) % 4];
        /* outward normal of triangle (tip, c, a) */
        float e1[3] = { c[0] - tip[0], c[1] - tip[1], c[2] - tip[2] };
        float e2[3] = { a[0] - tip[0], a[1] - tip[1], a[2] - tip[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const float *pts[3] = { tip, c, a };
        for (int k = 0; k < 3; k++) {
            float *o = v + nv * 9;
            o[0] = pts[k][0]; o[1] = pts[k][1]; o[2] = pts[k][2];
            o[3] = n[0] / len; o[4] = n[1] / len; o[5] = n[2] / len;
            o[6] = r; o[7] = g; o[8] = b;
            ix[ni++] = (uint16_t)nv++;
        }
    }
    const float up[3] = { 0, 0, 1 };
    quad(v, &nv, ix, &ni, cap, up, r, g, b);
    return scene3d_mesh_create(v, nv, ix, ni);
}

void scene3d_clip_lines(const Scene3DFrame *f, int line0, int line1) {
    int top = (int)((float)(line0 - 1) / f->pic_lines * f->target_h + 0.5f);
    int bot = (int)((float)line1 / f->pic_lines * f->target_h + 0.5f);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, f->target_h - bot, f->target_w, bot - top);
}

void scene3d_begin(const float clip[16], const float light_dir[3]) {
    memcpy(S.clip, clip, sizeof(S.clip));
    float l[3] = { light_dir[0], light_dir[1], light_dir[2] };
    float len = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (len > 0) { l[0] /= len; l[1] /= len; l[2] /= len; }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(S.mesh_prog);
    glUniformMatrix4fv(S.m_clip, 1, GL_FALSE, S.clip);
    glUniform3f(S.m_light, l[0], l[1], l[2]);
    /* eye direction: the camera looks along +depth, i.e. row 3 of clip */
    float ex = S.clip[3], ey = S.clip[7];
    float el = sqrtf(ex * ex + ey * ey);
    glUniform3f(S.m_eye, el > 0 ? -ex / el : 0, el > 0 ? -ey / el : 0, 0.0f);
}

void scene3d_draw(const Scene3DMesh *m, const float model[16], const float tint[4]) {
    if (!m) return;
    static const float white[4] = { 1, 1, 1, 1 };
    const float *t = tint ? tint : white;
    glUseProgram(S.mesh_prog);
    glDepthMask(GL_TRUE);
    glUniformMatrix4fv(S.m_model, 1, GL_FALSE, model);
    glUniform4f(S.m_tint, t[0], t[1], t[2], t[3]);
    glBindVertexArray(m->vao);
    glDrawElements(GL_TRIANGLES, m->nindices, GL_UNSIGNED_SHORT, (void *)0);
}

void scene3d_blob_shadow(float x, float y, float radius, float alpha) {
    glUseProgram(S.blob_prog);
    glDepthMask(GL_FALSE);   /* shadows don't occlude */
    glUniformMatrix4fv(S.b_clip, 1, GL_FALSE, S.clip);
    glUniform4f(S.b_blob, x, y, radius, 0.05f);
    glUniform1f(S.b_alpha, alpha);
    glBindVertexArray(S.blob_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDepthMask(GL_TRUE);
}

void scene3d_end(void) {
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
}

void scene3d_mat_trs(float out[16], float x, float y, float z,
                     float yaw_rad, float sx, float sy, float sz) {
    float c = cosf(yaw_rad), s = sinf(yaw_rad);
    memset(out, 0, 16 * sizeof(float));
    out[0] = c * sx;  out[1] = s * sx;
    out[4] = -s * sy; out[5] = c * sy;
    out[10] = sz;
    out[12] = x; out[13] = y; out[14] = z; out[15] = 1.0f;
}

void scene3d_mat_spin(float out[16], float x, float y, float z,
                      float yaw_rad, float tilt_rad, float scale) {
    float cy = cosf(yaw_rad), sy = sinf(yaw_rad);
    float ct = cosf(tilt_rad), st = sinf(tilt_rad);
    /* columns of Rz * Rx, scaled */
    out[0] = cy * scale;       out[1] = sy * scale;       out[2] = 0.0f;        out[3] = 0.0f;
    out[4] = -sy * ct * scale; out[5] = cy * ct * scale;  out[6] = st * scale;  out[7] = 0.0f;
    out[8] = sy * st * scale;  out[9] = -cy * st * scale; out[10] = ct * scale; out[11] = 0.0f;
    out[12] = x; out[13] = y; out[14] = z; out[15] = 1.0f;
}

/* ---- 2D overlay ---- */

Scene3DTex *scene3d_tex_r8(int w, int h) {
    Scene3DTex *t = (Scene3DTex *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->w = w; t->h = h;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return t;
}

void scene3d_tex_update(Scene3DTex *t, const uint8_t *data) {
    if (!t) return;
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->w, t->h, GL_RED, GL_UNSIGNED_BYTE, data);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

void scene3d_ui_begin(const Scene3DFrame *f) {
    U.tw = (float)f->target_w;
    U.th = (float)f->target_h;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(U.ui_prog);
    glUniform2f(U.u_target, U.tw, U.th);
    glUniform1i(U.u_mask, 0);
    glBindVertexArray(S.blob_vao);   /* unit quad, remapped below */
}

static void ui_quad(float x, float y, float w, float h) {
    /* blob_vao holds a -1..1 quad: map it onto [x, x+w] x [y, y+h] */
    glUniform4f(U.u_rect, x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void scene3d_ui_mask(const Scene3DTex *t, float x, float y, float w, float h,
                     const float uv0[2], const float uv1[2],
                     const float fill[4], const float edge[4]) {
    if (!t) return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glUniform1i(U.u_mode, 0);
    /* a_pos spans -1..1, so pass uv as centre and half-extent */
    glUniform4f(U.u_uv, (uv0[0] + uv1[0]) * 0.5f, (uv0[1] + uv1[1]) * 0.5f,
                (uv0[0] + uv1[0]) * 0.5f + (uv1[0] - uv0[0]) * 0.5f,
                (uv0[1] + uv1[1]) * 0.5f + (uv1[1] - uv0[1]) * 0.5f);
    glUniform4f(U.u_fill, fill[0], fill[1], fill[2], fill[3]);
    glUniform4f(U.u_edge, edge[0], edge[1], edge[2], edge[3]);
    ui_quad(x, y, w, h);
}

void scene3d_ui_disc(float cx, float cy, float r, const float fill[4],
                     const float ring[4], float ring_px) {
    glUniform1i(U.u_mode, 1);
    glUniform4f(U.u_fill, fill[0], fill[1], fill[2], fill[3]);
    glUniform4f(U.u_edge, ring[0], ring[1], ring[2], ring[3]);
    glUniform1f(U.u_ring, r > 0 ? ring_px / r : 0.0f);
    glUniform1f(U.u_aa, r > 0 ? 1.0f / r : 1.0f);
    ui_quad(cx - r, cy - r, 2.0f * r, 2.0f * r);
}

bool scene3d_ui_hud_size(int cls, float scale, float *w, float *h) {
    const SnesHudFrame *hf = snesrecomp_hud();
    if (!hf->valid || cls < 1 || cls > 3 || !hf->box[cls].present) return false;
    float bw = (float)(hf->box[cls].x1 - hf->box[cls].x0 + 1);
    float bh = (float)(hf->box[cls].y1 - hf->box[cls].y0 + 1);
    if (w) *w = bw * U.tw / hf->cols * scale;
    if (h) *h = bh * U.th / hf->lines * scale;
    return true;
}

void scene3d_ui_hud(int cls, float x, float y, float scale) {
    const SnesHudFrame *hf = snesrecomp_hud();
    float w, h;
    if (!scene3d_ui_hud_size(cls, scale, &w, &h)) return;
    int tc = 0, tl = 0;
    GLuint tex = renderer_hud_texture(&tc, &tl);
    if (!tex) return;
    const float TW = 256.0f + 2.0f * 64.0f, TH = 240.0f;   /* HUD texture size */
    float u0 = hf->box[cls].x0 / TW, v0 = hf->box[cls].y0 / TH;
    float u1 = (hf->box[cls].x1 + 1) / TW, v1 = (hf->box[cls].y1 + 1) / TH;

    float alpha = 1.0f;
    if (cls == SNES_HUD_MATH) {
        /* blend like the PPU's colour math (add / subtract / half-add) */
        glBlendEquation((hf->math_flags & 1) ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
        if (hf->math_flags & 2) {
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            alpha = 0.5f;
        }
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(U.u_mode, 2);
    glUniform1i(U.u_class, cls);
    glUniform4f(U.u_fill, 1.0f, 1.0f, 1.0f, alpha);
    glUniform4f(U.u_uv, (u0 + u1) * 0.5f, (v0 + v1) * 0.5f, u1, v1);
    ui_quad(x, y, w, h);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void scene3d_ui_end(void) {
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

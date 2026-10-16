/*
 * scene3d.h — minimal OpenGL ES 3.0 3D layer drawn over the SNES picture.
 *
 * Geometry is drawn into the renderer's internal target (640x480-class, with
 * depth) from a scene hook, after the SNES layer. Meshes are indexed triangle
 * lists with per-vertex colour; lighting is one directional light + ambient,
 * per-vertex diffuse with a cheap per-pixel specular term. Blob shadows are
 * alpha-blended radial quads on the ground plane.
 *
 * Coordinates: world units (Mode-7 map pixels), Z up. Matrices are 4x4
 * column-major float[16].
 */
#ifndef SNESRECOMP_SCENE3D_H
#define SNESRECOMP_SCENE3D_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Scene3DMesh Scene3DMesh;

typedef struct {
    int target_w, target_h;    /* internal render target size (pixels) */
    int pic_cols, pic_lines;   /* SNES picture size it shows (columns, lines) */
} Scene3DFrame;

/* Called once per presented frame with the internal target bound. */
typedef void (*Scene3DHook)(const Scene3DFrame *frame, void *user);

/* Install the per-frame 3D hook (NULL to remove). */
void scene3d_set_hook(Scene3DHook hook, void *user);

/* Vertex layout: position xyz, normal xyz, colour rgb (9 floats). */
Scene3DMesh *scene3d_mesh_create(const float *verts, int nverts,
                                 const uint16_t *indices, int nindices);
void scene3d_mesh_destroy(Scene3DMesh *m);

/* Built-in shapes (unit size, centred on the origin in XY, base at Z=0). */
Scene3DMesh *scene3d_mesh_cube(float r, float g, float b);     /* 1 x 1 x 1 */
Scene3DMesh *scene3d_mesh_pyramid(float r, float g, float b);  /* point down: marker */
/* Cube centred on the origin (-0.5..0.5 on every axis), one colour per face:
 * +Z, -Z, +X, -X, +Y, -Y. */
Scene3DMesh *scene3d_mesh_box(const float face_rgb[6][3]);

/* Restrict drawing to scanlines line0..line1 of the picture (clip a band). */
void scene3d_clip_lines(const Scene3DFrame *f, int line0, int line1);

/* Begin a batch with a clip matrix; light_dir points from the light (world). */
void scene3d_begin(const float clip[16], const float light_dir[3]);
void scene3d_draw(const Scene3DMesh *m, const float model[16], const float tint[4]);
/* Soft dark disc on the ground at (x, y), radius in world units. */
void scene3d_blob_shadow(float x, float y, float radius, float alpha);
void scene3d_end(void);

/* ---- 2D overlay (HUD) in internal-target pixels, origin top-left ---- */
typedef struct Scene3DTex Scene3DTex;
/* Single-channel (R8) texture, linearly filtered; update with w*h bytes. */
Scene3DTex *scene3d_tex_r8(int w, int h);
void scene3d_tex_update(Scene3DTex *t, const uint8_t *data);
void scene3d_ui_begin(const Scene3DFrame *f);
/* Shape from a mask: coverage = texture value. Inside (>= 0.5) gets `fill`,
 * a band just outside gets `edge` (outline). uv0/uv1 select the sub-rect. */
void scene3d_ui_mask(const Scene3DTex *t, float x, float y, float w, float h,
                     const float uv0[2], const float uv1[2],
                     const float fill[4], const float edge[4]);
/* Disc of radius r with a ring of `ring_px` pixels. */
void scene3d_ui_disc(float cx, float cy, float r, const float fill[4],
                     const float ring[4], float ring_px);
void scene3d_ui_end(void);

/* Matrix helpers. */
void scene3d_mat_trs(float out[16], float x, float y, float z,
                     float yaw_rad, float sx, float sy, float sz);
/* T(x,y,z) * Rz(yaw) * Rx(tilt) * uniform scale. */
void scene3d_mat_spin(float out[16], float x, float y, float z,
                      float yaw_rad, float tilt_rad, float scale);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_SCENE3D_H */

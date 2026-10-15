/*
 * m7view.c — fit a pinhole camera to the PPU's per-scanline Mode-7 state.
 * See include/snesrecomp/m7view.h for the model.
 */
#include "snesrecomp/m7view.h"
#include "m7view_internal.h"

#include <math.h>
#include <string.h>

#define MIN_BAND_LINES 8

/* Per-line samples of one band. */
typedef struct {
    double y;        /* scanline */
    double s;        /* world units per pixel */
    double cx, cy;   /* ground point under column 128 */
    double rx, ry;   /* unit screen-right on the ground */
} Sample;

/* Weighted least squares for v = k * u + o. */
static void fit_line(const double *u, const double *v, const double *w, int n,
                     double *k, double *o) {
    double sw = 0, su = 0, sv = 0, suu = 0, suv = 0;
    for (int i = 0; i < n; i++) {
        sw += w[i]; su += w[i] * u[i]; sv += w[i] * v[i];
        suu += w[i] * u[i] * u[i]; suv += w[i] * u[i] * v[i];
    }
    double det = sw * suu - su * su;
    if (fabs(det) < 1e-12) { *k = 0; *o = sw > 0 ? sv / sw : 0; return; }
    *k = (sw * suv - su * sv) / det;
    *o = (suu * sv - su * suv) / det;
}

static void solve_band(const Sample *sm, int n, M7Band *b) {
    double smin = 1e30, smax = 0;
    for (int i = 0; i < n; i++) {
        if (sm[i].s < smin) smin = sm[i].s;
        if (sm[i].s > smax) smax = sm[i].s;
    }
    b->perspective = false;
    if (n < MIN_BAND_LINES || smin <= 0 || smax / smin < 1.25) return;   /* flat / overhead */

    double u[240], v[240], w[240], vx[240], vy[240];
    /* y = y_h + H * (1/s): errors in scanlines */
    for (int i = 0; i < n; i++) { u[i] = 1.0 / sm[i].s; v[i] = sm[i].y; w[i] = 1.0; }
    double H, yh;
    fit_line(u, v, w, n, &H, &yh);
    if (H <= 0) return;

    /* ground(col 128) = cam + K * s, weighted by 1/s^2 so residuals are pixels */
    for (int i = 0; i < n; i++) {
        u[i] = sm[i].s; vx[i] = sm[i].cx; vy[i] = sm[i].cy;
        w[i] = 1.0 / (sm[i].s * sm[i].s);
    }
    double kx, ox, ky, oy;
    fit_line(u, vx, w, n, &kx, &ox);
    fit_line(u, vy, w, n, &ky, &oy);
    double f = hypot(kx, ky);
    if (f < 1.0) return;

    /* screen-right: mean of the per-line directions, made perpendicular to F */
    double fx = kx / f, fy = ky / f, rx = 0, ry = 0;
    for (int i = 0; i < n; i++) { rx += sm[i].rx; ry += sm[i].ry; }
    double side = rx * -fy + ry * fx;            /* which perpendicular */
    rx = side >= 0 ? -fy : fy;
    ry = side >= 0 ?  fx : -fx;

    double el = 0, ec = 0;
    for (int i = 0; i < n; i++) {
        double dy = sm[i].y - (yh + H / sm[i].s);
        double ex = sm[i].cx - (ox + kx * sm[i].s), ey = sm[i].cy - (oy + ky * sm[i].s);
        el += dy * dy;
        ec += (ex * ex + ey * ey) / (sm[i].s * sm[i].s);
    }
    b->perspective = true;
    b->cam[0] = (float)ox;   b->cam[1] = (float)oy;
    b->height = (float)H;
    b->fwd[0] = (float)fx;   b->fwd[1] = (float)fy;
    b->right[0] = (float)rx; b->right[1] = (float)ry;
    b->focal = (float)f;
    b->horizon = (float)yh;
    b->rms_line = (float)sqrt(el / n);
    b->rms_col = (float)sqrt(ec / n);
}

void m7view_solve(M7View *v, const PpuM7Line *lines, int cols, int nlines) {
    memset(v, 0, sizeof(*v));
    v->cols = cols;
    v->lines = nlines;
    Sample sm[240];
    int i = 0;
    while (i < nlines && v->nbands < M7VIEW_MAX_BANDS) {
        if (!lines[i].valid || lines[i].xFlip) { i++; continue; }
        int n = 0, start = i;
        /* a band is a run of mode-7 lines whose ground step changes smoothly */
        while (i < nlines && lines[i].valid && !lines[i].xFlip) {
            const PpuM7Line *l = &lines[i];
            double a = l->a / 256.0, c = l->c / 256.0, s = hypot(a, c);
            if (s <= 0) break;
            if (n > 0 && (s > sm[n - 1].s * 1.6 || s < sm[n - 1].s / 1.6) && n >= 2) break;
            sm[n].y = i + 1;
            sm[n].s = s;
            sm[n].cx = l->startX / 256.0 + a * 128.0;
            sm[n].cy = l->startY / 256.0 + c * 128.0;
            sm[n].rx = a / s;
            sm[n].ry = c / s;
            n++; i++;
        }
        if (n == 0) { i++; continue; }
        M7Band *b = &v->band[v->nbands++];
        b->line0 = start + 1;
        b->line1 = start + n;
        solve_band(sm, n, b);
    }
}

const M7Band *m7view_band_at(const M7View *v, int line) {
    for (int i = 0; i < v->nbands; i++)
        if (v->band[i].perspective && line >= v->band[i].line0 && line <= v->band[i].line1)
            return &v->band[i];
    return NULL;
}

/*
 * Picture coordinates (continuous): column U in [0, cols), line V in [0, lines)
 * from the top. Original column x (sampled at its centre x + 0.5) sits at
 * U = x + 0.5 + ext; scanline y is centred at V = y - 0.5.
 *   U = 128.5 + ext + f * lat / depth
 *   V = y_h - 0.5 + f * (H - z) / depth
 * NDC: x = 2U/cols - 1, y = 1 - 2V/lines.
 */
void m7view_clip_matrix(const M7View *v, const M7Band *b, float near_z, float far_z,
                        float out[16]) {
    double cols = v->cols, lines = v->lines;
    double Rx = b->right[0], Ry = b->right[1], Fx = b->fwd[0], Fy = b->fwd[1];
    double Rc = Rx * b->cam[0] + Ry * b->cam[1];   /* lat   = R.P - Rc */
    double Fc = Fx * b->cam[0] + Fy * b->cam[1];   /* depth = F.P - Fc */
    double f = b->focal, H = b->height;
    /* clip_x = depth/cols + (2f/cols) lat */
    double kx = 2.0 * f / cols, dx = 1.0 / cols;
    /* clip_y = depth (1 - 2(y_h - 0.5)/lines) + (2f/lines)(z - H) */
    double k1 = 1.0 - 2.0 * (b->horizon - 0.5) / lines, k2 = 2.0 * f / lines;
    /* clip_z = a depth - c */
    double a = (far_z + near_z) / (far_z - near_z), c = 2.0 * far_z * near_z / (far_z - near_z);
    double row[4][4] = {
        { kx * Rx + dx * Fx, kx * Ry + dx * Fy, 0.0, -(kx * Rc + dx * Fc) },
        { k1 * Fx,           k1 * Fy,           k2,  -(k1 * Fc) - k2 * H  },
        { a * Fx,            a * Fy,            0.0, -(a * Fc) - c        },
        { Fx,                Fy,                0.0, -Fc                  },
    };
    for (int r = 0; r < 4; r++)
        for (int col = 0; col < 4; col++)
            out[col * 4 + r] = (float)row[r][col];
}

bool m7view_project(const M7View *v, const M7Band *b, float x, float y, float z,
                    float *col, float *line) {
    double px = x - b->cam[0], py = y - b->cam[1];
    double depth = px * b->fwd[0] + py * b->fwd[1];
    if (depth <= 0.01) return false;
    double lat = px * b->right[0] + py * b->right[1];
    int ext = (v->cols - 256) / 2;
    if (col)  *col  = (float)(128.5 + ext + b->focal * lat / depth);
    if (line) *line = (float)(b->horizon - 0.5 + b->focal * (b->height - z) / depth);
    return true;
}

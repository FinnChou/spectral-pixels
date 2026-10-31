/*
 * light.c — implementation of the spectral lighting engine; see light.h for the interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * The colour of a pixel is the sum of the contributions of all lights:
 *   XYZ = tex · Σ_l E_l · T_l[row],   T_l[row] = Σ_λ Ŝ_l(λ) · R_row(λ) · (x̄, ȳ, z̄)(λ)
 * where Ŝ_l is the normalized spectrum of light l, R_row a row of the reflectance table and E_l the
 * illuminance. E_l depends only on geometry, not on wavelength, so it can be taken out of the
 * spectral sum: each pixel costs three multiply-adds per light, and the spectral sums are recomputed
 * only when a spectrum or a reflectance changes. Integrals are approximated by sums over the 5 nm
 * samples; the step Δλ cancels in the normalization.
 *
 * References
 *   [1] CIE 015:2018 Colorimetry, 4th ed. doi:10.25039/TR.015.2018
 *   [2] IEC 61966-2-1:1999 Multimedia systems and equipment — Colour measurement and management —
 *       Part 2-1: Colour management — Default RGB colour space — sRGB
 *   [3] K. Narkowicz, "ACES Filmic Tone Mapping Curve", 2016-01-06.
 *       https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
 *   [4] M. F. Cohen, S. E. Chen, J. R. Wallace, D. P. Greenberg, "A Progressive Refinement Approach to
 *       Fast Radiosity Image Generation", Computer Graphics (SIGGRAPH '88) 22(4):75–84, 1988.
 *       doi:10.1145/378456.378487
 */
#include <math.h>
#include <string.h>
#include "light.h"
#include "cmf.h"                                /* CMF[3][LIGHT_NW]: x̄, ȳ, z̄ */
#include "srgb_lut.h"                           /* SRGB_LUT[255]: 8-bit thresholds of the sRGB encoding */

enum { W = LIGHT_W, H = LIGHT_H, N = LIGHT_W * LIGHT_H };
enum { OFF, POINT, SUN, AREA };
enum { AREA_LONG = 8, AREA_SHORT = 4 };         /* strata of an area light along its long and short edge */
#define HARD_STEP 0.5f                          /* horizontal step of occlusion tests for point and directional
                                                   lights, in pixels */
#define SOFT_STEP 1.0f                          /* step for area lights: the penumbra averages 32 samples, so
                                                   the coarser quantization is not visible */
#define RHO_MAX 0.95f                           /* cap on the mean reflectance, keeps 1/(1 − ρ̄) finite */

typedef struct {                                /* everything the geometric factor depends on */
  int kind;
  float pos[3];                                 /* point: position; directional: unit vector towards the
                                                   light; area: centre */
  float u[3], v[3];                             /* area: half-edges */
} Shape;

typedef struct {
  Shape shape;
  float axis_long[3], axis_short[3];            /* area: sampling axes (the longer and the shorter half-edge) */
  float normal[3], area;                        /* area: unit normal and area of the emitting patch */
  float strength;                               /* intensity, illuminance or luminance, depending on the kind */
  float spd[LIGHT_NW];                          /* normalized spectrum: Σ spd · ȳ = 1 */
  float T[LIGHT_ROWS][3];                       /* XYZ of each reflectance row under unit illuminance */
  int has_spectrum;
  int cached;                                   /* geometry_cache[l] is valid for shape */
} Light;

static LightSurface surfaces[N];
static float heights[N], gaps[2 * N];
static float ceiling = INFINITY;
static float reflectance[LIGHT_ROWS][LIGHT_NW];
static Light lights[LIGHT_MAX];
static float geometry_cache[LIGHT_MAX][N];      /* illuminance = strength × geometric factor */
static float ambient_k = 1, exposure = 1;

static unsigned char frame[4 * N];
static float probe_buf[2 * LIGHT_NW];
static unsigned char rgb_buf[3];
static float scratch[LIGHT_SCRATCH * LIGHT_NW];

/* ---------- Vectors ---------- */
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float norm3(const float a[3]) { return sqrtf(dot3(a, a)); }
static void cross3(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

/* ---------- Spectra ---------- */
static void integrate(const float *spd, const float *refl, float xyz[3]) {
  for (int c = 0; c < 3; c++) {
    float sum = 0;
    for (int k = 0; k < LIGHT_NW; k++) sum += spd[k] * refl[k] * CMF[c][k];
    xyz[c] = sum;
  }
}

static float luminance(const float *spd) {
  float y = 0;
  for (int k = 0; k < LIGHT_NW; k++) y += spd[k] * CMF[1][k];
  return y;
}

LIGHT_API void light_set_spectrum(int l, const float *spd) {
  if (l < 0 || l >= LIGHT_MAX) return;
  Light *L = &lights[l];
  float y = luminance(spd);
  L->has_spectrum = y > 0;
  for (int k = 0; k < LIGHT_NW; k++) L->spd[k] = L->has_spectrum ? spd[k] / y : 0;
  for (int r = 0; r < LIGHT_ROWS; r++) integrate(L->spd, reflectance[r], L->T[r]);
}

LIGHT_API void light_set_reflectance(int row, const float *refl) {
  if (row < 0 || row >= LIGHT_ROWS) return;
  memcpy(reflectance[row], refl, sizeof reflectance[row]);
  for (int l = 0; l < LIGHT_MAX; l++) integrate(lights[l].spd, reflectance[row], lights[l].T[row]);
}

/* ---------- Lights ---------- */
static void set_shape(int l, const Shape *shape, float strength) {
  Light *L = &lights[l];
  if (memcmp(&L->shape, shape, sizeof *shape) != 0) { L->shape = *shape; L->cached = 0; }
  L->strength = strength;
}

static void new_shape(Shape *s, int kind, float x, float y, float z) {
  memset(s, 0, sizeof *s);                      /* zero u and v, unused by points and directional lights,
                                                   so that memcmp compares only meaningful data */
  s->kind = kind; s->pos[0] = x; s->pos[1] = y; s->pos[2] = z;
}

LIGHT_API void light_point(int l, float x, float y, float z, float intensity) {
  if (l < 0 || l >= LIGHT_MAX) return;
  Shape s;
  new_shape(&s, POINT, x, y, z);
  set_shape(l, &s, intensity);
}

LIGHT_API void light_sun(int l, float dx, float dy, float dz, float illuminance) {
  if (l < 0 || l >= LIGHT_MAX) return;
  float d[3] = { dx, dy, dz }, len = norm3(d);
  if (!(len > 0)) { lights[l].shape.kind = OFF; return; }
  Shape s;
  new_shape(&s, SUN, dx / len, dy / len, dz / len);
  set_shape(l, &s, illuminance);
}

LIGHT_API void light_area(int l, float cx, float cy, float cz, float ux, float uy, float uz,
                          float vx, float vy, float vz, float luminance) {
  if (l < 0 || l >= LIGHT_MAX) return;
  Light *L = &lights[l];
  Shape s;
  new_shape(&s, AREA, cx, cy, cz);
  s.u[0] = ux; s.u[1] = uy; s.u[2] = uz;
  s.v[0] = vx; s.v[1] = vy; s.v[2] = vz;
  float n[3];
  cross3(s.u, s.v, n);
  float len = norm3(n);
  if (!(len > 0)) { L->shape.kind = OFF; return; }
  for (int c = 0; c < 3; c++) L->normal[c] = n[c] / len;
  L->area = 4 * len;                            /* area of the parallelogram, |2u × 2v| */
  int u_long = norm3(s.u) >= norm3(s.v);
  memcpy(L->axis_long, u_long ? s.u : s.v, sizeof L->axis_long);
  memcpy(L->axis_short, u_long ? s.v : s.u, sizeof L->axis_short);
  set_shape(l, &s, luminance);
}

LIGHT_API void light_off(int l) { if (l >= 0 && l < LIGHT_MAX) lights[l].shape.kind = OFF; }
LIGHT_API void light_set_ambient(float k) { ambient_k = k > 0 ? k : 0; }
LIGHT_API void light_set_exposure(float k) { exposure = k > 0 ? k : 0; }

LIGHT_API void light_set_ceiling(float z) {
  if (z != ceiling) { ceiling = z; light_invalidate(); }
}

LIGHT_API void light_invalidate(void) {
  for (int l = 0; l < LIGHT_MAX; l++) lights[l].cached = 0;
}

/* ---------- Occlusion ---------- */
/* Whether the point (x, y, z), inside the grid, is solid: above the ceiling, or below the height field
 * and not inside a gap. */
static int solid(float x, float y, float z) {
  int i = (int)y * W + (int)x;
  return z > ceiling || (z < heights[i] && !(z > gaps[2 * i] && z < gaps[2 * i + 1]));
}

/* Whether the segment p → q is unobstructed. The occluders form a height field, so the segment is
 * sampled every `step` pixels of horizontal length; at least one interior sample is taken so that
 * nearly vertical segments are still stopped by the ceiling. Once the segment leaves the grid (a
 * convex rectangle) it cannot re-enter it, and nothing outside the grid blocks light, so the segment
 * is then visible. The start point is not tested; the caller keeps it outside solid space. */
static int unobstructed(const float p[3], const float q[3], float step) {
  float d[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
  int n = (int)(sqrtf(d[0] * d[0] + d[1] * d[1]) / step) + 2;
  for (int k = 1; k < n; k++) {
    float t = (float)k / n, x = p[0] + d[0] * t, y = p[1] + d[1] * t;
    if (x < 0 || y < 0 || x >= W || y >= H) return 1;
    if (solid(x, y, p[2] + d[2] * t)) return 0;
  }
  return 1;
}

/* Geometric factor cos θ / d² of surface s towards point q. If q lies on an area light (nq is not
 * NULL), it is multiplied by the cosine of emission at q as well. */
static float point_factor(const LightSurface *s, const float q[3], const float *nq, float step) {
  float v[3] = { q[0] - s->p[0], q[1] - s->p[1], q[2] - s->p[2] };
  float d2 = dot3(v, v);
  if (!(d2 > 0)) return 0;
  float d = sqrtf(d2), cos_s = dot3(s->n, v) / d, cos_q = nq ? -dot3(nq, v) / d : 1;
  return cos_s > 0 && cos_q > 0 && unobstructed(s->p, q, step) ? cos_s * cos_q / d2 : 0;
}

/* Offset of a stratified sample on an area light. It depends only on the pixel and the sample index,
 * so the noise is constant from frame to frame and identical on every platform. */
static float jitter(int i, int k) {
  unsigned h = (unsigned)i * 2654435761u + (unsigned)k * 40503u;
  h = (h ^ (h >> 15)) * 2246822519u;
  return (float)((h ^ (h >> 13)) >> 8) / 16777216.0f;
}

static float geometry(const Light *L, const LightSurface *s, int i) {
  const Shape *sh = &L->shape;
  if (sh->kind == POINT) return point_factor(s, sh->pos, NULL, HARD_STEP);
  if (sh->kind == SUN) {                        /* within W + H pixels the ray either leaves the grid or moves
                                                   more than (W + H) / 2 vertically, which is assumed to take
                                                   it above every occluder or into the floor */
    float cos_t = dot3(s->n, sh->pos), far[3];
    for (int c = 0; c < 3; c++) far[c] = s->p[c] + sh->pos[c] * (W + H);
    return cos_t > 0 && unobstructed(s->p, far, HARD_STEP) ? cos_t : 0;
  }
  float sum = 0;                                /* area light: one sample per stratum, weighted by area */
  for (int a = 0; a < AREA_LONG; a++)
    for (int b = 0; b < AREA_SHORT; b++) {
      int k = 2 * (a * AREA_SHORT + b);
      float fa = (a + jitter(i, k)) / AREA_LONG * 2 - 1, fb = (b + jitter(i, k + 1)) / AREA_SHORT * 2 - 1, q[3];
      for (int c = 0; c < 3; c++) q[c] = sh->pos[c] + L->axis_long[c] * fa + L->axis_short[c] * fb;
      sum += point_factor(s, q, L->normal, SOFT_STEP);
    }
  return sum * L->area / (AREA_LONG * AREA_SHORT);
}

static int active(const Light *L) { return L->shape.kind != OFF && L->has_spectrum; }

static void update_geometry(void) {
  for (int l = 0; l < LIGHT_MAX; l++) {
    if (!active(&lights[l]) || lights[l].cached) continue;
    for (int i = 0; i < N; i++) geometry_cache[l][i] = geometry(&lights[l], &surfaces[i], i);
    lights[l].cached = 1;
  }
}

static int has_surface(const LightSurface *s) { return s->row >= 0 && s->row < LIGHT_ROWS; }

/* Ambient illuminance of each light (see light.h). Ē and ρ̄ are averaged over the pixels that show a
 * surface. The idea follows the ambient term of [4]. */
static void ambient_terms(float amb[LIGHT_MAX]) {
  for (int l = 0; l < LIGHT_MAX; l++) {
    const Light *L = &lights[l];
    amb[l] = 0;
    if (!active(L) || ambient_k == 0) continue;
    double g = 0, rho = 0;
    int count = 0;
    for (int i = 0; i < N; i++) {
      const LightSurface *s = &surfaces[i];
      if (!has_surface(s)) continue;
      g += geometry_cache[l][i];
      rho += s->tex * L->T[s->row][1];          /* the spectrum is normalized, so Y is the luminance reflectance */
      count++;
    }
    if (count == 0) continue;
    float mean_rho = fminf((float)(rho / count), RHO_MAX);
    amb[l] = ambient_k * L->strength * (float)(g / count) * mean_rho / (1 - mean_rho);
  }
}

/* ---------- Output: XYZ → linear sRGB → exposure → tone mapping → sRGB encoding ---------- */
/* XYZ to linear sRGB, D65 white point [2]. */
static const float XYZ_TO_RGB[3][3] = {
  { 3.2406f, -1.5372f, -0.4986f },
  { -0.9689f, 1.8758f, 0.0415f },
  { 0.0557f, -0.2040f, 1.0570f },
};

/* Tone mapping: Narkowicz's rational fit [3] to the ACES reference output curve, applied to each
 * linear sRGB channel. The fit is a luminance-only fit and slightly oversaturates highlights. */
static float tonemap(float x) { return x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f); }

/* Linear value → 8-bit sRGB code by binary search in SRGB_LUT. Only comparisons are used, so the result
 * does not depend on the platform's math library. NaN maps to 0. */
static unsigned char quantize(float v) {
  int lo = 0, hi = 255;
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    if (v >= SRGB_LUT[mid - 1]) lo = mid; else hi = mid - 1;
  }
  return (unsigned char)lo;
}

/* Matrix, exposure, tone mapping and encoding; writes 3 bytes. Negative components (outside the sRGB
 * gamut) are clipped to 0. */
static void encode(const float xyz[3], float k, unsigned char out[3]) {
  for (int c = 0; c < 3; c++) out[c] = quantize(tonemap(fmaxf(k * dot3(XYZ_TO_RGB[c], xyz), 0)));
}

LIGHT_API const unsigned char *light_render(void) {
  float amb[LIGHT_MAX];
  update_geometry();
  ambient_terms(amb);
  for (int i = 0; i < N; i++) {
    const LightSurface *s = &surfaces[i];
    unsigned char *out = frame + 4 * i;
    if (!has_surface(s)) { memset(out, 0, 4); continue; }
    float xyz[3] = { 0, 0, 0 };
    for (int l = 0; l < LIGHT_MAX; l++) {
      const Light *L = &lights[l];
      if (!active(L)) continue;
      float e = s->tex * (L->strength * geometry_cache[l][i] + amb[l]);
      for (int c = 0; c < 3; c++) xyz[c] += e * L->T[s->row][c];
    }
    encode(xyz, exposure, out);
    out[3] = 255;
  }
  return frame;
}

LIGHT_API const float *light_probe(int sx, int sy) {
  memset(probe_buf, 0, sizeof probe_buf);
  if (sx < 0 || sy < 0 || sx >= W || sy >= H) return probe_buf;
  int i = sy * W + sx;
  const LightSurface *s = &surfaces[i];
  if (!has_surface(s)) return probe_buf;
  float amb[LIGHT_MAX];
  update_geometry();
  ambient_terms(amb);
  for (int l = 0; l < LIGHT_MAX; l++) {
    const Light *L = &lights[l];
    if (!active(L)) continue;
    float e = L->strength * geometry_cache[l][i] + amb[l];
    for (int k = 0; k < LIGHT_NW; k++) probe_buf[k] += e * L->spd[k];
  }
  for (int k = 0; k < LIGHT_NW; k++) probe_buf[LIGHT_NW + k] = s->tex * reflectance[s->row][k] * probe_buf[k];
  return probe_buf;
}

LIGHT_API int light_row_at(int sx, int sy) {
  return sx < 0 || sy < 0 || sx >= W || sy >= H ? -1 : surfaces[sy * W + sx].row;
}

/* ---------- Colour utilities ---------- */
LIGHT_API const unsigned char *light_source_rgb(const float *spd) {
  float xyz[3] = { 0, 0, 0 }, rgb[3], m = 0;
  for (int k = 0; k < LIGHT_NW; k++)
    for (int c = 0; c < 3; c++) xyz[c] += spd[k] * CMF[c][k];
  for (int c = 0; c < 3; c++) m = fmaxf(m, rgb[c] = fmaxf(dot3(XYZ_TO_RGB[c], xyz), 0));
  for (int c = 0; c < 3; c++) rgb_buf[c] = m > 0 ? quantize(rgb[c] / m) : 0;
  return rgb_buf;
}

LIGHT_API const unsigned char *light_surface_rgb(const float *spd, const float *refl, float e) {
  float y = luminance(spd), xyz[3];
  integrate(spd, refl, xyz);
  for (int c = 0; c < 3; c++) xyz[c] = y > 0 ? e * xyz[c] / y : 0;
  encode(xyz, 1, rgb_buf);
  return rgb_buf;
}

LIGHT_API LightSurface *light_surfaces(void) { return surfaces; }
LIGHT_API float *light_heights(void) { return heights; }
LIGHT_API float *light_gaps(void) { return gaps; }
LIGHT_API float *light_scratch(void) { return scratch; }
LIGHT_API int light_width(void) { return LIGHT_W; }
LIGHT_API int light_height(void) { return LIGHT_H; }
LIGHT_API int light_spectrum_size(void) { return LIGHT_NW; }
LIGHT_API int light_max(void) { return LIGHT_MAX; }

/*
 * spl.c - spectral-pixels engine; see include/spl.h for the interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * The colour of a pixel is the sum of the contributions of all lights:
 *   XYZ = tex · Σ_l E_l · T_l[row],   T_l[row] = Σ_λ Ŝ_l(λ) · R_row(λ) · (x̄, ȳ, z̄)(λ)
 * where Ŝ_l is the normalized spectrum of light l, R_row a row of the reflectance table and E_l the
 * illuminance. E_l depends only on geometry, not on wavelength, so it can be taken out of the
 * spectral sum: each pixel costs three multiply-adds per light, and the spectral sums are
 * recomputed only when a spectrum or a reflectance changes. Integrals are approximated by sums over
 * the 5 nm samples; the step Δλ cancels in the normalization.
 *
 * Determinism: the engine uses only arithmetic, sqrtf, floorf, fminf, fmaxf and conversions, all of
 * which IEEE 754 defines exactly, and encodes sRGB with a precomputed threshold table instead of
 * powf. Compiled without floating-point contraction (-ffp-contract=off), it gives the same bytes on
 * every platform.
 *
 * References
 *   [1] CIE 015:2018 Colorimetry, 4th ed. doi:10.25039/TR.015.2018
 *   [2] IEC 61966-2-1:1999 Multimedia systems and equipment - Colour measurement and management -
 *       Part 2-1: Colour management - Default RGB colour space - sRGB
 *   [3] K. Narkowicz, "ACES Filmic Tone Mapping Curve", 2016-01-06.
 *       https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
 *   [4] M. F. Cohen, S. E. Chen, J. R. Wallace, D. P. Greenberg, "A Progressive Refinement
 *       Approach to Fast Radiosity Image Generation", Computer Graphics (SIGGRAPH '88)
 *       22(4):75-84, 1988. doi:10.1145/378456.378487
 */
#ifndef SPL_BUILD
#define SPL_BUILD /* exporting, not importing, when SPL_SHARED is defined */
#endif
#include "spl.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cmf.h"      /* CMF[3][SPL_NW]: x̄, ȳ, z̄ */
#include "srgb_lut.h" /* SRGB_LUT[255]: 8-bit thresholds of the sRGB encoding */

/* Strata of an area light along its long and its short edge. */
enum { AREA_LONG = 8, AREA_SHORT = 4 };

/* Horizontal step of occlusion tests, in pixels. Hard shadows of point and directional lights need
 * half a pixel; the penumbra of an area light averages 32 samples, so the coarser step is not
 * visible. */
#define HARD_STEP 0.5f
#define SOFT_STEP 1.0f

/* Cap on the mean reflectance, which keeps 1 / (1 − ρ̄) finite. */
#define RHO_MAX 0.95f

enum shape_kind { OFF, POINT, DIRECTIONAL, AREA };

/* Everything the geometric factor depends on. Compared with memcmp, so it must have no padding and
 * unused members must be zero. */
typedef struct {
    int kind;
    float pos[3]; /* point: position; directional: unit vector towards the light; area: center */
    float u[3];   /* area: half-edges */
    float v[3];
} shape;
_Static_assert(sizeof(shape) == sizeof(int) + 9 * sizeof(float), "shape must have no padding");

typedef struct {
    shape shape;
    float axis_long[3]; /* area: sampling axes (the longer and the shorter half-edge) */
    float axis_short[3];
    float normal[3];   /* area: unit normal of the emitting patch */
    float area;        /* area: area of the patch */
    float strength;    /* intensity, illuminance or luminance, depending on the kind */
    float spd[SPL_NW]; /* normalized spectrum: Σ spd · ȳ = 1 */
    float *xyz;        /* rows × 3: XYZ of each reflectance row under unit illuminance */
    int has_spectrum;
    int cached; /* geometry[light] is valid for shape */
} light;

struct spl_engine {
    spl_config config;
    int pixels;            /* width × height */
    spl_surface *surfaces; /* pixels */
    float *heights;        /* pixels */
    float *gaps;           /* 2 × pixels */
    float ceiling;
    float *reflectance; /* rows × SPL_NW */
    light *lights;      /* config.lights */
    float *geometry;    /* lights × pixels; illuminance = strength × geometric factor */
    float *ambient;     /* lights; ambient illuminance of each light during a render */
    float ambient_k;
    float exposure;
};

/* ---------- Small helpers ---------- */

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float norm3(const float a[3])
{
    return sqrtf(dot3(a, a));
}

static void cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int finite3(const float a[3])
{
    return isfinite(a[0]) && isfinite(a[1]) && isfinite(a[2]);
}

/* All values finite and non-negative. */
static int valid_spectrum(const float s[SPL_NW])
{
    for (int k = 0; k < SPL_NW; k++) {
        if (!isfinite(s[k]) || s[k] < 0) {
            return 0;
        }
    }
    return 1;
}

static int valid_light(const spl_engine *e, int l)
{
    return e && l >= 0 && l < e->config.lights;
}

/* ---------- Engine ---------- */

#define SPL_STR2(x) #x
#define SPL_STR(x)  SPL_STR2(x)

const char *spl_version(void)
{
    return SPL_STR(SPL_VERSION_MAJOR) "." SPL_STR(SPL_VERSION_MINOR) "." SPL_STR(SPL_VERSION_PATCH);
}

/* Zeroed allocation of a × b × size bytes; NULL if the product overflows or memory runs out. */
static void *alloc(size_t a, size_t b, size_t size)
{
    if (b != 0 && a > (size_t)-1 / b) {
        return NULL;
    }
    if (size != 0 && a * b > (size_t)-1 / size) {
        return NULL;
    }
    return calloc(a * b, size);
}

static int valid_config(const spl_config *c)
{
    if (c->width < 1 || c->height < 1 || c->rows < 1 || c->lights < 1) {
        return 0;
    }
    if (c->width > SPL_MAX_SIZE || c->height > SPL_MAX_SIZE || c->rows > SPL_MAX_ROWS ||
        c->lights > SPL_MAX_LIGHTS) {
        return 0;
    }
    return (size_t)c->width * (size_t)c->height <= (size_t)INT_MAX;
}

spl_status spl_create(const spl_config *config, spl_engine **engine)
{
    if (!engine) {
        return SPL_ERR_ARGUMENT;
    }
    *engine = NULL;
    if (!config || !valid_config(config)) {
        return SPL_ERR_ARGUMENT;
    }
    size_t pixels = (size_t)config->width * (size_t)config->height;
    spl_engine *e = alloc(1, 1, sizeof *e);
    if (!e) {
        return SPL_ERR_MEMORY;
    }
    e->config = *config;
    e->pixels = (int)pixels;
    size_t rows = (size_t)config->rows;
    size_t lights = (size_t)config->lights;
    e->surfaces = alloc(pixels, 1, sizeof *e->surfaces);
    e->heights = alloc(pixels, 1, sizeof *e->heights);
    e->gaps = alloc(pixels, 2, sizeof *e->gaps);
    e->reflectance = alloc(rows, SPL_NW, sizeof *e->reflectance);
    e->lights = alloc(lights, 1, sizeof *e->lights);
    e->geometry = alloc(lights, pixels, sizeof *e->geometry);
    e->ambient = alloc(lights, 1, sizeof *e->ambient);
    int ok = e->surfaces && e->heights && e->gaps && e->reflectance && e->lights && e->geometry &&
             e->ambient;
    for (int l = 0; ok && l < config->lights; l++) {
        e->lights[l].xyz = alloc(rows, 3, sizeof(float));
        ok = e->lights[l].xyz != NULL;
    }
    if (!ok) {
        spl_destroy(e);
        return SPL_ERR_MEMORY;
    }
    for (size_t i = 0; i < pixels; i++) {
        e->surfaces[i].n[2] = 1;
        e->surfaces[i].row = -1;
        e->surfaces[i].tex = 1;
    }
    e->ceiling = INFINITY;
    e->ambient_k = 1;
    e->exposure = 1;
    *engine = e;
    return SPL_OK;
}

void spl_destroy(spl_engine *e)
{
    if (!e) {
        return;
    }
    if (e->lights) {
        for (int l = 0; l < e->config.lights; l++) {
            free(e->lights[l].xyz);
        }
    }
    free(e->surfaces);
    free(e->heights);
    free(e->gaps);
    free(e->reflectance);
    free(e->lights);
    free(e->geometry);
    free(e->ambient);
    free(e);
}

spl_config spl_get_config(const spl_engine *e)
{
    spl_config none = { 0, 0, 0, 0 };
    return e ? e->config : none;
}

spl_surface *spl_surfaces(spl_engine *e)
{
    return e ? e->surfaces : NULL;
}

float *spl_heights(spl_engine *e)
{
    return e ? e->heights : NULL;
}

float *spl_gaps(spl_engine *e)
{
    return e ? e->gaps : NULL;
}

void spl_invalidate(spl_engine *e)
{
    if (!e) {
        return;
    }
    for (int l = 0; l < e->config.lights; l++) {
        e->lights[l].cached = 0;
    }
}

spl_status spl_set_ceiling(spl_engine *e, float z)
{
    if (!e || isnan(z)) {
        return SPL_ERR_ARGUMENT;
    }
    if (z != e->ceiling) {
        e->ceiling = z;
        spl_invalidate(e);
    }
    return SPL_OK;
}

/* ---------- Spectra ---------- */

static void integrate(const float *spd, const float *refl, float xyz[3])
{
    for (int c = 0; c < 3; c++) {
        float sum = 0;
        for (int k = 0; k < SPL_NW; k++) {
            sum += spd[k] * refl[k] * CMF[c][k];
        }
        xyz[c] = sum;
    }
}

static float spectrum_luminance(const float *spd)
{
    float y = 0;
    for (int k = 0; k < SPL_NW; k++) {
        y += spd[k] * CMF[1][k];
    }
    return y;
}

spl_status spl_light_set_spectrum(spl_engine *e, int l, const float spd[SPL_NW])
{
    if (!valid_light(e, l) || !spd || !valid_spectrum(spd)) {
        return SPL_ERR_ARGUMENT;
    }
    float y = spectrum_luminance(spd);
    if (!(y > 0) || !isfinite(y)) {
        return SPL_ERR_ARGUMENT;
    }
    light *L = &e->lights[l];
    for (int k = 0; k < SPL_NW; k++) {
        L->spd[k] = spd[k] / y;
    }
    L->has_spectrum = 1;
    for (int r = 0; r < e->config.rows; r++) {
        integrate(L->spd, &e->reflectance[r * SPL_NW], &L->xyz[3 * r]);
    }
    return SPL_OK;
}

spl_status spl_set_reflectance(spl_engine *e, int row, const float refl[SPL_NW])
{
    if (!e || row < 0 || row >= e->config.rows || !refl || !valid_spectrum(refl)) {
        return SPL_ERR_ARGUMENT;
    }
    float *dst = &e->reflectance[row * SPL_NW];
    memcpy(dst, refl, SPL_NW * sizeof *dst);
    for (int l = 0; l < e->config.lights; l++) {
        integrate(e->lights[l].spd, dst, &e->lights[l].xyz[3 * row]);
    }
    return SPL_OK;
}

/* ---------- Lights ---------- */

/* Stores the shape of light l; its cached geometry stays valid only if the shape is unchanged. */
static void set_shape(spl_engine *e, int l, const shape *s, float strength)
{
    light *L = &e->lights[l];
    if (memcmp(&L->shape, s, sizeof *s) != 0) {
        L->shape = *s;
        L->cached = 0;
    }
    L->strength = strength;
}

static void new_shape(shape *s, int kind, float x, float y, float z)
{
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->pos[0] = x;
    s->pos[1] = y;
    s->pos[2] = z;
}

spl_status spl_light_set_point(spl_engine *e, int l, float x, float y, float z, float intensity)
{
    if (!valid_light(e, l) || !isfinite(x) || !isfinite(y) || !isfinite(z) ||
        !isfinite(intensity) || intensity < 0) {
        return SPL_ERR_ARGUMENT;
    }
    shape s;
    new_shape(&s, POINT, x, y, z);
    set_shape(e, l, &s, intensity);
    return SPL_OK;
}

spl_status spl_light_set_directional(spl_engine *e, int l, float dx, float dy, float dz,
                                     float illuminance)
{
    float d[3] = { dx, dy, dz };
    if (!valid_light(e, l) || !finite3(d) || !isfinite(illuminance) || illuminance < 0) {
        return SPL_ERR_ARGUMENT;
    }
    float len = norm3(d);
    if (!(len > 0) || !isfinite(len)) {
        return SPL_ERR_ARGUMENT;
    }
    shape s;
    new_shape(&s, DIRECTIONAL, dx / len, dy / len, dz / len);
    set_shape(e, l, &s, illuminance);
    return SPL_OK;
}

spl_status spl_light_set_area(spl_engine *e, int l, const float center[3], const float u[3],
                              const float v[3], float luminance)
{
    if (!valid_light(e, l) || !center || !u || !v || !finite3(center) || !finite3(u) ||
        !finite3(v) || !isfinite(luminance) || luminance < 0) {
        return SPL_ERR_ARGUMENT;
    }
    float n[3];
    cross3(u, v, n);
    float len = norm3(n);
    if (!(len > 0) || !isfinite(len)) {
        return SPL_ERR_ARGUMENT;
    }
    light *L = &e->lights[l];
    shape s;
    new_shape(&s, AREA, center[0], center[1], center[2]);
    memcpy(s.u, u, sizeof s.u);
    memcpy(s.v, v, sizeof s.v);
    for (int c = 0; c < 3; c++) {
        L->normal[c] = n[c] / len;
    }
    L->area = 4 * len; /* area of the parallelogram, |2u × 2v| */
    int u_long = norm3(u) >= norm3(v);
    memcpy(L->axis_long, u_long ? u : v, sizeof L->axis_long);
    memcpy(L->axis_short, u_long ? v : u, sizeof L->axis_short);
    set_shape(e, l, &s, luminance);
    return SPL_OK;
}

spl_status spl_light_disable(spl_engine *e, int l)
{
    if (!valid_light(e, l)) {
        return SPL_ERR_ARGUMENT;
    }
    e->lights[l].shape.kind = OFF;
    return SPL_OK;
}

spl_status spl_set_ambient(spl_engine *e, float k)
{
    if (!e || !isfinite(k) || k < 0) {
        return SPL_ERR_ARGUMENT;
    }
    e->ambient_k = k;
    return SPL_OK;
}

spl_status spl_set_exposure(spl_engine *e, float k)
{
    if (!e || !isfinite(k) || k < 0) {
        return SPL_ERR_ARGUMENT;
    }
    e->exposure = k;
    return SPL_OK;
}

/* ---------- Occlusion ---------- */

/* Whether the point (x, y, z), inside the grid, is solid: above the ceiling, or below the height
 * field and not inside a gap. */
static int solid(const spl_engine *e, float x, float y, float z)
{
    int i = (int)y * e->config.width + (int)x;
    if (z > e->ceiling) {
        return 1;
    }
    int in_gap = z > e->gaps[2 * i] && z < e->gaps[2 * i + 1];
    return z < e->heights[i] && !in_gap;
}

/* Whether the segment p → q is unobstructed. The occluders form a height field, so the segment is
 * sampled at intervals of at most `step` pixels of horizontal length; at least one interior sample
 * is taken so that nearly vertical segments are still stopped by the ceiling. Once the segment
 * leaves the grid (a convex rectangle) it cannot re-enter it, and nothing outside the grid blocks
 * light, so the segment is then visible. The start point is not tested; the caller keeps it outside
 * solid space. The sample count is capped at 10⁹ so that it fits in an int; segments longer than
 * 10⁹ steps are sampled more coarsely. A sample that is not a number counts as outside the grid. */
static int unobstructed(const spl_engine *e, const float p[3], const float q[3], float step)
{
    float d[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
    int n = (int)fminf(sqrtf(d[0] * d[0] + d[1] * d[1]) / step, 1e9f) + 2;
    float w = (float)e->config.width;
    float h = (float)e->config.height;
    for (int k = 1; k < n; k++) {
        float t = (float)k / n;
        float x = p[0] + d[0] * t;
        float y = p[1] + d[1] * t;
        if (!(x >= 0 && y >= 0 && x < w && y < h)) {
            return 1;
        }
        if (solid(e, x, y, p[2] + d[2] * t)) {
            return 0;
        }
    }
    return 1;
}

/* Geometric factor cos θ / d² of surface s towards point q. If q lies on an area light (nq is not
 * NULL), it is multiplied by the cosine of emission at q as well. */
static float point_factor(const spl_engine *e, const spl_surface *s, const float q[3],
                          const float *nq, float step)
{
    float v[3] = { q[0] - s->p[0], q[1] - s->p[1], q[2] - s->p[2] };
    float d2 = dot3(v, v);
    if (!(d2 > 0)) {
        return 0;
    }
    float d = sqrtf(d2);
    float cos_s = dot3(s->n, v) / d;
    float cos_q = 1;
    if (nq) {
        cos_q = -dot3(nq, v) / d;
    }
    if (!(cos_s > 0 && cos_q > 0 && unobstructed(e, s->p, q, step))) {
        return 0;
    }
    return cos_s * cos_q / d2;
}

/* Offset of a stratified sample on an area light. It depends only on the pixel and the sample
 * index, so the noise is fixed and identical on every platform. */
static float jitter(int i, int k)
{
    unsigned h = (unsigned)i * 2654435761u + (unsigned)k * 40503u;
    h = (h ^ (h >> 15)) * 2246822519u;
    return (float)((h ^ (h >> 13)) >> 8) / 16777216.0f;
}

static float geometry(const spl_engine *e, const light *L, const spl_surface *s, int i)
{
    const shape *sh = &L->shape;
    if (sh->kind == POINT) {
        return point_factor(e, s, sh->pos, NULL, HARD_STEP);
    }
    if (sh->kind == DIRECTIONAL) {
        /* Within width + height pixels the ray either leaves the grid or moves more than
         * (width + height) / 2 vertically, which is assumed to take it above every occluder or into
         * the floor. */
        float reach = (float)(e->config.width + e->config.height);
        float cos_t = dot3(s->n, sh->pos);
        float far[3];
        for (int c = 0; c < 3; c++) {
            far[c] = s->p[c] + sh->pos[c] * reach;
        }
        if (!(cos_t > 0 && unobstructed(e, s->p, far, HARD_STEP))) {
            return 0;
        }
        return cos_t;
    }
    /* Area light: one sample per stratum, weighted by area. */
    float sum = 0;
    for (int a = 0; a < AREA_LONG; a++) {
        for (int b = 0; b < AREA_SHORT; b++) {
            int k = 2 * (a * AREA_SHORT + b);
            float fa = (a + jitter(i, k)) / AREA_LONG * 2 - 1;
            float fb = (b + jitter(i, k + 1)) / AREA_SHORT * 2 - 1;
            float q[3];
            for (int c = 0; c < 3; c++) {
                q[c] = sh->pos[c] + L->axis_long[c] * fa + L->axis_short[c] * fb;
            }
            sum += point_factor(e, s, q, L->normal, SOFT_STEP);
        }
    }
    return sum * L->area / (AREA_LONG * AREA_SHORT);
}

static int active(const light *L)
{
    return L->shape.kind != OFF && L->has_spectrum;
}

static int has_surface(const spl_engine *e, const spl_surface *s)
{
    return s->row >= 0 && s->row < e->config.rows;
}

static void update_geometry(spl_engine *e)
{
    for (int l = 0; l < e->config.lights; l++) {
        light *L = &e->lights[l];
        if (!active(L) || L->cached) {
            continue;
        }
        float *g = &e->geometry[(size_t)l * (size_t)e->pixels];
        for (int i = 0; i < e->pixels; i++) {
            g[i] = geometry(e, L, &e->surfaces[i], i);
        }
        L->cached = 1;
    }
}

/* Ambient illuminance of each light (see spl.h). Ē and ρ̄ are averaged over the pixels that show a
 * surface. The idea follows the ambient term of [4]. */
static void ambient_terms(spl_engine *e)
{
    for (int l = 0; l < e->config.lights; l++) {
        const light *L = &e->lights[l];
        const float *g = &e->geometry[(size_t)l * (size_t)e->pixels];
        e->ambient[l] = 0;
        if (!active(L) || e->ambient_k == 0) {
            continue;
        }
        double sum_g = 0;
        double sum_rho = 0;
        int count = 0;
        for (int i = 0; i < e->pixels; i++) {
            const spl_surface *s = &e->surfaces[i];
            if (!has_surface(e, s)) {
                continue;
            }
            sum_g += (double)g[i];
            /* The spectrum is normalized, so Y is the luminance reflectance. */
            sum_rho += (double)(s->tex * L->xyz[3 * s->row + 1]);
            count++;
        }
        if (count == 0) {
            continue;
        }
        float mean_rho = fminf((float)(sum_rho / count), RHO_MAX);
        e->ambient[l] =
            e->ambient_k * L->strength * (float)(sum_g / count) * mean_rho / (1 - mean_rho);
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
 * linear sRGB channel. The fit is a luminance-only fit and slightly oversaturates highlights.
 * Inputs are capped where x · x would overflow; the curve is flat there. */
static float tonemap(float x)
{
    x = fminf(x, 1e18f);
    return x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f);
}

/* Linear value → 8-bit sRGB code by binary search in SRGB_LUT. Only comparisons are used, so the
 * result does not depend on the platform's math library. NaN maps to 0. */
static uint8_t quantize(float v)
{
    int lo = 0;
    int hi = 255;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (v >= SRGB_LUT[mid - 1]) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return (uint8_t)lo;
}

/* Matrix, exposure, tone mapping and encoding; writes 3 bytes. Negative components (outside the
 * sRGB gamut) are clipped to 0. */
static void encode(const float xyz[3], float k, uint8_t out[3])
{
    for (int c = 0; c < 3; c++) {
        out[c] = quantize(tonemap(fmaxf(k * dot3(XYZ_TO_RGB[c], xyz), 0)));
    }
}

spl_status spl_render(spl_engine *e, uint8_t *rgba)
{
    if (!e || !rgba) {
        return SPL_ERR_ARGUMENT;
    }
    update_geometry(e);
    ambient_terms(e);
    for (int i = 0; i < e->pixels; i++) {
        const spl_surface *s = &e->surfaces[i];
        uint8_t *out = rgba + 4 * (size_t)i;
        if (!has_surface(e, s)) {
            memset(out, 0, 4);
            continue;
        }
        float xyz[3] = { 0, 0, 0 };
        for (int l = 0; l < e->config.lights; l++) {
            const light *L = &e->lights[l];
            if (!active(L)) {
                continue;
            }
            float g = e->geometry[(size_t)l * (size_t)e->pixels + (size_t)i];
            float illuminance = s->tex * (L->strength * g + e->ambient[l]);
            for (int c = 0; c < 3; c++) {
                xyz[c] += illuminance * L->xyz[3 * s->row + c];
            }
        }
        encode(xyz, e->exposure, out);
        out[3] = 255;
    }
    return SPL_OK;
}

spl_status spl_probe(spl_engine *e, int x, int y, float incident[SPL_NW], float reflected[SPL_NW])
{
    if (!e || x < 0 || y < 0 || x >= e->config.width || y >= e->config.height) {
        return SPL_ERR_ARGUMENT;
    }
    float in[SPL_NW] = { 0 };
    float out[SPL_NW] = { 0 };
    int i = y * e->config.width + x;
    const spl_surface *s = &e->surfaces[i];
    if (has_surface(e, s)) {
        update_geometry(e);
        ambient_terms(e);
        for (int l = 0; l < e->config.lights; l++) {
            const light *L = &e->lights[l];
            if (!active(L)) {
                continue;
            }
            float g = e->geometry[(size_t)l * (size_t)e->pixels + (size_t)i];
            float illuminance = L->strength * g + e->ambient[l];
            for (int k = 0; k < SPL_NW; k++) {
                in[k] += illuminance * L->spd[k];
            }
        }
        const float *refl = &e->reflectance[s->row * SPL_NW];
        for (int k = 0; k < SPL_NW; k++) {
            out[k] = s->tex * refl[k] * in[k];
        }
    }
    if (incident) {
        memcpy(incident, in, sizeof in);
    }
    if (reflected) {
        memcpy(reflected, out, sizeof out);
    }
    return SPL_OK;
}

/* ---------- Colour utilities ---------- */

/* A light spectrum as spl_light_set_spectrum() accepts it: valid, with a positive finite
 * luminance. */
static int valid_light_spectrum(const float spd[SPL_NW])
{
    if (!valid_spectrum(spd)) {
        return 0;
    }
    float y = spectrum_luminance(spd);
    return y > 0 && isfinite(y);
}

spl_status spl_light_rgb(const float spd[SPL_NW], uint8_t rgb[3])
{
    if (!spd || !rgb || !valid_light_spectrum(spd)) {
        return SPL_ERR_ARGUMENT;
    }
    float xyz[3] = { 0, 0, 0 };
    for (int k = 0; k < SPL_NW; k++) {
        for (int c = 0; c < 3; c++) {
            xyz[c] += spd[k] * CMF[c][k];
        }
    }
    float lin[3];
    float m = 0;
    for (int c = 0; c < 3; c++) {
        lin[c] = fmaxf(dot3(XYZ_TO_RGB[c], xyz), 0);
        m = fmaxf(m, lin[c]);
    }
    for (int c = 0; c < 3; c++) {
        rgb[c] = m > 0 ? quantize(lin[c] / m) : 0;
    }
    return SPL_OK;
}

spl_status spl_surface_rgb(const float spd[SPL_NW], const float refl[SPL_NW], float e,
                           uint8_t rgb[3])
{
    if (!spd || !refl || !rgb || !valid_light_spectrum(spd) || !valid_spectrum(refl) ||
        !isfinite(e) || e < 0) {
        return SPL_ERR_ARGUMENT;
    }
    float y = spectrum_luminance(spd);
    float xyz[3];
    integrate(spd, refl, xyz);
    for (int c = 0; c < 3; c++) {
        xyz[c] = y > 0 ? e * xyz[c] / y : 0;
    }
    encode(xyz, 1, rgb);
    return SPL_OK;
}

/*
 * spl.h - spectral-pixels: spectral lighting for 2D pixel scenes
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * The engine lights a pixel image wavelength by wavelength. It works only with spectra: the
 * relative spectral power distribution of each light and a table of spectral reflectances indexed
 * by row. It knows nothing about material names or the structure of the scene; the caller
 * describes, through the buffers below, which surface each pixel shows and which parts of space
 * block light.
 *
 * Usage
 *   1. Create an engine with spl_create().
 *   2. Fill spl_surfaces() (one visible surface point per pixel) and the occluders spl_heights(),
 *      spl_gaps() and spl_set_ceiling().
 *   3. Set the reflectance table with spl_set_reflectance() and the lights with
 *      spl_light_set_spectrum() and one of spl_light_set_point(), spl_light_set_directional() or
 *      spl_light_set_area().
 *   4. Call spl_render() for an RGBA image, or spl_probe() for the spectra at one pixel.
 *   5. Free the engine with spl_destroy().
 *
 * Conventions
 *   Lengths are in pixels. Coordinates: x to the right, y down the image, z up. Positions must be
 *   finite and normals unit vectors. Spectra have SPL_NW samples, from SPL_LAMBDA_MIN nm in steps
 *   of SPL_LAMBDA_STEP nm.
 *
 *   Light spectra are normalized inside the engine so that sum S(λ)·ȳ(λ) = 1. Changing a spectrum
 *   therefore changes the colour of a light but not its luminance, and all strength parameters are
 *   relative photometric quantities. Surfaces are Lambertian; the factor 1/π of the reflected
 *   radiance, like every other constant scale, is absorbed into the exposure.
 *
 *   Output: XYZ is converted to linear sRGB, scaled by the exposure, tone mapped and encoded with
 *   the sRGB transfer function. No white balancing is applied; lights appear in their own colour.
 *
 * Errors
 *   Functions that can fail return an spl_status. An invalid argument (a NULL pointer, an index
 *   out of range, a value that is not finite or out of its documented range) leaves the engine
 *   unchanged and returns SPL_ERR_ARGUMENT.
 *
 * Threads
 *   Engines are independent of each other and the library has no global state. One engine must
 *   not be used from two threads at the same time.
 *
 * Geometry cache
 *   The geometric factor of each light at each pixel (distance, angle of incidence, visibility) is
 *   cached. It is recomputed automatically when the kind, position or shape of a light changes.
 *   After changing the position p or normal n of any surface, or the occluders, call
 *   spl_invalidate(). Changes to strengths, spectra, reflectances, texture shades or rows do not
 *   require it.
 */
#ifndef SPECTRAL_PIXELS_SPL_H
#define SPECTRAL_PIXELS_SPL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPL_VERSION_MAJOR 0
#define SPL_VERSION_MINOR 1
#define SPL_VERSION_PATCH 0

/* Spectral sampling: 400, 405, ..., 700 nm. */
#define SPL_NW          61
#define SPL_LAMBDA_MIN  400
#define SPL_LAMBDA_STEP 5

/* Limits of spl_config. */
#define SPL_MAX_SIZE   65536   /* width and height; width × height must also fit in an int */
#define SPL_MAX_ROWS   1048576 /* rows of the reflectance table */
#define SPL_MAX_LIGHTS 1024    /* light slots */

/* Define SPL_SHARED when building or using spectral-pixels as a shared library (DLL), so that its
 * functions are exported; a static build exports nothing beyond what the compiler does anyway. */
#ifndef SPL_API
#if defined(SPL_SHARED) && defined(_WIN32)
#ifdef SPL_BUILD
#define SPL_API __declspec(dllexport)
#else
#define SPL_API __declspec(dllimport)
#endif
#elif defined(SPL_SHARED) && (defined(__GNUC__) || defined(__clang__))
#define SPL_API __attribute__((visibility("default")))
#else
#define SPL_API
#endif
#endif

typedef enum {
    SPL_OK = 0,
    SPL_ERR_ARGUMENT = -1, /* NULL pointer, index out of range, value not finite or out of range */
    SPL_ERR_MEMORY = -2    /* allocation failed */
} spl_status;

typedef struct spl_engine spl_engine;

typedef struct {
    int width; /* image width and height in pixels; also the size of the occluder grid */
    int height;
    int rows;   /* rows of the reflectance table */
    int lights; /* number of light slots */
} spl_config;

typedef struct {
    float p[3];  /* world position of the visible surface point */
    float n[3];  /* unit normal */
    int32_t row; /* reflectance row; outside the table (e.g. -1): no surface, transparent pixel */
    float tex;   /* texture shade, multiplies the reflectance */
} spl_surface;

/* ---- Engine ---- */

/* "major.minor.patch" */
SPL_API const char *spl_version(void);

/* Creates an engine and stores it in *engine. Every member of the configuration must be at least 1
 * and at most its SPL_MAX_* limit. Surfaces start with row -1, occluders empty, the ceiling at
 * infinity, all lights off, ambient factor 1 and exposure 1. On failure *engine is set to NULL and
 * SPL_ERR_ARGUMENT (invalid configuration) or SPL_ERR_MEMORY is returned. */
SPL_API spl_status spl_create(const spl_config *config, spl_engine **engine);

/* Frees an engine; NULL is ignored. */
SPL_API void spl_destroy(spl_engine *engine);

/* The configuration the engine was created with. */
SPL_API spl_config spl_get_config(const spl_engine *engine);

/* ---- Scene, filled in by the caller ---- */

/* width × height surfaces, one per pixel, row major. */
SPL_API spl_surface *spl_surfaces(spl_engine *engine);

/* width × height height field: inside the grid, every point with z < h is solid. */
SPL_API float *spl_heights(spl_engine *engine);

/* width × height × 2 gaps (lo, hi) in the solid: lo < z < hi does not block light; (0, 0) for
 * none. */
SPL_API float *spl_gaps(spl_engine *engine);

/* Inside the grid, points above the ceiling block light; outside the grid nothing does. z may be
 * +infinity (no ceiling) but not NaN. */
SPL_API spl_status spl_set_ceiling(spl_engine *engine, float z);

/* Surface positions, normals or occluders have changed. */
SPL_API void spl_invalidate(spl_engine *engine);

/* ---- Lights, index 0 <= light < config.lights ---- */

/* Relative spectral power distribution: non-negative, finite, with a positive luminance. */
SPL_API spl_status spl_light_set_spectrum(spl_engine *engine, int light, const float spd[SPL_NW]);

/* Point light at (x, y, z) with luminous intensity I >= 0: illuminance E = I cos θ / d². Shadows of
 * a light more than 5 · 10⁸ pixels away are tested more coarsely; use a directional light. */
SPL_API spl_status spl_light_set_point(spl_engine *engine, int light, float x, float y, float z,
                                       float intensity);

/* Directional light. (dx, dy, dz) points towards the light and need not be normalized, but its
 * length must be a positive finite float (vectors so short or long that it underflows or overflows
 * are rejected). E⊥ >= 0 is the illuminance on a plane perpendicular to the rays, so
 * E = E⊥ cos θ. */
SPL_API spl_status spl_light_set_directional(spl_engine *engine, int light, float dx, float dy,
                                             float dz, float illuminance);

/* Area light: the parallelogram with the given center and half-edges u and v, emitting as a
 * Lambertian source of luminance L >= 0 towards the side of u × v. E = L ∫ cos θ cos θ' / d² dA,
 * estimated by stratified sampling. u × v must not be zero. */
SPL_API spl_status spl_light_set_area(spl_engine *engine, int light, const float center[3],
                                      const float u[3], const float v[3], float luminance);

/* Switches a light off. Its spectrum is kept. */
SPL_API spl_status spl_light_disable(spl_engine *engine, int light);

/* ---- Ambient light ----
 * Light that reaches surfaces after repeated diffuse reflection, estimated with the
 * integrating-sphere relation from room averages. For each light
 *   E_amb = k · Ē · ρ̄ / (1 − ρ̄)
 * where Ē is the mean direct illuminance from that light over the visible surfaces and ρ̄ their
 * mean luminance reflectance under it. E_amb is the same for every surface, ignores occlusion and
 * has the spectrum of the light. k >= 0; k = 0 disables ambient light. */
SPL_API spl_status spl_set_ambient(spl_engine *engine, float k);

/* ---- Materials and output ---- */

/* Sets one row of the reflectance table: non-negative, finite values, normally 0-1. */
SPL_API spl_status spl_set_reflectance(spl_engine *engine, int row, const float refl[SPL_NW]);

/* Linear exposure factor >= 0 (not EV). */
SPL_API spl_status spl_set_exposure(spl_engine *engine, float k);

/* Renders width × height RGBA pixels, row major, into rgba. Pixels without a surface are
 * (0, 0, 0, 0). */
SPL_API spl_status spl_render(spl_engine *engine, uint8_t *rgba);

/* Incident and reflected spectra at pixel (x, y), in units of illuminance, before exposure; all
 * zeros where there is no surface. Either output may be NULL. */
SPL_API spl_status spl_probe(spl_engine *engine, int x, int y, float incident[SPL_NW],
                             float reflected[SPL_NW]);

/* ---- Colour utilities, independent of any engine ---- */

/* Colour of a light itself: linear sRGB scaled so that the largest channel is 1, then encoded; no
 * tone mapping. The spectrum must be valid for spl_light_set_spectrum(). */
SPL_API spl_status spl_light_rgb(const float spd[SPL_NW], uint8_t rgb[3]);

/* Displayed colour of a surface with reflectance refl under illuminance e >= 0 from a light with
 * spectrum spd (valid for spl_light_set_spectrum()): tone mapped, with an exposure of 1. */
SPL_API spl_status spl_surface_rgb(const float spd[SPL_NW], const float refl[SPL_NW], float e,
                                   uint8_t rgb[3]);

#ifdef __cplusplus
}
#endif

#endif

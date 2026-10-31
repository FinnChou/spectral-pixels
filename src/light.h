/*
 * light.h — interface of the spectral lighting engine
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * The engine works only with spectra: the relative spectral power distribution of each light and a
 * table of spectral reflectances indexed by row. It knows nothing about material names or the
 * structure of the scene; the caller describes, through the buffers below, which surface each pixel
 * shows and which parts of space block light.
 *
 * Conventions
 *   Lengths are in pixels. Coordinates: x to the right, y down the screen, z up. Normals must be unit
 *   vectors.
 *   Spectra have LIGHT_NW samples (400–700 nm in 5 nm steps). Light spectra are normalized inside the
 *   engine so that Σ S(λ)·ȳ(λ) = 1; changing a spectrum therefore changes the colour of a light but
 *   not its luminance, and all strength parameters are relative photometric quantities.
 *   Surfaces are Lambertian. The factor 1/π of the reflected radiance, like every other constant
 *   scale, is absorbed into the exposure.
 *   Output: XYZ is converted to linear sRGB, scaled by the exposure, tone mapped and encoded with the
 *   sRGB transfer function. No white balancing is applied; lights appear in their own colour.
 *   There is a single engine instance and it is not reentrant. All returned pointers refer to internal
 *   static buffers. The buffers of light_surfaces(), light_heights(), light_gaps() and light_scratch()
 *   are permanent. All other results are overwritten by the next call that returns the same buffer:
 *   light_render() and light_probe() each have their own, while light_source_rgb() and
 *   light_surface_rgb() share one.
 *
 * Geometry cache
 *   The geometric factor of each light at each pixel (distance, angle of incidence, visibility) is
 *   cached. It is recomputed automatically when the kind, position or shape of a light changes. After
 *   changing the position p or normal n of any surface, or the occluders (heights, gaps), the caller
 *   must call light_invalidate(). Changes to strengths, spectra, reflectances, texture shades or row
 *   indices do not require it.
 */
#ifndef SPECTRAL_LIGHT_H
#define SPECTRAL_LIGHT_H

#ifndef LIGHT_API
#define LIGHT_API __attribute__((visibility("default")))
#endif

#define LIGHT_NW 61            /* spectral samples: 400, 405, …, 700 nm */
#define LIGHT_W 256            /* image width and height in pixels; also the size of the occluder grid */
#define LIGHT_H 160
#define LIGHT_ROWS 16          /* rows of the reflectance table */
#define LIGHT_MAX 8            /* maximum number of lights */
#define LIGHT_SCRATCH 3        /* spectra that fit into light_scratch() */

typedef struct {
  float p[3];                  /* world position of the visible surface point */
  float n[3];                  /* unit normal */
  int row;                     /* row of the reflectance table; -1: no surface, the pixel is transparent */
  float tex;                   /* texture shade, multiplies the reflectance */
} LightSurface;

/* ---- Scene (filled in by the caller) ---- */
LIGHT_API LightSurface *light_surfaces(void);   /* LIGHT_W × LIGHT_H surfaces, one per pixel */
LIGHT_API float *light_heights(void);           /* LIGHT_W × LIGHT_H height field: 0 ≤ z < h is solid */
LIGHT_API float *light_gaps(void);              /* LIGHT_W × LIGHT_H × 2 gaps (lo, hi) in the solid:
                                                   lo < z < hi does not block light; (0, 0) for none */
LIGHT_API void light_set_ceiling(float z);      /* inside the grid, points above the ceiling block light;
                                                   outside the grid nothing does */
LIGHT_API void light_invalidate(void);          /* surface positions, normals or occluders have changed */

/* ---- Lights (index l, 0 ≤ l < LIGHT_MAX) ---- */
LIGHT_API void light_set_spectrum(int l, const float *spd);
/* Point light with luminous intensity I; illuminance E = I·cos θ / d². */
LIGHT_API void light_point(int l, float x, float y, float z, float intensity);
/* Directional light. (dx, dy, dz) points towards the light and need not be normalized; E⊥ is the
 * illuminance on a plane perpendicular to the rays, so E = E⊥·cos θ. */
LIGHT_API void light_sun(int l, float dx, float dy, float dz, float illuminance);
/* Area light: the parallelogram with centre c and half-edges u and v, emitting as a Lambertian
 * source of luminance L towards the side of u × v. E = L ∫ cos θ cos θ' / d² dA, estimated by
 * stratified sampling. A window may be treated as such a patch of uniformly bright sky. */
LIGHT_API void light_area(int l, float cx, float cy, float cz, float ux, float uy, float uz,
                          float vx, float vy, float vz, float luminance);
LIGHT_API void light_off(int l);

/* ---- Ambient light ----
 * Light that reaches surfaces after repeated diffuse reflection in the room, estimated as in an
 * integrating sphere, from room averages (the ambient term of Cohen et al. 1988 rests on the same
 * relation). For each light
 *   E_amb = k · Ē · ρ̄ / (1 − ρ̄)
 * where Ē is the mean direct illuminance from that light over the visible surfaces and ρ̄ their mean
 * luminance reflectance under it. E_amb is the same for every surface, ignores occlusion and has the
 * spectrum of the light (colour shifts from interreflection are ignored). k is a global factor,
 * 1 by default; k = 0 disables ambient light. */
LIGHT_API void light_set_ambient(float k);

/* ---- Materials and output ---- */
LIGHT_API void light_set_reflectance(int row, const float *refl);  /* sets a row; reflectances 0–1 */
LIGHT_API void light_set_exposure(float k);                        /* linear factor, not EV */
LIGHT_API const unsigned char *light_render(void);                 /* RGBA, LIGHT_W × LIGHT_H, row major */
/* Incident and reflected spectra at pixel (sx, sy), LIGHT_NW values each, in units of illuminance,
 * before exposure. All zeros outside the image or where there is no surface. */
LIGHT_API const float *light_probe(int sx, int sy);
LIGHT_API int light_row_at(int sx, int sy);                        /* -1 outside the image */

/* ---- Colour utilities ---- */
/* Colour of a light itself: linear sRGB scaled so that the largest channel is 1, then encoded; no
 * tone mapping. Returns 3 bytes. */
LIGHT_API const unsigned char *light_source_rgb(const float *spd);
/* Displayed colour of a surface with reflectance refl under illuminance e from a light with spectrum
 * spd: tone mapped, with an exposure of 1. Returns 3 bytes. */
LIGHT_API const unsigned char *light_surface_rgb(const float *spd, const float *refl, float e);
/* LIGHT_SCRATCH × LIGHT_NW floats through which a host (e.g. JavaScript) can pass spectra. */
LIGHT_API float *light_scratch(void);

/* ---- Compile-time constants, for hosts ---- */
LIGHT_API int light_width(void);                                   /* LIGHT_W */
LIGHT_API int light_height(void);                                  /* LIGHT_H */
LIGHT_API int light_spectrum_size(void);                           /* LIGHT_NW */
LIGHT_API int light_max(void);                                     /* LIGHT_MAX */

#endif

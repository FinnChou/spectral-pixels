/*
 * test_spl.c - unit tests of the spectral-pixels engine
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * Each test sets up a small scene, usually a flat floor, and compares what the engine computes with
 * the physics it is meant to implement. Illuminances are read back with spl_probe(): the incident
 * spectrum is E times the normalized light spectrum, so the ratio of two incident spectra at the
 * same wavelength is the ratio of the illuminances.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spl.h"

static int failures;
static int checks;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                                          \
    } while (0)

#define CHECK_NEAR(a, b, rel)                                                                      \
    do {                                                                                           \
        double a_ = (a);                                                                           \
        double b_ = (b);                                                                           \
        checks++;                                                                                  \
        if (!(fabs(a_ - b_) <= (rel)*fabs(b_))) {                                                  \
            failures++;                                                                            \
            fprintf(stderr, "%s:%d: %s = %.9g, expected %.9g\n", __FILE__, __LINE__, #a, a_, b_);  \
        }                                                                                          \
    } while (0)

enum { W = 32, H = 24, ROWS = 2, LIGHTS = 4, MID = 30 };

static float flat_spectrum[SPL_NW];
static float grey[SPL_NW];
static float black[SPL_NW];

static void fill(float *s, float v)
{
    for (int k = 0; k < SPL_NW; k++) {
        s[k] = v;
    }
}

/* An engine whose pixels all show a floor point at height 0, facing up, with reflectance row 0. */
static spl_engine *floor_engine(void)
{
    spl_config cfg = { W, H, ROWS, LIGHTS };
    spl_engine *e;
    if (spl_create(&cfg, &e) != SPL_OK) {
        fprintf(stderr, "cannot create an engine\n");
        exit(1);
    }
    spl_surface *s = spl_surfaces(e);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            spl_surface *p = &s[y * W + x];
            p->p[0] = x + 0.5f;
            p->p[1] = y + 0.5f;
            p->p[2] = 0;
            p->n[0] = 0;
            p->n[1] = 0;
            p->n[2] = 1;
            p->row = 0;
            p->tex = 1;
        }
    }
    spl_invalidate(e);
    spl_set_reflectance(e, 0, grey);
    spl_set_ambient(e, 0);
    return e;
}

/* Incident illuminance at a pixel relative to a reference value, read at one wavelength. */
static double incident(spl_engine *e, int x, int y)
{
    float in[SPL_NW];
    spl_probe(e, x, y, in, NULL);
    return (double)in[MID];
}

static void test_version_and_create(void)
{
    char expected[32];
    snprintf(expected, sizeof expected, "%d.%d.%d", SPL_VERSION_MAJOR, SPL_VERSION_MINOR,
             SPL_VERSION_PATCH);
    CHECK(strcmp(spl_version(), expected) == 0);

    spl_config bad[] = {
        { 0, 1, 1, 1 },
        { 1, 0, 1, 1 },
        { 1, 1, 0, 1 },
        { 1, 1, 1, 0 },
        { -4, 4, 1, 1 },
        { SPL_MAX_SIZE + 1, 1, 1, 1 },
        { SPL_MAX_SIZE, SPL_MAX_SIZE, 1, 1 }, /* more than INT_MAX pixels */
        { 1, 1, SPL_MAX_ROWS + 1, 1 },
        { 1, 1, 1, SPL_MAX_LIGHTS + 1 },
    };
    spl_engine *e = (spl_engine *)&e; /* any non-NULL value */
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(spl_create(&bad[i], &e) == SPL_ERR_ARGUMENT);
        CHECK(e == NULL);
    }
    CHECK(spl_create(NULL, &e) == SPL_ERR_ARGUMENT);
    spl_config cfg = { 3, 2, 5, 7 };
    CHECK(spl_create(&cfg, NULL) == SPL_ERR_ARGUMENT);

    CHECK(spl_create(&cfg, &e) == SPL_OK);
    CHECK(e != NULL);
    spl_config got = spl_get_config(e);
    CHECK(got.width == 3 && got.height == 2 && got.rows == 5 && got.lights == 7);
    /* A new engine shows no surfaces: every pixel is transparent. */
    uint8_t rgba[3 * 2 * 4];
    memset(rgba, 7, sizeof rgba);
    CHECK(spl_render(e, rgba) == SPL_OK);
    for (size_t i = 0; i < sizeof rgba; i++) {
        CHECK(rgba[i] == 0);
    }
    spl_destroy(e);
    spl_destroy(NULL);
}

static void test_arguments(void)
{
    spl_engine *e = floor_engine();
    float nan_spd[SPL_NW];
    fill(nan_spd, 1);
    nan_spd[3] = NAN;
    float negative[SPL_NW];
    fill(negative, 0.5f);
    negative[10] = -0.1f;
    float c[3] = { 1, 1, 1 };
    float u[3] = { 1, 0, 0 };
    float parallel[3] = { 2, 0, 0 };

    CHECK(spl_light_set_spectrum(e, -1, flat_spectrum) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_spectrum(e, LIGHTS, flat_spectrum) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_spectrum(e, 0, NULL) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_spectrum(e, 0, nan_spd) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_spectrum(e, 0, negative) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_spectrum(e, 0, black) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_point(e, 0, 1, 1, NAN, 1) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_point(e, 0, 1, 1, 1, -1) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_directional(e, 0, 0, 0, 0, 1) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_area(e, 0, c, u, parallel, 1) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_area(e, 0, c, u, NULL, 1) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_disable(e, LIGHTS) == SPL_ERR_ARGUMENT);
    CHECK(spl_set_reflectance(e, ROWS, grey) == SPL_ERR_ARGUMENT);
    CHECK(spl_set_reflectance(e, 0, negative) == SPL_ERR_ARGUMENT);
    CHECK(spl_set_ambient(e, -1) == SPL_ERR_ARGUMENT);
    CHECK(spl_set_exposure(e, INFINITY) == SPL_ERR_ARGUMENT);
    CHECK(spl_set_ceiling(e, NAN) == SPL_ERR_ARGUMENT);
    CHECK(spl_set_ceiling(e, INFINITY) == SPL_OK);
    CHECK(spl_render(e, NULL) == SPL_ERR_ARGUMENT);
    CHECK(spl_probe(e, W, 0, NULL, NULL) == SPL_ERR_ARGUMENT);
    CHECK(spl_probe(e, 0, 0, NULL, NULL) == SPL_OK);

    spl_destroy(e);
}

/* A rejected call leaves the engine as it was: the light keeps its spectrum and shape. */
static void test_rejected_calls(void)
{
    spl_engine *e = floor_engine();
    float red[SPL_NW];
    fill(red, 0);
    for (int k = 50; k < SPL_NW; k++) {
        red[k] = 1;
    }
    float c[3] = { 16, 12, 8 };
    float u[3] = { 2, 0, 0 };
    float v[3] = { 0, -1, 0 };
    float zero[3] = { 0, 0, 0 };
    float out[SPL_NW];
    float again[SPL_NW];

    CHECK(spl_light_set_spectrum(e, 0, red) == SPL_OK);
    CHECK(spl_light_set_point(e, 0, 16, 12, 8, 100) == SPL_OK);
    spl_probe(e, 3, 3, NULL, out);
    CHECK(spl_light_set_spectrum(e, 0, black) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_point(e, 0, 16, 12, NAN, 100) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_directional(e, 0, 0, 0, 0, 1) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_directional(e, 0, 1e-30f, 0, 0, 1) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_set_area(e, 0, c, u, zero, 1) == SPL_ERR_ARGUMENT);
    spl_probe(e, 3, 3, NULL, again);
    CHECK(memcmp(out, again, sizeof out) == 0);

    CHECK(spl_light_set_area(e, 0, c, u, v, 10) == SPL_OK);
    spl_probe(e, 3, 3, NULL, out);
    CHECK(spl_light_set_area(e, 0, c, u, zero, 10) == SPL_ERR_ARGUMENT);
    spl_probe(e, 3, 3, NULL, again);
    CHECK(memcmp(out, again, sizeof out) == 0);
    spl_destroy(e);
}

/* The cached geometry follows changes of shape, and a light switched off and on again is the same
 * light. */
static void test_cache(void)
{
    spl_engine *e = floor_engine();
    float c[3] = { 16, 12, 8 };
    float u[3] = { 2, 0, 0 };
    float wide[3] = { 6, 0, 0 };
    float v[3] = { 0, -1, 0 };
    spl_light_set_spectrum(e, 0, flat_spectrum);
    spl_light_set_area(e, 0, c, u, v, 10);
    double small = incident(e, 16, 12);
    spl_light_set_area(e, 0, c, wide, v, 10);
    double large = incident(e, 16, 12);
    CHECK(large > small);
    spl_light_set_area(e, 0, c, u, v, 10);
    CHECK(incident(e, 16, 12) == small);

    spl_light_disable(e, 0);
    CHECK(incident(e, 16, 12) == 0);
    spl_light_set_area(e, 0, c, u, v, 10);
    CHECK(incident(e, 16, 12) == small);
    spl_destroy(e);
}

/* Pixels without a surface (row -1, or a row outside the table) are transparent, and probing them
 * gives zeros. */
static void test_no_surface(void)
{
    spl_engine *e = floor_engine();
    spl_surface *s = spl_surfaces(e);
    s[0].row = -1;
    s[1].row = ROWS;
    spl_light_set_spectrum(e, 0, flat_spectrum);
    spl_light_set_directional(e, 0, 0, 0, 1, 10);
    uint8_t rgba[W * H * 4];
    CHECK(spl_render(e, rgba) == SPL_OK);
    for (int k = 0; k < 8; k++) {
        CHECK(rgba[k] == 0);
    }
    CHECK(rgba[8 + 3] == 255);
    float in[SPL_NW];
    fill(in, 1);
    CHECK(spl_probe(e, 1, 0, in, NULL) == SPL_OK);
    CHECK(in[MID] == 0);
    spl_destroy(e);
}

/* Extreme but finite inputs do not crash and do not turn bright pixels black. (Run under the
 * undefined-behaviour sanitizer by `make sanitize`.) */
static void test_extremes(void)
{
    spl_engine *e = floor_engine();
    spl_light_set_spectrum(e, 0, flat_spectrum);
    spl_light_set_directional(e, 0, 0, 0, 1, 1e20f);
    uint8_t rgba[W * H * 4];
    CHECK(spl_render(e, rgba) == SPL_OK);
    CHECK(rgba[0] == 255 && rgba[1] == 255 && rgba[2] == 255);
    uint8_t rgb[3];
    CHECK(spl_surface_rgb(flat_spectrum, grey, 1e30f, rgb) == SPL_OK);
    CHECK(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);

    /* A light billions of pixels away, and a surface point that is not finite. */
    spl_light_set_point(e, 1, 3e9f, 12, 8, 1);
    spl_light_set_spectrum(e, 1, flat_spectrum);
    spl_surfaces(e)[5].p[0] = NAN;
    spl_invalidate(e);
    CHECK(spl_render(e, rgba) == SPL_OK);
    spl_destroy(e);
}

/* E = I cos θ / d²: compare two floor points under a point light at height h. */
static void test_point_light(void)
{
    spl_engine *e = floor_engine();
    const float h = 8;
    spl_light_set_spectrum(e, 0, flat_spectrum);
    spl_light_set_point(e, 0, 16.5f, 12.5f, h, 100);
    double below = incident(e, 16, 12);
    for (int dx = 1; dx <= 12; dx += 5) {
        double r2 = (double)dx * dx;
        double h2 = (double)h * (double)h;
        double expected = ((double)h / sqrt(r2 + h2)) / (r2 + h2) * h2;
        CHECK_NEAR(incident(e, 16 + dx, 12) / below, expected, 1e-5);
    }
    /* Doubling the intensity doubles the illuminance without recomputing the geometry. */
    spl_light_set_point(e, 0, 16.5f, 12.5f, h, 200);
    CHECK_NEAR(incident(e, 16, 12), 2 * below, 1e-6);
    /* A light below the floor does not light it. */
    spl_light_set_point(e, 0, 16.5f, 12.5f, -1, 100);
    CHECK(incident(e, 16, 12) == 0);
    spl_destroy(e);
}

/* Occluders: a wall between the light and a point blocks it; a gap in the wall lets light
 * through. */
static void test_occlusion(void)
{
    spl_engine *e = floor_engine();
    spl_light_set_spectrum(e, 0, flat_spectrum);
    spl_light_set_point(e, 0, 4.5f, 12.5f, 4, 100);
    double open = incident(e, 28, 12);
    CHECK(open > 0);

    float *heights = spl_heights(e);
    for (int y = 0; y < H; y++) {
        heights[y * W + 16] = 10;
    }
    spl_invalidate(e);
    CHECK(incident(e, 28, 12) == 0);
    CHECK(incident(e, 8, 12) > 0);

    /* The ray from the light at z = 4 to the floor at x = 28.5 crosses x = 16 at z = 4 · 12.5 / 24
     * ≈ 2.1. */
    float *gaps = spl_gaps(e);
    gaps[2 * (12 * W + 16)] = 1;
    gaps[2 * (12 * W + 16) + 1] = 3;
    spl_invalidate(e);
    CHECK_NEAR(incident(e, 28, 12), open, 1e-6);

    /* A ceiling below the light blocks everything inside the grid. */
    spl_set_ceiling(e, 2);
    CHECK(incident(e, 8, 12) == 0);
    spl_destroy(e);
}

/* E = E⊥ cos θ for a directional light. */
static void test_directional_light(void)
{
    spl_engine *e = floor_engine();
    spl_surface *s = spl_surfaces(e);
    const float d[3] = { 0.6f, 0, 0.8f };
    s[5 * W + 5].n[0] = 0.6f;
    s[5 * W + 5].n[2] = 0.8f;
    spl_invalidate(e);
    spl_light_set_spectrum(e, 0, flat_spectrum);
    spl_light_set_directional(e, 0, d[0] * 3, d[1] * 3, d[2] * 3, 50);
    double facing = incident(e, 5, 5);
    CHECK_NEAR(incident(e, 6, 5) / facing, 0.8, 1e-6);
    spl_destroy(e);
}

/* A small area light seen from far away acts like a point light of intensity L · A · cos θ'. */
static void test_area_light(void)
{
    spl_engine *e = floor_engine();
    const float lum = 1000;
    const float half = 0.05f;
    float center[3] = { 16.5f, 12.5f, 10 };
    float u[3] = { half, 0, 0 };
    float v[3] = { 0, -half, 0 }; /* u × v points down, towards the floor */
    spl_light_set_spectrum(e, 0, flat_spectrum);
    spl_light_set_spectrum(e, 1, flat_spectrum);
    CHECK(spl_light_set_area(e, 0, center, u, v, lum) == SPL_OK);
    double area = 4 * (double)half * (double)half;
    for (int dx = 0; dx <= 8; dx += 4) {
        spl_light_disable(e, 1);
        double from_area = incident(e, 16 + dx, 12);
        /* Point light of the same intensity in this direction: I = L A cos θ', with
         * cos θ' = cos θ. */
        double d2 = (double)dx * dx + 100;
        double cos_t = 10 / sqrt(d2);
        spl_light_disable(e, 0);
        spl_light_set_point(e, 1, center[0], center[1], center[2],
                            (float)((double)lum * area * cos_t));
        double from_point = incident(e, 16 + dx, 12);
        spl_light_set_area(e, 0, center, u, v, lum);
        CHECK_NEAR(from_area, from_point, 1e-3);
    }
    spl_destroy(e);
}

/* Ambient light: under a uniform light, E_amb = Ē ρ̄ / (1 − ρ̄), so with ρ = 0.5 the incident light
 * doubles. */
static void test_ambient(void)
{
    spl_engine *e = floor_engine();
    spl_light_set_spectrum(e, 0, flat_spectrum);
    spl_light_set_directional(e, 0, 0, 0, 1, 40);
    double direct = incident(e, 10, 10);
    spl_set_ambient(e, 1);
    CHECK_NEAR(incident(e, 10, 10), 2 * direct, 1e-5);
    spl_set_ambient(e, 0.5f);
    CHECK_NEAR(incident(e, 10, 10), 1.5 * direct, 1e-5);
    spl_destroy(e);
}

/* Output: exposure 0 gives black; the reflected spectrum is the incident one times the
 * reflectance. */
static void test_output(void)
{
    spl_engine *e = floor_engine();
    spl_light_set_spectrum(e, 0, flat_spectrum);
    spl_light_set_directional(e, 0, 0, 0, 1, 1);
    uint8_t rgba[W * H * 4];
    spl_set_exposure(e, 0);
    spl_render(e, rgba);
    CHECK(rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);

    spl_set_exposure(e, 1);
    spl_render(e, rgba);
    uint8_t again[W * H * 4];
    spl_render(e, again);
    CHECK(memcmp(rgba, again, sizeof rgba) == 0);
    CHECK(rgba[0] > 0);

    float in[SPL_NW];
    float out[SPL_NW];
    spl_probe(e, 2, 2, in, out);
    CHECK_NEAR((double)out[MID], 0.5 * (double)in[MID], 1e-6);

    uint8_t rgb[3];
    CHECK(spl_light_rgb(flat_spectrum, rgb) == SPL_OK);
    CHECK(rgb[0] == 255 || rgb[1] == 255 || rgb[2] == 255);
    CHECK(spl_surface_rgb(flat_spectrum, grey, 0, rgb) == SPL_OK);
    CHECK(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0);
    CHECK(spl_surface_rgb(flat_spectrum, grey, -1, rgb) == SPL_ERR_ARGUMENT);
    CHECK(spl_light_rgb(black, rgb) == SPL_ERR_ARGUMENT);
    CHECK(spl_surface_rgb(black, grey, 1, rgb) == SPL_ERR_ARGUMENT);
    spl_destroy(e);
}

/* Two engines do not share any state. */
static void test_independent_engines(void)
{
    spl_engine *a = floor_engine();
    spl_engine *b = floor_engine();
    spl_light_set_spectrum(a, 0, flat_spectrum);
    spl_light_set_point(a, 0, 10, 10, 5, 100);
    uint8_t first[W * H * 4];
    uint8_t second[W * H * 4];
    spl_render(a, first);
    spl_light_set_spectrum(b, 0, flat_spectrum);
    spl_light_set_point(b, 0, 20, 5, 3, 900);
    spl_set_exposure(b, 3);
    spl_render(b, second);
    spl_render(a, second);
    CHECK(memcmp(first, second, sizeof first) == 0);
    spl_destroy(a);
    spl_destroy(b);
}

int main(void)
{
    fill(flat_spectrum, 1);
    fill(grey, 0.5f);
    fill(black, 0);
    test_version_and_create();
    test_arguments();
    test_rejected_calls();
    test_cache();
    test_no_surface();
    test_extremes();
    test_point_light();
    test_occlusion();
    test_directional_light();
    test_area_light();
    test_ambient();
    test_output();
    test_independent_engines();
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}

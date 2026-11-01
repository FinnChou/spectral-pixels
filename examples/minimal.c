/*
 * minimal.c - the smallest complete use of spectral-pixels
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * An equal-energy point light above a 50 % grey floor, seen from straight above, with a block 16
 * pixels tall standing on it. Writes the image as a PPM file (default minimal.ppm).
 *
 *   cc -std=c11 -Iinclude -Isrc examples/minimal.c src/spl.c -lm -o minimal && ./minimal
 */
#include <stdio.h>
#include <stdlib.h>

#include "spl.h"

enum { W = 256, H = 160 };

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "minimal.ppm";
    spl_config config = { W, H, 1, 1 }; /* one reflectance row, one light */
    spl_engine *engine;
    spl_status st = spl_create(&config, &engine);
    if (st != SPL_OK) {
        fprintf(stderr, "spectral-pixels: error %d\n", (int)st);
        return 1;
    }

    float light[SPL_NW];
    float grey[SPL_NW];
    for (int k = 0; k < SPL_NW; k++) {
        light[k] = 1;   /* equal energy */
        grey[k] = 0.5f; /* 50 % reflectance at every wavelength */
    }

    /* Scene: every pixel sees the top of whatever stands there: the floor, or a block 16 pixels
     * tall. */
    spl_surface *surfaces = spl_surfaces(engine);
    float *heights = spl_heights(engine);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            int block = x >= 120 && x < 136 && y >= 72 && y < 88;
            heights[i] = block ? 16 : 0;
            spl_surface s = { { x + 0.5f, y + 0.5f, heights[i] }, { 0, 0, 1 }, 0, 1 };
            surfaces[i] = s;
        }
    }
    spl_invalidate(engine);

    st = spl_set_reflectance(engine, 0, grey);
    if (st == SPL_OK) {
        st = spl_light_set_spectrum(engine, 0, light);
    }
    if (st == SPL_OK) {
        st = spl_light_set_point(engine, 0, 96, 80, 24, 2000);
    }
    static uint8_t rgba[W * H * 4];
    if (st == SPL_OK) {
        st = spl_render(engine, rgba);
    }
    spl_destroy(engine);
    if (st != SPL_OK) {
        fprintf(stderr, "spectral-pixels: error %d\n", (int)st);
        return 1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        fwrite(&rgba[4 * i], 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
    return 0;
}

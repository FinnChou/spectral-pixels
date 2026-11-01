/*
 * render.c - command-line renderer for the demo room
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * Reads the spectral data, sets up the room and lights from the command line, renders one frame
 * and writes it as an image. Used to produce stills natively and, by check.mjs, as the reference
 * against which the WebAssembly build is compared.
 *
 * Usage: render <illuminants> <reflectances> <output> settings...
 *   An output name ending in .pam is written as RGBA (PAM), keeping the window opening
 *   transparent; anything else is written as RGB (PPM).
 *   Settings have the form key=value:
 *     floor= wall= pillar= crate= barrel= frame=
 *         body id,joint id,style (joint id - means the same as the body). All six parts are
 *         required; styles are listed in demo/scene.h.
 *     light<N>=point,spectrum id,x,y,z,intensity
 *     light<N>=sun,spectrum id,dx,dy,dz,illuminance        directional light
 *     light<N>=area,spectrum id,cx,cy,cz,ux,uy,uz,vx,vy,vz,luminance
 *     light<N>=window,spectrum id,luminance                the window opening as an area light
 *     window=0|1   ambient=factor   exposure=linear factor   scale=magnification (integer 1-16)
 *     dot=x,y      draws a marker at that floor point
 *     probe=x,y    prints the incident and reflected spectra at that pixel to standard output,
 *                  one line of 2 × SPL_NW numbers; may be repeated, up to 8 times
 *   Spectrum and reflectance ids are those of the data files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scene.h"
#include "spl.h"

enum { MAX_SPECTRA = 64, MAX_PROBES = 8, MAX_LIGHTS = 8 };

typedef struct {
    char id[32];
    float v[SPL_NW];
} spectrum;

static spectrum spectra[MAX_SPECTRA];
static int spectrum_count;

static void fail(const char *msg, const char *detail)
{
    fprintf(stderr, "render: %s%s\n", msg, detail);
    exit(1);
}

static void check(spl_status st, const char *what)
{
    if (st == SPL_ERR_MEMORY) {
        fail("out of memory: ", what);
    }
    if (st != SPL_OK) {
        fail("invalid value: ", what);
    }
}

/* Data file: lines starting with # are comments; every other line holds an id, the English, Chinese
 * and Japanese names and SPL_NW values, separated by tabs. */
static void load(const char *path)
{
    static char line[8192];
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(1);
    }
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n') && !feof(f)) {
            fail("line too long in ", path);
        }
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (spectrum_count == MAX_SPECTRA) {
            fail("too many spectra in ", path);
        }
        char *id = strtok(line, "\t");
        for (int k = 0; k < 3; k++) {
            if (!strtok(NULL, "\t")) {
                fail("malformed line in ", path);
            }
        }
        if (!id) {
            fail("malformed line in ", path);
        }
        spectrum *s = &spectra[spectrum_count];
        snprintf(s->id, sizeof s->id, "%s", id);
        for (int k = 0; k < SPL_NW; k++) {
            char *tok = strtok(NULL, "\t\n");
            char *end;
            if (!tok) {
                fail("too few values for ", id);
            }
            s->v[k] = strtof(tok, &end);
            if (end == tok) {
                fail("value is not a number for ", id);
            }
        }
        if (strtok(NULL, "\t\n")) {
            fail("too many values for ", id);
        }
        spectrum_count++;
    }
    fclose(f);
}

static const float *find(const char *id)
{
    for (int i = 0; i < spectrum_count; i++) {
        if (strcmp(spectra[i].id, id) == 0) {
            return spectra[i].v;
        }
    }
    fail("no spectrum with id ", id);
    return NULL;
}

/* An integer, and nothing else. */
static int parse_int(const char *s, const char *arg)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < -1000000 || v > 1000000) {
        fail("malformed integer: ", arg);
    }
    return (int)v;
}

/* Exactly n comma-separated numbers. */
static void parse_numbers(const char *s, float *out, int n, const char *arg)
{
    for (int k = 0; k < n; k++) {
        char *end;
        out[k] = strtof(s, &end);
        char expected = k < n - 1 ? ',' : '\0';
        if (end == s || *end != expected) {
            fail("malformed numbers: ", arg);
        }
        s = end + 1;
    }
}

static void parse_light(spl_engine *engine, scene *room, int l, const char *value, const char *arg)
{
    char kind[8];
    char id[32];
    int used;
    if (sscanf(value, "%7[^,],%31[^,],%n", kind, id, &used) != 2) {
        fail("malformed light: ", arg);
    }
    float v[10];
    check(spl_light_set_spectrum(engine, l, find(id)), arg);
    if (strcmp(kind, "point") == 0) {
        parse_numbers(value + used, v, 4, arg);
        check(spl_light_set_point(engine, l, v[0], v[1], v[2], v[3]), arg);
    } else if (strcmp(kind, "sun") == 0) {
        parse_numbers(value + used, v, 4, arg);
        check(spl_light_set_directional(engine, l, v[0], v[1], v[2], v[3]), arg);
    } else if (strcmp(kind, "area") == 0) {
        parse_numbers(value + used, v, 10, arg);
        check(spl_light_set_area(engine, l, &v[0], &v[3], &v[6], v[9]), arg);
    } else if (strcmp(kind, "window") == 0) {
        parse_numbers(value + used, v, 1, arg);
        check(scene_window_light(room, l, v[0]), arg);
    } else {
        fail("unknown kind of light: ", arg);
    }
}

/* Writes the frame, magnified by scale, with an optional marker at a floor point. */
static void write_image(const char *path, const uint8_t *img, int scale, const float *dot)
{
    size_t len = strlen(path);
    int rgba = len > 4 && strcmp(path + len - 4, ".pam") == 0;
    int w = SCENE_WIDTH * scale;
    int h = SCENE_HEIGHT * scale;
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    if (rgba) {
        fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w,
                h);
    } else {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
    }
    static const uint8_t rim[4] = { 0, 0, 0, 255 };
    static const uint8_t core[4] = { 255, 236, 200, 255 };
    float r = 2.5f * scale; /* marker radius: 2.5 scene pixels */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t *c = img + 4 * ((y / scale) * SCENE_WIDTH + x / scale);
            if (dot) {
                float dx = x + 0.5f - dot[0] * scale;
                float dy = y + 0.5f - dot[1] * scale;
                float d2 = dx * dx + dy * dy;
                if (d2 < r * r) {
                    c = d2 < (r - scale) * (r - scale) ? core : rim;
                }
            }
            fwrite(c, 1, rgba ? 4 : 3, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static const char *const part_key[PART_COUNT] = { "floor", "wall",   "pillar",
                                                      "crate", "barrel", "frame" };
    if (argc < 4) {
        fprintf(stderr, "usage: %s <illuminants> <reflectances> <output> settings...\n", argv[0]);
        return 1;
    }
    load(argv[1]);
    load(argv[2]);

    spl_config config = { SCENE_WIDTH, SCENE_HEIGHT, SCENE_ROWS, MAX_LIGHTS };
    spl_engine *engine;
    check(spl_create(&config, &engine), "engine configuration");
    scene *room = scene_create(engine);
    if (!room) {
        fail("out of memory", "");
    }

    int part_given[PART_COUNT] = { 0 };
    int scale = 1;
    int has_dot = 0;
    int probe_count = 0;
    int window = 1;
    float dot[2];
    float probe[MAX_PROBES][2];
    float ambient = 1;
    float exposure = 1;
    for (int i = 4; i < argc; i++) {
        const char *arg = argv[i];
        const char *eq = strchr(arg, '=');
        if (!eq) {
            fail("settings must have the form key=value: ", arg);
        }
        const char *value = eq + 1;
        size_t key_len = (size_t)(eq - arg);
        int handled = 0;
        for (int p = 0; p < PART_COUNT && !handled; p++) {
            if (strlen(part_key[p]) != key_len || strncmp(arg, part_key[p], key_len) != 0) {
                continue;
            }
            char body_id[32];
            char joint_id[32];
            int style;
            int used = 0;
            if (sscanf(value, "%31[^,],%31[^,],%d%n", body_id, joint_id, &style, &used) != 3 ||
                value[used] != '\0') {
                fail("a part must be given as body,joint,style: ", arg);
            }
            const float *joint = strcmp(joint_id, "-") == 0 ? NULL : find(joint_id);
            check(scene_set_material(room, p, style, find(body_id), joint), arg);
            part_given[p] = 1;
            handled = 1;
        }
        if (handled) {
            continue;
        }
        int l;
        if (sscanf(arg, "light%d=", &l) == 1) {
            if (l < 0 || l >= MAX_LIGHTS) {
                fail("light index out of range: ", arg);
            }
            parse_light(engine, room, l, value, arg);
        } else if (strncmp(arg, "window=", 7) == 0) {
            window = parse_int(value, arg);
            if (window != 0 && window != 1) {
                fail("window must be 0 or 1: ", arg);
            }
        } else if (strncmp(arg, "ambient=", 8) == 0) {
            parse_numbers(value, &ambient, 1, arg);
        } else if (strncmp(arg, "exposure=", 9) == 0) {
            parse_numbers(value, &exposure, 1, arg);
        } else if (strncmp(arg, "scale=", 6) == 0) {
            scale = parse_int(value, arg);
            if (scale < 1 || scale > 16) {
                fail("scale must be 1-16: ", arg);
            }
        } else if (strncmp(arg, "dot=", 4) == 0) {
            parse_numbers(value, dot, 2, arg);
            has_dot = 1;
        } else if (strncmp(arg, "probe=", 6) == 0) {
            if (probe_count == MAX_PROBES) {
                fail("at most 8 probes: ", arg);
            }
            parse_numbers(value, probe[probe_count], 2, arg);
            if (!(probe[probe_count][0] >= 0 && probe[probe_count][0] < SCENE_WIDTH &&
                  probe[probe_count][1] >= 0 && probe[probe_count][1] < SCENE_HEIGHT)) {
                fail("probe outside the image: ", arg);
            }
            probe_count++;
        } else {
            fail("unknown setting: ", arg);
        }
    }
    for (int p = 0; p < PART_COUNT; p++) {
        if (!part_given[p]) {
            fail("missing part: ", part_key[p]);
        }
    }

    scene_set_window(room, window);
    scene_build(room);
    check(spl_set_ambient(engine, ambient), "ambient");
    check(spl_set_exposure(engine, exposure), "exposure");
    static uint8_t img[SCENE_WIDTH * SCENE_HEIGHT * 4];
    check(spl_render(engine, img), "render");

    for (int j = 0; j < probe_count; j++) {
        float spectra_out[2][SPL_NW];
        check(spl_probe(engine, (int)probe[j][0], (int)probe[j][1], spectra_out[0], spectra_out[1]),
              "probe");
        for (int k = 0; k < 2 * SPL_NW; k++) {
            printf("%.9g%c", (double)spectra_out[k / SPL_NW][k % SPL_NW],
                   k + 1 < 2 * SPL_NW ? ' ' : '\n');
        }
    }

    write_image(argv[3], img, scale, has_dot ? dot : NULL);
    scene_destroy(room);
    spl_destroy(engine);
    return 0;
}

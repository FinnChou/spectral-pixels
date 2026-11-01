/*
 * render.c — command-line renderer
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 FinnZhou
 *
 * Reads the spectral data, sets up the scene and lights from the command line, renders one frame and
 * writes it as an image. Used to produce stills natively and, by check.mjs, as the reference against
 * which the WebAssembly build is compared.
 *
 * Usage: render <illuminants> <reflectances> <output> settings…
 *   An output name ending in .pam is written as RGBA (PAM), keeping the window opening transparent;
 *   anything else is written as RGB (PPM).
 *   Settings have the form key=value:
 *     floor= wall= pillar= crate= barrel= frame=   body id,joint id,style (joint id - means same as body).
 *                                                  All six parts are required; styles are listed in scene.h.
 *     light<N>=point,spectrum id,x,y,z,intensity
 *     light<N>=sun,spectrum id,dx,dy,dz,illuminance
 *     light<N>=area,spectrum id,cx,cy,cz,ux,uy,uz,vx,vy,vz,luminance
 *     light<N>=window,spectrum id,luminance        the window opening as an area light; see scene_window_light()
 *     window=0|1     ambient=factor     exposure=linear factor     scale=magnification (integer)
 *     dot=x,y        draws a marker at that floor point
 *     probe=x,y      prints the incident and reflected spectra at that pixel to standard output (one line of
 *                    2 × LIGHT_NW numbers); may be repeated, up to 8 times
 *   Spectrum and reflectance ids are those of the data files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scene.h"

enum { MAX_SPECTRA = 64, MAX_PROBES = 8 };

static struct { char id[32]; float v[LIGHT_NW]; } spectra[MAX_SPECTRA];
static int spectrum_count;

static void fail(const char *msg, const char *detail) {
  fprintf(stderr, "render: %s%s\n", msg, detail);
  exit(1);
}

/* Data file: lines starting with # are comments; every other line holds an id, the English, Chinese and
 * Japanese names and LIGHT_NW values, separated by tabs. */
static void load(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); exit(1); }
  static char line[8192];
  while (fgets(line, sizeof line, f)) {
    if (!strchr(line, '\n') && !feof(f)) fail("line too long in ", path);
    if (line[0] == '#' || line[0] == '\n') continue;
    if (spectrum_count == MAX_SPECTRA) fail("too many spectra in ", path);
    char *id = strtok(line, "\t");
    for (int k = 0; k < 3; k++)                   /* skip the three names */
      if (!strtok(NULL, "\t")) fail("malformed line in ", path);
    if (!id) fail("malformed line in ", path);
    snprintf(spectra[spectrum_count].id, sizeof spectra[0].id, "%s", id);
    for (int k = 0; k < LIGHT_NW; k++) {
      char *tok = strtok(NULL, "\t\n"), *end;
      if (!tok) fail("too few values for ", id);
      spectra[spectrum_count].v[k] = strtof(tok, &end);
      if (end == tok) fail("value is not a number for ", id);
    }
    if (strtok(NULL, "\t\n")) fail("too many values for ", id);
    spectrum_count++;
  }
  fclose(f);
}

static const float *find(const char *id) {
  for (int i = 0; i < spectrum_count; i++)
    if (strcmp(spectra[i].id, id) == 0) return spectra[i].v;
  fail("no spectrum with id ", id);
  return 0;
}

/* An integer, and nothing else. */
static int parse_int(const char *s, const char *arg) {
  char *end;
  long v = strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < -1000000 || v > 1000000) fail("malformed integer: ", arg);
  return (int)v;
}

/* Exactly n comma-separated numbers. */
static void parse_numbers(const char *s, float *out, int n, const char *arg) {
  for (int k = 0; k < n; k++) {
    char *end;
    out[k] = strtof(s, &end);
    if (end == s || (k < n - 1 ? *end != ',' : *end != '\0')) fail("malformed numbers: ", arg);
    s = end + 1;
  }
}

static void parse_light(int l, const char *value, const char *arg) {
  char kind[8], id[32];
  int used;
  if (sscanf(value, "%7[^,],%31[^,],%n", kind, id, &used) != 2) fail("malformed light: ", arg);
  float v[10];
  light_set_spectrum(l, find(id));
  if (strcmp(kind, "point") == 0) { parse_numbers(value + used, v, 4, arg); light_point(l, v[0], v[1], v[2], v[3]); }
  else if (strcmp(kind, "sun") == 0) { parse_numbers(value + used, v, 4, arg); light_sun(l, v[0], v[1], v[2], v[3]); }
  else if (strcmp(kind, "area") == 0) {
    parse_numbers(value + used, v, 10, arg);
    light_area(l, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
  }
  else if (strcmp(kind, "window") == 0) { parse_numbers(value + used, v, 1, arg); scene_window_light(l, v[0]); }
  else fail("unknown kind of light: ", arg);
}

int main(int argc, char **argv) {
  static const char *const part_key[PART_COUNT] = { "floor", "wall", "pillar", "crate", "barrel", "frame" };
  if (argc < 4) { fprintf(stderr, "usage: %s <illuminants> <reflectances> <output> settings…\n", argv[0]); return 1; }
  load(argv[1]);
  load(argv[2]);

  int part_given[PART_COUNT] = { 0 }, scale = 1, has_dot = 0, probe_count = 0, window = 1;
  float dot[2], probe[MAX_PROBES][2], ambient = 1, exposure = 1;
  for (int i = 4; i < argc; i++) {
    const char *arg = argv[i], *eq = strchr(arg, '=');
    if (!eq) fail("settings must have the form key=value: ", arg);
    const char *value = eq + 1;
    size_t key_len = (size_t)(eq - arg);
    int l, handled = 0;
    for (int p = 0; p < PART_COUNT && !handled; p++) {
      if (strlen(part_key[p]) != key_len || strncmp(arg, part_key[p], key_len) != 0) continue;
      char main_id[32], joint_id[32];
      int style, used = 0;
      if (sscanf(value, "%31[^,],%31[^,],%d%n", main_id, joint_id, &style, &used) != 3 || value[used] != '\0')
        fail("a part must be given as body,joint,style: ", arg);
      scene_set_material(p, style, find(main_id), strcmp(joint_id, "-") == 0 ? 0 : find(joint_id));
      part_given[p] = handled = 1;
    }
    if (handled) continue;
    if (sscanf(arg, "light%d=", &l) == 1) {
      if (l < 0 || l >= LIGHT_MAX) fail("light index out of range: ", arg);
      parse_light(l, value, arg);
    }
    else if (!strncmp(arg, "window=", 7)) { window = parse_int(value, arg); if (window != 0 && window != 1) fail("window must be 0 or 1: ", arg); }
    else if (!strncmp(arg, "ambient=", 8)) parse_numbers(value, &ambient, 1, arg);
    else if (!strncmp(arg, "exposure=", 9)) parse_numbers(value, &exposure, 1, arg);
    else if (!strncmp(arg, "scale=", 6)) { scale = parse_int(value, arg); if (scale < 1 || scale > 16) fail("scale must be 1–16: ", arg); }
    else if (!strncmp(arg, "dot=", 4)) { parse_numbers(value, dot, 2, arg); has_dot = 1; }
    else if (!strncmp(arg, "probe=", 6)) {
      if (probe_count == MAX_PROBES) fail("at most 8 probes: ", arg);
      parse_numbers(value, probe[probe_count++], 2, arg);
    }
    else fail("unknown setting: ", arg);
  }
  for (int p = 0; p < PART_COUNT; p++)
    if (!part_given[p]) fail("missing part: ", part_key[p]);

  scene_set_window(window);
  scene_build();
  light_set_ambient(ambient);
  light_set_exposure(exposure);
  const unsigned char *img = light_render();

  for (int j = 0; j < probe_count; j++) {
    const float *s = light_probe((int)probe[j][0], (int)probe[j][1]);
    for (int k = 0; k < 2 * LIGHT_NW; k++) printf("%.9g%c", s[k], k + 1 < 2 * LIGHT_NW ? ' ' : '\n');
  }

  const char *out = argv[3];
  size_t out_len = strlen(out);
  int rgba = out_len > 4 && strcmp(out + out_len - 4, ".pam") == 0;
  int w = LIGHT_W * scale, h = LIGHT_H * scale;
  FILE *f = fopen(out, "wb");
  if (!f) { perror(out); return 1; }
  if (rgba) fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
  else fprintf(f, "P6\n%d %d\n255\n", w, h);
  float r = 2.5f * scale;                       /* marker radius: 2.5 scene pixels */
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      static const unsigned char rim[4] = { 0, 0, 0, 255 }, core[4] = { 255, 236, 200, 255 };
      const unsigned char *c = img + 4 * ((y / scale) * LIGHT_W + x / scale);
      if (has_dot) {
        float dx = x + 0.5f - dot[0] * scale, dy = y + 0.5f - dot[1] * scale, d2 = dx * dx + dy * dy;
        if (d2 < r * r) c = d2 < (r - scale) * (r - scale) ? core : rim;
      }
      fwrite(c, 1, rgba ? 4 : 3, f);
    }
  fclose(f);
  return 0;
}
